#include "doc/pdf_loader_win.h"
#include <QDir>
#include <QFile>
#include <algorithm>

#ifdef Q_OS_WIN
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>
#endif

namespace jietuba::doc_pdf {

#ifdef Q_OS_WIN
namespace {
namespace win_pdf = winrt::Windows::Data::Pdf;
namespace win_storage = winrt::Windows::Storage;
namespace win_streams = winrt::Windows::Storage::Streams;

// The worker threads come from QtConcurrent's pool and are reused, so the
// apartment may already be initialised; only a conflicting model is a worry,
// and a pool thread never had one. False: initialisation failed for real.
bool ensure_apartment(QString *error) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        return true;
    } catch (winrt::hresult_error const &e) {
        // RPC_E_CHANGED_MODE: this thread already runs another apartment.
        if (static_cast<int32_t>(e.code()) == static_cast<int32_t>(0x80010106)) return true;
        if (error) *error = QString::fromWCharArray(e.message().c_str());
        return false;
    }
}

QString hresult_text(winrt::hresult_error const &e) {
    const auto code = static_cast<quint32>(static_cast<int32_t>(e.code()));
    const auto hex = QStringLiteral("%1").arg(code, 8, 16, QLatin1Char('0'));
    return QStringLiteral("0x%1: %2").arg(hex, QString::fromWCharArray(e.message().c_str()));
}

// Loads the WinRT document from raw bytes; used when the path route fails.
win_pdf::PdfDocument load_winrt_document_from_bytes(const QByteArray &bytes) {
    win_streams::InMemoryRandomAccessStream stream;
    win_streams::DataWriter writer(stream);
    writer.WriteBytes(winrt::array_view<const uint8_t>(reinterpret_cast<const uint8_t *>(bytes.constData()),
                                                       reinterpret_cast<const uint8_t *>(bytes.constData()) + bytes.size()));
    writer.StoreAsync().get();
    writer.FlushAsync().get();
    writer.DetachStream();
    stream.Seek(0);
    return win_pdf::PdfDocument::LoadFromStreamAsync(stream).get();
}

// Loads the WinRT document for path on this thread; null plus error on failure.
// The StorageFile path route can reject paths Qt itself handles happily
// (observed on a real machine: 0x800700A1 ERROR_BAD_PATHNAME for a file the
// open dialog returned), so a byte-stream load is tried whenever it throws.
win_pdf::PdfDocument load_winrt_document(const QString &file_path, QString *error) {
    try {
        const auto native_path = QDir::toNativeSeparators(file_path);
        auto file = win_storage::StorageFile::GetFileFromPathAsync(winrt::hstring(native_path.toStdWString())).get();
        if (!file) {
            if (error) *error = QStringLiteral("StorageFile is null");
            return nullptr;
        }
        auto document = win_pdf::PdfDocument::LoadFromFileAsync(file).get();
        if (!document) {
            if (error) *error = QStringLiteral("PdfDocument is null");
            return nullptr;
        }
        return document;
    } catch (winrt::hresult_error const &) {
        QFile file(file_path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("could not open the file: %1").arg(file.errorString());
            return nullptr;
        }
        return load_winrt_document_from_bytes(file.readAll());
    }
}
}

PdfLoadResult load_pdf_document(const QString &file_path) {
    PdfLoadResult result;
    if (!ensure_apartment(&result.error)) return result;
    try {
        const auto document = load_winrt_document(file_path, &result.error);
        if (!document) return result;
        auto loaded = std::make_shared<PdfDocument>();
        loaded->file_path_ = file_path;
        loaded->page_count_ = static_cast<int>(document.PageCount());
        result.document = loaded;
        return result;
    } catch (winrt::hresult_error const &e) {
        result.error = hresult_text(e);
        result.document.reset();
        return result;
    } catch (std::exception const &e) {
        result.error = QString::fromUtf8(e.what());
        result.document.reset();
        return result;
    }
}

QImage render_pdf_page(const PdfDocument &document, int page_index, int dpi, QString *error) {
    if (page_index < 0 || page_index >= document.page_count()) {
        if (error) *error = QStringLiteral("page index out of range");
        return {};
    }
    if (!ensure_apartment(error)) return {};
    try {
        const auto winrt_document = load_winrt_document(document.file_path(), error);
        if (!winrt_document) return {};
        auto page = winrt_document.GetPage(static_cast<uint32_t>(page_index));
        if (!page) {
            if (error) *error = QStringLiteral("PdfPage is null");
            return {};
        }
        // PdfPage.Size is in DIPs (1/96 inch); the destination size is in pixels.
        const auto page_size = page.Size();
        win_pdf::PdfPageRenderOptions options;
        options.DestinationWidth(static_cast<uint32_t>(std::max(1.0, page_size.Width * dpi / 96.0)));
        options.DestinationHeight(static_cast<uint32_t>(std::max(1.0, page_size.Height * dpi / 96.0)));
        win_streams::InMemoryRandomAccessStream stream;
        page.RenderToStreamAsync(stream, options).get();
        const uint64_t byte_count = stream.Size();
        if (byte_count == 0) {
            if (error) *error = QStringLiteral("rendered stream is empty");
            return {};
        }
        stream.Seek(0);
        win_streams::DataReader reader(stream.GetInputStreamAt(0));
        const uint32_t loaded = reader.LoadAsync(static_cast<uint32_t>(byte_count)).get();
        if (loaded == 0) {
            if (error) *error = QStringLiteral("could not read the rendered stream");
            return {};
        }
        QByteArray bytes(static_cast<int>(loaded), Qt::Uninitialized);
        reader.ReadBytes(winrt::array_view<uint8_t>(reinterpret_cast<uint8_t *>(bytes.data()),
                                                    reinterpret_cast<uint8_t *>(bytes.data()) + bytes.size()));
        QImage image;
        if (!image.loadFromData(bytes)) {
            if (error) *error = QStringLiteral("could not decode the rendered page");
            return {};
        }
        return image;
    } catch (winrt::hresult_error const &e) {
        if (error) *error = hresult_text(e);
        return {};
    } catch (std::exception const &e) {
        if (error) *error = QString::fromUtf8(e.what());
        return {};
    }
}

#else

PdfLoadResult load_pdf_document(const QString &file_path) {
    Q_UNUSED(file_path);
    PdfLoadResult result;
    result.error = QStringLiteral("PDF files can only be opened on Windows");
    return result;
}

QImage render_pdf_page(const PdfDocument &document, int page_index, int dpi, QString *error) {
    Q_UNUSED(document);
    Q_UNUSED(page_index);
    Q_UNUSED(dpi);
    if (error) *error = QStringLiteral("PDF files can only be opened on Windows");
    return {};
}

#endif

}

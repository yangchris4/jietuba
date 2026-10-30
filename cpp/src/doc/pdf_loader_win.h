#pragma once
#include <QImage>
#include <QString>
#include <memory>

namespace jietuba::doc_pdf {

struct PdfLoadResult;

// A loaded PDF document, opened through Windows' built-in PDF engine
// (Windows.Data.Pdf, WinRT). Only the file path and page count are kept:
// every render loads the document afresh on the calling thread, so no WinRT
// object ever crosses a thread boundary. All functions here block (async
// WinRT calls waited on) and are meant to run on worker threads, never the
// GUI thread. On non-Windows builds every entry point fails with an error.
class PdfDocument {
public:
    QString file_path() const { return file_path_; }
    int page_count() const { return page_count_; }

private:
    friend PdfLoadResult load_pdf_document(const QString &file_path);
    QString file_path_;
    int page_count_ = 0;
};

struct PdfLoadResult {
    // Null when loading failed; error then says why.
    std::shared_ptr<PdfDocument> document;
    QString error;
};

// Opens the document and reads its page count.
PdfLoadResult load_pdf_document(const QString &file_path);

// Renders one page (0-based) at dpi into an image; a null image plus an
// error string on failure. The rendered size follows the page's own size.
QImage render_pdf_page(const PdfDocument &document, int page_index, int dpi, QString *error = nullptr);

}

#include "doc/document_window.h"
#include "canvas/view.h"
#include "core/clipboard_utils.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/save.h"
#include "doc/document_canvas.h"
#include "doc/document_canvas_view.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include "ui/toolbar.h"
#include "ui/toolbar_layout.h"
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QFile>
#include <QResizeEvent>
#include <QShowEvent>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>

namespace jietuba {
namespace {
QString doc_tr(const char *text) { return i18n_tr(text, "DocumentWindow"); }

struct RenderResult {
    QImage image;
    QString error;
};

// Asks the PDF render resolution; 0 when cancelled.
int ask_pdf_dpi(QWidget *parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(doc_tr("PDF Render Quality"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel(doc_tr("Choose the rendering resolution for PDF pages. Higher values are sharper but use more memory."), &dialog);
    label->setWordWrap(true);
    layout->addWidget(label);
    QList<QRadioButton *> buttons;
    for (int dpi : {150, 200, 300}) {
        auto *button = new QRadioButton(QStringLiteral("%1 DPI").arg(dpi), &dialog);
        button->setChecked(dpi == 200);
        buttons.append(button);
        layout->addWidget(button);
    }
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(box);
    QObject::connect(box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (exec_dialog(&dialog, parent) != QDialog::Accepted) return 0;
    static constexpr int DPIS[] = {150, 200, 300};
    for (int i = 0; i < buttons.size(); ++i)
        if (buttons[i]->isChecked()) return DPIS[i];
    return 200;
}
}

DocumentWindow::DocumentWindow(ToolSettingsManager *config, const QString &file_path, const QImage &image)
    : QWidget(nullptr), config_manager(config), file_path_(file_path) {
    pages_.resize(1);
    pages_[0].image = image;
    pages_[0].image_ready = true;
    build_ui();
    show_page(0);
}

DocumentWindow::DocumentWindow(ToolSettingsManager *config, const QString &file_path, std::shared_ptr<doc_pdf::PdfDocument> pdf, int pdf_dpi)
    : QWidget(nullptr), config_manager(config), file_path_(file_path), pdf_(std::move(pdf)), pdf_dpi_(pdf_dpi), is_pdf_(true) {
    pages_.resize(pdf_ ? pdf_->page_count() : 0);
    build_ui();
    if (pages_.isEmpty()) {
        if (auto *label = qobject_cast<QLabel *>(loading_page_)) label->setText(doc_tr("Could not load this PDF file."));
        update_top_bar();
    } else {
        show_page(0);
    }
}

DocumentWindow::~DocumentWindow() = default;

void DocumentWindow::build_ui() {
    setAttribute(Qt::WA_DeleteOnClose, true);
    setWindowTitle(QFileInfo(file_path_).fileName());
    resize(1100, 800);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *top_bar = new QWidget(this);
    top_bar->setFixedHeight(40);
    auto *bar = new QHBoxLayout(top_bar);
    bar->setContentsMargins(6, 4, 6, 4);
    bar->setSpacing(6);
    auto *open_button = new QPushButton(doc_tr("Open"), top_bar);
    connect(open_button, &QPushButton::clicked, this, [this] { DocumentManager::instance()->open_document_dialog(config_manager); });
    bar->addWidget(open_button);
    file_label_ = new QLabel(QFileInfo(file_path_).fileName(), top_bar);
    bar->addWidget(file_label_);
    bar->addStretch();
    prev_button_ = new QPushButton(doc_tr("Previous Page"), top_bar);
    connect(prev_button_, &QPushButton::clicked, this, [this] { show_page(current_page_ - 1); });
    bar->addWidget(prev_button_);
    page_label_ = new QLabel(top_bar);
    bar->addWidget(page_label_);
    next_button_ = new QPushButton(doc_tr("Next Page"), top_bar);
    connect(next_button_, &QPushButton::clicked, this, [this] { show_page(current_page_ + 1); });
    bar->addWidget(next_button_);
    bar->addStretch();
    zoom_label_ = new QLabel(top_bar);
    bar->addWidget(zoom_label_);
    auto *fit_button = new QPushButton(doc_tr("Fit Window"), top_bar);
    connect(fit_button, &QPushButton::clicked, this, [this] {
        if (auto *view = current_view()) view->fit_to_window();
    });
    bar->addWidget(fit_button);
    auto *actual_button = new QPushButton(QStringLiteral("100%"), top_bar);
    connect(actual_button, &QPushButton::clicked, this, [this] {
        if (auto *view = current_view()) view->actual_size();
    });
    bar->addWidget(actual_button);
    auto *save_button = new QPushButton(doc_tr("Save"), top_bar);
    connect(save_button, &QPushButton::clicked, this, &DocumentWindow::save_image);
    bar->addWidget(save_button);
    auto *copy_button = new QPushButton(doc_tr("Copy"), top_bar);
    connect(copy_button, &QPushButton::clicked, this, &DocumentWindow::copy_to_clipboard);
    bar->addWidget(copy_button);
    auto *close_button = new QPushButton(doc_tr("Close"), top_bar);
    connect(close_button, &QPushButton::clicked, this, &QWidget::close);
    bar->addWidget(close_button);
    layout->addWidget(top_bar);

    stack_ = new QStackedWidget(this);
    auto *loading_label = new QLabel(doc_tr("Loading page..."), stack_);
    loading_label->setAlignment(Qt::AlignCenter);
    loading_page_ = loading_label;
    stack_->addWidget(loading_page_);
    layout->addWidget(stack_, 1);

    // The pin layout's toolbar, floating inside the window under the top bar.
    toolbar_ = new Toolbar(this, toolbar_layout::PIN);
    toolbar_->show();
    update_top_bar();
}

void DocumentWindow::position_toolbar() {
    if (!toolbar_) return;
    const int x = std::max(0, (width() - toolbar_->width()) / 2);
    toolbar_->move(x, 46);
    toolbar_->raise();
}

void DocumentWindow::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (!shown_once_) {
        shown_once_ = true;
        position_toolbar();
    }
}

void DocumentWindow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    position_toolbar();
}

void DocumentWindow::closeEvent(QCloseEvent *event) {
    // Release the scenes while their views are still alive, as PinWindow does.
    for (auto &page : pages_)
        if (page.canvas) page.canvas->cleanup();
    QWidget::closeEvent(event);
}

CanvasView *DocumentWindow::canvas_view() const { return current_view(); }

DocumentCanvasView *DocumentWindow::current_view() const {
    if (current_page_ < 0 || current_page_ >= pages_.size()) return nullptr;
    return pages_[current_page_].view;
}

void DocumentWindow::show_page(int index) {
    if (index < 0 || index >= pages_.size()) return;
    // The toolbar belongs to one page at a time; detach it everywhere first,
    // so its signals can never reach two pages' undo stacks at once.
    for (auto &page : pages_)
        if (page.canvas) page.canvas->disconnect_toolbar();
    current_page_ = index;
    Page &page = pages_[index];
    if (!page.image_ready) {
        stack_->setCurrentWidget(loading_page_);
        if (auto *label = qobject_cast<QLabel *>(loading_page_)) label->setText(doc_tr("Loading page..."));
        start_page_render(index);
        update_top_bar();
        return;
    }
    ensure_page_canvas(index);
    stack_->setCurrentWidget(page.view);
    page.canvas->connect_toolbar(toolbar_, page.view);
    // A tool left selected on the previous page applies here too.
    if (toolbar_->current_tool && *toolbar_->current_tool != "cursor") page.canvas->on_tool_changed(*toolbar_->current_tool, toolbar_, page.view);
    update_top_bar();
}

void DocumentWindow::ensure_page_canvas(int index) {
    Page &page = pages_[index];
    if (page.canvas) return;
    page.canvas = new DocumentCanvas(this, page.image.size(), page.image);
    page.view = new DocumentCanvasView(page.canvas->scene, page.canvas, this);
    stack_->addWidget(page.view);
    // Fit once the stack has given the view its real size.
    QTimer::singleShot(0, page.view, [view = QPointer<DocumentCanvasView>(page.view)] {
        if (view) view->fit_to_window();
    });
}

void DocumentWindow::start_page_render(int index) {
    Page &page = pages_[index];
    if (!is_pdf_ || !pdf_ || page.load_started || page.image_ready) return;
    page.load_started = true;
    auto *watcher = new QFutureWatcher<RenderResult>(this);
    connect(watcher, &QFutureWatcher<RenderResult>::finished, this, [this, watcher, index] {
        const auto result = watcher->result();
        watcher->deleteLater();
        on_page_rendered(index, result.image, result.error);
    });
    watcher->setFuture(QtConcurrent::run([pdf = pdf_, index, dpi = pdf_dpi_]() -> RenderResult {
        QString error;
        QImage image = doc_pdf::render_pdf_page(*pdf, index, dpi, &error);
        return {image, error};
    }));
}

void DocumentWindow::on_page_rendered(int index, const QImage &image, const QString &error) {
    if (index < 0 || index >= pages_.size()) return;
    Page &page = pages_[index];
    page.load_started = false;
    if (image.isNull()) {
        log_error(T("PDF 页面渲染失败: {file_path} 第 {page} 页: {error}",
                    {{"file_path", file_path_}, {"page", index + 1}, {"error", error}}),
                  "DocumentWindow");
        if (index == current_page_) {
            if (auto *label = qobject_cast<QLabel *>(loading_page_))
                label->setText(doc_tr("Could not load this PDF page.") + (error.isEmpty() ? QString() : QStringLiteral("\n") + error));
        }
        return;
    }
    page.image = image;
    page.image_ready = true;
    if (index == current_page_) show_page(index);
}

void DocumentWindow::update_top_bar() {
    const bool multi = page_count() > 1;
    page_label_->setVisible(multi);
    prev_button_->setVisible(multi);
    next_button_->setVisible(multi);
    if (multi) {
        page_label_->setText(doc_tr("Page {current} of {total}").replace("{current}", QString::number(current_page_ + 1)).replace("{total}", QString::number(page_count())));
        prev_button_->setEnabled(current_page_ > 0);
        next_button_->setEnabled(current_page_ < page_count() - 1);
    }
    if (auto *view = current_view()) on_view_zoom_changed(view->current_scale());
}

void DocumentWindow::on_view_zoom_changed(double scale) {
    if (zoom_label_) zoom_label_->setText(QStringLiteral("%1%").arg(static_cast<int>(std::lround(scale * 100))));
}

QImage DocumentWindow::current_page_image() {
    if (current_page_ < 0 || current_page_ >= pages_.size()) return {};
    Page &page = pages_[current_page_];
    if (page.canvas) return page.canvas->get_current_image();
    if (page.image_ready) return page.image;
    return {};
}

QList<QImage> DocumentWindow::all_page_images() {
    QList<QImage> images;
    for (int i = 0; i < pages_.size(); ++i) {
        Page &page = pages_[i];
        if (page.canvas) {
            images.append(page.canvas->get_current_image());
        } else if (page.image_ready) {
            images.append(page.image);
        } else if (is_pdf_ && pdf_) {
            // Never visited: render on the spot. This blocks the window for
            // as long as the remaining pages take; it only happens on an
            // explicit export-all.
            QString error;
            QImage image = doc_pdf::render_pdf_page(*pdf_, i, pdf_dpi_, &error);
            if (!image.isNull()) {
                page.image = image;
                page.image_ready = true;
                images.append(image);
            } else {
                log_error(T("PDF 页面渲染失败: {file_path} 第 {page} 页: {error}", {{"file_path", file_path_}, {"page", i + 1}, {"error", error}}), "DocumentWindow");
            }
        }
    }
    return images;
}

bool DocumentWindow::export_all_pages_pdf(const QString &path, const QList<QImage> &images) {
    if (images.isEmpty()) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QPdfWriter writer(&file);
    const int dpi = is_pdf_ ? pdf_dpi_ : 200;
    writer.setResolution(dpi);
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));
    QPainter painter;
    bool first = true;
    for (const auto &image : images) {
        const QSizeF size_points(image.width() * 72.0 / dpi, image.height() * 72.0 / dpi);
        writer.setPageSize(QPageSize(size_points, QPageSize::Point));
        if (first) {
            if (!painter.begin(&writer)) return false;
            first = false;
        } else if (!writer.newPage()) {
            painter.end();
            return false;
        }
        const QRect rect = writer.pageLayout().paintRectPixels(writer.resolution());
        painter.drawImage(rect, image);
    }
    painter.end();
    return true;
}

void DocumentWindow::save_image() {
    if (current_page_ < 0 || current_page_ >= pages_.size() || !pages_[current_page_].image_ready) return;
    const SaveService save_service(config_manager ? *config_manager : *get_tool_settings_manager());
    const QString base = QFileInfo(file_path_).completeBaseName();
    const auto fmt = (config_manager ? config_manager->get_screenshot_format() : QString("PNG")).toLower();
    const auto default_name = QStringLiteral("%1_annotated.%2").arg(base, fmt);
    auto [file_path, selected_filter] =
        get_save_file_name(this, doc_tr("Save Document Image"), save_service.suggest_path(default_name), "PNG (*.png);;JPG (*.jpg);;BMP (*.bmp);;WebP (*.webp);;PDF (*.pdf)");
    if (file_path.isEmpty()) return;
    const auto ext = QFileInfo(file_path).suffix();
    QString image_format;
    if (!ext.isEmpty()) {
        image_format = ext.toUpper();
    } else {
        static const QRegularExpression pattern(R"(\*\.(\w+))");
        const auto match = pattern.match(selected_filter);
        image_format = match.hasMatch() ? match.captured(1).toUpper() : QString("PNG");
        file_path = file_path + "." + image_format.toLower();
    }
    // A multi-page PDF saved as PDF keeps every page, annotations included.
    if (image_format == "PDF" && is_pdf_ && page_count() > 1) {
        if (export_all_pages_pdf(file_path, all_page_images()))
            log_info(T("保存成功: {file_path}", {{"file_path", file_path}}), "DocumentWindow");
        else
            log_error(T("保存失败: {file_path}", {{"file_path", file_path}}), "DocumentWindow");
        return;
    }
    const auto image = current_page_image();
    if (image.isNull()) return;
    if (SaveService::save_qimage_to_path(image, file_path, image_format))
        log_info(T("保存成功: {file_path}", {{"file_path", file_path}}), "DocumentWindow");
    else
        log_error(T("保存失败: {file_path}", {{"file_path", file_path}}), "DocumentWindow");
}

void DocumentWindow::copy_to_clipboard() {
    const auto image = current_page_image();
    if (image.isNull()) return;
    deliver_image_async(image);
}

// ---------------------------------------------------------------- DocumentManager

DocumentManager *DocumentManager::instance() {
    static DocumentManager manager;
    return &manager;
}

void DocumentManager::track_window(DocumentWindow *window) {
    windows_.append(window);
    connect(window, &QObject::destroyed, this, [this] {
        windows_.erase(std::remove_if(windows_.begin(), windows_.end(), [](const QPointer<DocumentWindow> &w) { return w.isNull(); }), windows_.end());
    });
}

void DocumentManager::open_document_dialog(ToolSettingsManager *config_manager) {
    const auto [path, filter] = get_open_file_name(nullptr, doc_tr("Open Image/PDF for Annotation"), QString(),
                                                   doc_tr("Images and PDF (*.png *.jpg *.jpeg *.bmp *.webp *.gif *.tif *.tiff *.pdf);;All Files (*.*)"));
    Q_UNUSED(filter);
    if (path.isEmpty()) return;
    const auto absolute = QFileInfo(path).absoluteFilePath();
    for (const auto &window : windows_) {
        if (window && QFileInfo(window->file_path()).absoluteFilePath() == absolute) {
            window->raise();
            window->activateWindow();
            return;
        }
    }
    if (QFileInfo(path).suffix().compare("pdf", Qt::CaseInsensitive) == 0) open_pdf_file(config_manager, path);
    else open_image_file(config_manager, path);
}

void DocumentManager::open_image_file(ToolSettingsManager *config_manager, const QString &path) {
    const QImage image(path);
    if (image.isNull()) {
        log_error(T("打开图片失败: {file_path}", {{"file_path", path}}), "DocumentWindow");
        show_error_dialog(nullptr, doc_tr("Open Failed"), doc_tr("Could not load this image file."));
        return;
    }
    auto *window = new DocumentWindow(config_manager, path, image);
    track_window(window);
    window->show();
    log_info(T("打开文档标注: {file_path} ({width}x{height})", {{"file_path", path}, {"width", image.width()}, {"height", image.height()}}), "DocumentWindow");
}

void DocumentManager::open_pdf_file(ToolSettingsManager *config_manager, const QString &path) {
    const int dpi = ask_pdf_dpi(nullptr);
    if (dpi == 0) return;
    auto *watcher = new QFutureWatcher<doc_pdf::PdfLoadResult>(this);
    connect(watcher, &QFutureWatcher<doc_pdf::PdfLoadResult>::finished, this, [this, watcher, config_manager, path, dpi] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (!result.document) {
            log_error(T("打开 PDF 失败: {file_path}: {error}", {{"file_path", path}, {"error", result.error}}), "DocumentWindow");
            show_error_dialog(nullptr, doc_tr("Open Failed"), doc_tr("Could not load this PDF file.") + (result.error.isEmpty() ? QString() : QStringLiteral("\n") + result.error));
            return;
        }
        auto *window = new DocumentWindow(config_manager, path, result.document, dpi);
        track_window(window);
        window->show();
        log_info(T("打开文档标注: {file_path} ({pages} 页, {dpi} DPI)",
                   {{"file_path", path}, {"pages", result.document->page_count()}, {"dpi", dpi}}),
                 "DocumentWindow");
    });
    watcher->setFuture(QtConcurrent::run([path] { return doc_pdf::load_pdf_document(path); }));
}
}

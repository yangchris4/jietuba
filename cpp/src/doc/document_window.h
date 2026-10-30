#pragma once
#include "canvas/canvas_host.h"
#include "doc/pdf_loader_win.h"
#include <QImage>
#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>
#include <memory>

class QLabel;
class QPushButton;
class QStackedWidget;

namespace jietuba {
class CanvasView;
class DocumentCanvas;
class DocumentCanvasView;
class Toolbar;
class ToolSettingsManager;

// A normal, resizable window annotating one local file — an image, or one
// PDF whose pages are rendered through Windows' PDF engine. Every page gets
// its own canvas scene (built on first visit), so pages keep their own
// annotations; the window's one toolbar is reconnected to the page shown.
// The annotation tools are the capture/pin ones, unchanged.
class DocumentWindow : public QWidget, public CanvasHost {
    Q_OBJECT
public:
    // An image file: a single page, ready at once.
    DocumentWindow(ToolSettingsManager *config_manager, const QString &file_path, const QImage &image);
    // A PDF file: pages render lazily at dpi on worker threads.
    DocumentWindow(ToolSettingsManager *config_manager, const QString &file_path, std::shared_ptr<doc_pdf::PdfDocument> pdf, int pdf_dpi);
    ~DocumentWindow() override;

    // CanvasHost: the view finds the toolbar and this window through here.
    Toolbar *canvas_toolbar() const override { return toolbar_; }
    CanvasView *canvas_view() const override;
    void update_toolbar_position() override { position_toolbar(); }

    Toolbar *annotation_toolbar() const { return toolbar_; }
    QString file_path() const { return file_path_; }
    // From the current page's view after every zoom change.
    void on_view_zoom_changed(double scale);
    // The toolbar's and the bar's save/copy actions.
    void save_image();
    void copy_to_clipboard();

protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    struct Page {
        // The rendered page; empty until ready (PDF pages load lazily).
        QImage image;
        bool image_ready = false;
        bool load_started = false;
        // Built on first visit; kept so the page's annotations survive.
        DocumentCanvas *canvas = nullptr;
        DocumentCanvasView *view = nullptr;
    };
    void build_ui();
    void position_toolbar();
    int page_count() const { return pages_.size(); }
    void show_page(int index);
    void ensure_page_canvas(int index);
    void start_page_render(int index);
    void on_page_rendered(int index, const QImage &image, const QString &error);
    // The current page flattened (background plus annotations).
    QImage current_page_image();
    // Every page flattened in order; unvisited PDF pages render on the spot.
    QList<QImage> all_page_images();
    bool export_all_pages_pdf(const QString &path, const QList<QImage> &images);
    void update_top_bar();
    DocumentCanvasView *current_view() const;

    ToolSettingsManager *config_manager;
    QString file_path_;
    std::shared_ptr<doc_pdf::PdfDocument> pdf_;
    int pdf_dpi_ = 200;
    bool is_pdf_ = false;
    QList<Page> pages_;
    int current_page_ = 0;
    bool shown_once_ = false;

    Toolbar *toolbar_ = nullptr;
    QStackedWidget *stack_ = nullptr;
    QWidget *loading_page_ = nullptr;
    QLabel *file_label_ = nullptr;
    QLabel *page_label_ = nullptr;
    QLabel *zoom_label_ = nullptr;
    QPushButton *prev_button_ = nullptr;
    QPushButton *next_button_ = nullptr;
};

// Holds the open document windows, like PinManager holds the pins: one
// window per file, several files at once.
class DocumentManager : public QObject {
    Q_OBJECT
public:
    static DocumentManager *instance();
    // The one entry point (tray menu): pick a file, then open its window.
    void open_document_dialog(ToolSettingsManager *config_manager);

private:
    DocumentManager() = default;
    void track_window(DocumentWindow *window);
    void open_image_file(ToolSettingsManager *config_manager, const QString &path);
    void open_pdf_file(ToolSettingsManager *config_manager, const QString &path);
    QList<QPointer<DocumentWindow>> windows_;
};
}

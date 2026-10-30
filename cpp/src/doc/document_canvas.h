#pragma once
#include <QColor>
#include <QImage>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QVariant>

namespace jietuba {
class CanvasScene;
class CanvasView;
class CommandUndoStack;
class DocumentWindow;
class Toolbar;
class ToolController;

// One document page's canvas: the same CanvasScene the capture and pin
// windows use, so every annotation tool behaves identically. Modelled on
// PinCanvas, minus the pin-only parts (OCR, file cards, hover controls).
// Connections to the shared toolbar are kept, so the window can move the
// toolbar between pages by disconnecting and reconnecting.
class DocumentCanvas : public QObject {
    Q_OBJECT
public:
    DocumentCanvas(DocumentWindow *parent_window, const QSize &base_size, const QImage &background_image);
    ~DocumentCanvas() override;

    // Enters editing; false for a tool the canvas does not have.
    bool activate_tool(const QString &tool_name);
    void deactivate_tool();
    bool is_editing() const { return is_editing_; }
    // Connects the window's one toolbar to this page; disconnect first.
    void connect_toolbar(Toolbar *toolbar, CanvasView *view);
    void disconnect_toolbar();
    // What a toolbar tool change does; also called after a page switch to
    // bring this page to the toolbar's current tool.
    void on_tool_changed(const QString &tool_name, Toolbar *toolbar, CanvasView *view);
    // All three go through scene.update_style.
    void set_color(const QColor &color);
    void set_stroke_width(int width);
    void set_opacity(double opacity);
    // Background plus vector annotations, at the page's own resolution.
    QImage get_current_image();
    void cleanup();

    DocumentWindow *parent_window;
    QSize base_size;
    CanvasScene *scene = nullptr;
    CommandUndoStack *undo_stack = nullptr;
    ToolController *tool_controller = nullptr;

private:
    void initialize_selection();
    void on_stroke_width_changed(int width, CanvasView *view);
    void on_opacity_changed(int opacity_int, CanvasView *view);
    void on_arrow_style_changed(const QString &style, CanvasView *view);
    void on_watermark_requested(Toolbar *toolbar);
    void on_watermark_style_changed(const QString &key, const QVariant &value);
    void on_watermark_text_changed(const QString &template_text, Toolbar *toolbar);

    bool is_editing_ = false;
    bool cleaned_up_ = false;
    QPointer<CanvasView> view_;
    QList<QMetaObject::Connection> toolbar_connections_;
};
}

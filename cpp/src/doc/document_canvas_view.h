#pragma once
#include "canvas/view.h"
#include <QPoint>

namespace jietuba {
class DocumentCanvas;
class DocumentWindow;

// The document window's page view. Navigation lives here and nowhere else:
// with the cursor tool the wheel zooms around the pointer and a left drag
// pans (the middle button and Space+drag pan under any tool), while every
// other tool keeps CanvasView's wheel and mouse behaviour untouched — brush
// width on the wheel, Shift+wheel for the next number, item editing, all of
// it. Cross-tool selection stays off, like a pin outside editing: with the
// cursor tool you navigate, with a drawing tool you draw and edit.
class DocumentCanvasView : public CanvasView {
    Q_OBJECT
public:
    DocumentCanvasView(CanvasScene *scene, DocumentCanvas *canvas, DocumentWindow *window);
    // The view transform's scale: 1.0 shows one image pixel per logical pixel.
    double current_scale() const;
    // Zooms around the viewport point, clamped to [MIN_SCALE, MAX_SCALE].
    void zoom_by(double factor, const QPoint &viewport_pos);
    void fit_to_window();
    void actual_size();

    DocumentCanvas *doc_canvas;
    DocumentWindow *doc_window;

protected:
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private:
    static constexpr double MIN_SCALE = 0.05;
    static constexpr double MAX_SCALE = 8.0;
    bool cursor_tool_active() const;
    void begin_pan(const QPoint &viewport_pos);
    void notify_zoom_changed();

    bool space_down_ = false;
    bool panning_ = false;
    QPoint pan_last_pos_;
};
}

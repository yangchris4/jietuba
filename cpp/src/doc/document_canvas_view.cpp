#include "doc/document_canvas_view.h"
#include "canvas/scene.h"
#include "doc/document_canvas.h"
#include "doc/document_window.h"
#include "tools/controller.h"
#include "tools/cursor_manager.h"
#include "ui/toolbar.h"
#include <QColor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace jietuba {
DocumentCanvasView::DocumentCanvasView(CanvasScene *scene, DocumentCanvas *canvas, DocumentWindow *window)
    : CanvasView(scene, nullptr, false, false), doc_canvas(canvas), doc_window(window) {
    setFrameShape(QFrame::NoFrame);
    setRenderHint(QPainter::SmoothPixmapTransform, true);
    // The scroll bars are the pan mechanism; they stay available while zooming.
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // A document view centres the page (CanvasView aligns captures top-left)
    // and shows a neutral desk colour around it.
    setAlignment(Qt::AlignCenter);
    setBackgroundBrush(QColor(0x5a, 0x5a, 0x5a));
}

double DocumentCanvasView::current_scale() const { return transform().m11(); }

bool DocumentCanvasView::cursor_tool_active() const {
    return canvas_scene && canvas_scene->tool_controller && canvas_scene->tool_controller->current_tool_id() == "cursor";
}

void DocumentCanvasView::notify_zoom_changed() {
    if (cursor_manager) cursor_manager->update_view_scale(std::sqrt(std::abs(transform().determinant())));
    if (doc_window) doc_window->on_view_zoom_changed(current_scale());
}

void DocumentCanvasView::zoom_by(double factor, const QPoint &viewport_pos) {
    Q_UNUSED(viewport_pos);
    if (!canvas_scene) return;
    const double current = current_scale();
    const double target = std::clamp(current * factor, MIN_SCALE, MAX_SCALE);
    if (std::abs(target - current) < 1e-9) return;
    // AnchorUnderMouse keeps the scene point under the pointer in place.
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    scale(target / current, target / current);
    setTransformationAnchor(QGraphicsView::AnchorViewCenter);
    notify_zoom_changed();
}

void DocumentCanvasView::fit_to_window() {
    if (!canvas_scene) return;
    const QRectF rect = canvas_scene->sceneRect();
    if (rect.isEmpty()) return;
    resetTransform();
    const auto vp = viewport()->size();
    double factor = std::min(vp.width() / rect.width(), vp.height() / rect.height());
    factor = std::clamp(factor, MIN_SCALE, MAX_SCALE);
    scale(factor, factor);
    centerOn(rect.center());
    notify_zoom_changed();
}

void DocumentCanvasView::actual_size() {
    if (!canvas_scene) return;
    const QPointF centre = mapToScene(viewport()->rect().center());
    resetTransform();
    centerOn(centre);
    notify_zoom_changed();
}

void DocumentCanvasView::wheelEvent(QWheelEvent *event) {
    // Only the cursor tool navigates; every other tool's wheel semantics are
    // CanvasView's own and must not be intercepted.
    if (cursor_tool_active() && !is_text_editing()) {
        const int delta = event->angleDelta().y();
        if (delta != 0) {
            // One standard wheel notch (120) is one 1.15 step.
            zoom_by(std::pow(1.15, delta / 120.0), event->position().toPoint());
            event->accept();
            return;
        }
    }
    CanvasView::wheelEvent(event);
}

void DocumentCanvasView::begin_pan(const QPoint &viewport_pos) {
    panning_ = true;
    pan_last_pos_ = viewport_pos;
    viewport()->setCursor(Qt::ClosedHandCursor);
}

void DocumentCanvasView::mousePressEvent(QMouseEvent *event) {
    const bool pan_button = event->button() == Qt::MiddleButton;
    const bool pan_left = event->button() == Qt::LeftButton && (space_down_ || cursor_tool_active());
    if ((pan_button || pan_left) && !is_text_editing()) {
        begin_pan(event->pos());
        event->accept();
        return;
    }
    CanvasView::mousePressEvent(event);
}

void DocumentCanvasView::mouseMoveEvent(QMouseEvent *event) {
    if (panning_) {
        const QPoint delta = event->pos() - pan_last_pos_;
        pan_last_pos_ = event->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        event->accept();
        return;
    }
    CanvasView::mouseMoveEvent(event);
}

void DocumentCanvasView::mouseReleaseEvent(QMouseEvent *event) {
    if (panning_ && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        panning_ = false;
        viewport()->setCursor(space_down_ ? Qt::OpenHandCursor : Qt::ArrowCursor);
        event->accept();
        return;
    }
    CanvasView::mouseReleaseEvent(event);
}

void DocumentCanvasView::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Space && !is_text_editing()) {
        if (!event->isAutoRepeat()) {
            space_down_ = true;
            if (!panning_) viewport()->setCursor(Qt::OpenHandCursor);
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && doc_canvas && doc_canvas->is_editing()) {
        // Esc leaves the tool and keeps the window, as on a pin.
        doc_canvas->deactivate_tool();
        if (auto *toolbar = doc_window ? doc_window->annotation_toolbar() : nullptr) {
            if (toolbar->current_tool) {
                for (auto *button : toolbar->tool_buttons) button->setChecked(false);
                toolbar->current_tool.reset();
            }
        }
        event->accept();
        return;
    }
    CanvasView::keyPressEvent(event);
}

void DocumentCanvasView::keyReleaseEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        space_down_ = false;
        if (!panning_) apply_tool_cursor();
    }
    CanvasView::keyReleaseEvent(event);
}
}

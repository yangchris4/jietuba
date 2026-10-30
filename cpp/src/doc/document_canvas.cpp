#include "doc/document_canvas.h"
#include "canvas/items/arrow_item.h"
#include "canvas/items/selection_item.h"
#include "canvas/scene.h"
#include "canvas/selection_model.h"
#include "canvas/smart_edit_controller.h"
#include "canvas/undo.h"
#include "canvas/view.h"
#include "core/logger.h"
#include "doc/document_window.h"
#include "pin/pin_canvas.h"
#include "tools/controller.h"
#include "tools/cursor_manager.h"
#include "tools/mosaic.h"
#include "tools/number.h"
#include "tools/watermark.h"
#include "ui/toolbar.h"
#include <QPainter>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace jietuba {
DocumentCanvas::DocumentCanvas(DocumentWindow *parent_window, const QSize &base_size, const QImage &background_image)
    : QObject(parent_window), parent_window(parent_window), base_size(base_size) {
    // Like a pin: the scene's background is the page itself, so mosaic smears
    // the page and the origin lines up with the image.
    scene = new CanvasScene(background_image, QRectF(0, 0, base_size.width(), base_size.height()), true);
    initialize_selection();
    undo_stack = scene->undo_stack;
    tool_controller = scene->tool_controller.get();
}

DocumentCanvas::~DocumentCanvas() { cleanup(); }

void DocumentCanvas::initialize_selection() {
    const QRectF full_rect(0, 0, base_size.width(), base_size.height());
    scene->selection_model->initialize_confirmed_rect(full_rect);
    scene->selection_item->hide();
    scene->selection_item->setEnabled(false);
}

bool DocumentCanvas::activate_tool(const QString &tool_name) {
    if (!tool_controller->get_tool(tool_name)) {
        log_warning(T("文档页不支持工具: {tool_name}", {{"tool_name", tool_name}}), "DocumentCanvas");
        return false;
    }
    try {
        tool_controller->activate(tool_name);
        is_editing_ = tool_name != "cursor";
        return true;
    } catch (const std::exception &e) {
        log_error(T("工具激活失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "DocumentCanvas");
        is_editing_ = false;
        return false;
    }
}

void DocumentCanvas::deactivate_tool() {
    if (scene && scene->items().isEmpty()) {
        is_editing_ = false;
        return;
    }
    if (tool_controller) tool_controller->activate("cursor");
    is_editing_ = false;
    if (view_) {
        view_->setCursor(Qt::ArrowCursor);
        view_->viewport()->setCursor(Qt::ArrowCursor);
    }
}

void DocumentCanvas::connect_toolbar(Toolbar *toolbar, CanvasView *view) {
    view_ = view;
    auto track = [this](QMetaObject::Connection connection) { toolbar_connections_.append(connection); };
    track(connect(toolbar, &Toolbar::tool_changed, this, [this, toolbar, view](const QString &name) { on_tool_changed(name, toolbar, view); }));
    track(connect(toolbar, &Toolbar::undo_clicked, this, [this] { undo_stack->undo(); }));
    track(connect(toolbar, &Toolbar::redo_clicked, this, [this] { undo_stack->redo(); }));
    track(connect(toolbar, &Toolbar::color_changed, this, &DocumentCanvas::set_color));
    track(connect(toolbar, &Toolbar::stroke_width_changed, this, [this, view](int width) { on_stroke_width_changed(width, view); }));
    track(connect(toolbar, &Toolbar::opacity_changed, this, [this, view](int opacity) { on_opacity_changed(opacity, view); }));
    track(connect(toolbar, &Toolbar::arrow_style_changed, this, [this, view](const QString &style) { on_arrow_style_changed(style, view); }));
    track(connect(toolbar, &Toolbar::mosaic_style_changed, this, [view](const QString &style) { MosaicTool::apply_style_change(style, view); }));
    track(connect(toolbar, &Toolbar::mosaic_block_size_changed, this, [view](int value) { MosaicTool::apply_block_size_change(value, view); }));
    track(connect(toolbar, &Toolbar::line_style_changed, this, [view](const QString &style) {
        if (view) view->apply_line_style_change_to_selection(style);
    }));
    track(connect(toolbar, &Toolbar::highlighter_mode_changed, this, [view](const QString &) {
        if (view && view->cursor_manager) view->cursor_manager->set_tool_cursor("highlighter", true);
    }));
    track(connect(toolbar, &Toolbar::number_next_changed, this, [this](int value) { NumberTool::set_next_number_and_refresh(scene, value); }));
    track(connect(toolbar, &Toolbar::number_style_changed, this, [this, view](const QString &style) { NumberTool::apply_style_change(style, view, undo_stack); }));
    if (view && view->smart_edit_controller) {
        auto *controller = view->smart_edit_controller.get();
        track(connect(toolbar, &Toolbar::text_font_changed, view, [controller](const QFont &font) { controller->on_text_font_changed(font); }));
        track(connect(toolbar, &Toolbar::text_outline_changed, view,
                      [controller](bool enabled, const QColor &color, double width) { controller->on_text_outline_changed(enabled, color, width); }));
        track(connect(toolbar, &Toolbar::text_shadow_changed, view,
                      [controller](bool enabled, const QColor &color) { controller->on_text_shadow_changed(enabled, color); }));
        track(connect(toolbar, &Toolbar::text_background_changed, view,
                      [controller](bool enabled, const QColor &color, int opacity) { controller->on_text_background_changed(enabled, color, opacity); }));
        track(connect(toolbar, &Toolbar::color_changed, view, [controller](const QColor &color) { controller->on_text_color_changed(color); }));
    }
    track(connect(toolbar, &Toolbar::watermark_requested, this, [this, toolbar] { on_watermark_requested(toolbar); }));
    track(connect(toolbar, &Toolbar::watermark_style_changed, this, &DocumentCanvas::on_watermark_style_changed));
    track(connect(toolbar, &Toolbar::watermark_text_changed, this, [this, toolbar](const QString &template_text) { on_watermark_text_changed(template_text, toolbar); }));
    // Saving and copying stay with the window, as in PinCanvas.
    track(connect(toolbar, &Toolbar::save_clicked, parent_window, &DocumentWindow::save_image));
    track(connect(toolbar, &Toolbar::copy_clicked, parent_window, &DocumentWindow::copy_to_clipboard));
}

void DocumentCanvas::disconnect_toolbar() {
    for (const auto &connection : toolbar_connections_) disconnect(connection);
    toolbar_connections_.clear();
}

void DocumentCanvas::on_tool_changed(const QString &tool_name, Toolbar *toolbar, CanvasView *view) {
    if (!tool_name.isEmpty() && tool_name != "cursor") {
        if (!activate_tool(tool_name)) return;
        if (toolbar && tool_controller) {
            const auto &ctx = tool_controller->ctx;
            const bool blocked = toolbar->blockSignals(true);
            toolbar->set_current_color(ctx.color);
            toolbar->set_stroke_width(static_cast<int>(ctx.stroke_width));
            toolbar->set_opacity(static_cast<int>(ctx.opacity * 255));
            if (tool_name == "number") toolbar->set_number_next_value(NumberTool::get_next_number(scene));
            toolbar->blockSignals(blocked);
        }
        QTimer::singleShot(0, view, [view = QPointer<CanvasView>(view)] {
            if (view) view->setFocus();
        });
    } else {
        deactivate_tool();
    }
}

void DocumentCanvas::on_watermark_requested(Toolbar *toolbar) {
    auto *item = WatermarkTool::add(scene);
    toolbar->show_watermark(item->template_text(), item->created_at());
}

void DocumentCanvas::on_watermark_style_changed(const QString &key, const QVariant &value) { WatermarkTool::change_style(scene, {{key, value}}); }

void DocumentCanvas::on_watermark_text_changed(const QString &template_text, Toolbar *toolbar) {
    if (auto *item = WatermarkTool::change_text(scene, template_text)) toolbar->show_watermark(item->template_text(), item->created_at());
}

void DocumentCanvas::on_stroke_width_changed(int width, CanvasView *view) {
    const auto *ctx = tool_controller ? &tool_controller->ctx : nullptr;
    const double prev = ctx ? std::max(1.0, ctx->stroke_width) : width;
    set_stroke_width(width);
    const double now = ctx ? std::max(1.0, ctx->stroke_width) : width;
    if (std::abs(now - prev) > 1e-6 && prev > 0 && now > 0 && view) view->apply_size_change_to_selection(now / prev);
    if (view && view->cursor_manager) view->cursor_manager->update_tool_cursor_size(width);
}

void DocumentCanvas::on_opacity_changed(int opacity_int, CanvasView *view) {
    const double opacity = opacity_int / 255.0;
    set_opacity(opacity);
    if (view) view->apply_opacity_change_to_selection(opacity);
}

void DocumentCanvas::on_arrow_style_changed(const QString &style, CanvasView *view) {
    if (!view || !view->smart_edit_controller) return;
    auto *arrow = dynamic_cast<ArrowItem *>(view->smart_edit_controller->selected_item());
    if (!arrow) return;
    const auto old_state = PinCanvas::capture_arrow_state(arrow);
    arrow->set_arrow_style(style);
    const auto new_state = PinCanvas::capture_arrow_state(arrow);
    if (undo_stack) undo_stack->push(new EditItemCommand(ItemReference(arrow), old_state, new_state, QString::fromUtf8("修改箭头样式")));
    arrow->update();
}

void DocumentCanvas::set_color(const QColor &color) { scene->update_style(color); }

void DocumentCanvas::set_stroke_width(int width) { scene->update_style(std::nullopt, width); }

void DocumentCanvas::set_opacity(double opacity) { scene->update_style(std::nullopt, std::nullopt, opacity); }

QImage DocumentCanvas::get_current_image() {
    QImage image(base_size.width(), base_size.height(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    scene->render(&painter, QRectF(0, 0, base_size.width(), base_size.height()), QRectF(0, 0, base_size.width(), base_size.height()));
    painter.end();
    return image;
}

void DocumentCanvas::cleanup() {
    if (cleaned_up_) return;
    cleaned_up_ = true;
    disconnect_toolbar();
    deactivate_tool();
    if (undo_stack) undo_stack->clear();
    if (tool_controller && tool_controller->current_tool) {
        tool_controller->current_tool->on_deactivate(tool_controller->ctx);
        tool_controller->current_tool = nullptr;
    }
    if (scene) {
        scene->deleteLater();
        scene = nullptr;
        undo_stack = nullptr;
        tool_controller = nullptr;
    }
}
}

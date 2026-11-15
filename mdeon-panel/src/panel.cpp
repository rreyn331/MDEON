#include "panel/panel.hpp"

#include "panel/theme.hpp"
#include "panel/logging.hpp"

#include <algorithm>
#include <chrono>
#include <glib-unix.h>

Panel::Panel(std::unique_ptr<IBackend> backend, bool vertical)
    : backend_(std::move(backend)), vertical_(vertical)
{
    backend_->set_on_click([this](int x, int y, int button) {
        handle_click(x, y, button);
    });
    backend_->set_on_motion([this](int x, int y) {
        handle_motion(x, y);
    });
}

void Panel::add_module(std::unique_ptr<IModule> module, Alignment alignment)
{
    modules_.push_back({std::move(module), alignment, {}});
}

void Panel::request_redraw()
{
    render_frame();
}

void Panel::run_loop()
{
    render_frame();
    if (rendered_width_ > 0 && rendered_height_ > 0) {
        panel_debug("first panel frame committed; ready size=", rendered_width_, "x", rendered_height_);
    } else {
        panel_debug("initial panel frame could not be committed");
    }

    main_loop_ = g_main_loop_new(nullptr, FALSE);
    guint event_source = 0;
    const int backend_fd = backend_->event_fd();
    if (backend_fd >= 0) {
        event_source = g_unix_fd_add(
            backend_fd, static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP),
            [](gint, GIOCondition condition, gpointer data) -> gboolean {
                auto* panel = static_cast<Panel*>(data);
                if ((condition & (G_IO_ERR | G_IO_HUP)) != 0) {
                    panel->stop();
                    return G_SOURCE_REMOVE;
                }
                panel->backend_->process_events();
                if (panel->backend_->width() != panel->rendered_width_ ||
                    panel->backend_->height() != panel->rendered_height_) {
                    panel->render_frame();
                }
                return G_SOURCE_CONTINUE;
            }, this);
    }
    const guint update_source = g_timeout_add_seconds(
        1, [](gpointer data) -> gboolean {
            auto* panel = static_cast<Panel*>(data);
            if (!panel->running_) {
                return G_SOURCE_REMOVE;
            }
            bool changed = false;
            for (auto& item : panel->modules_) {
                changed = item.module->update() || changed;
            }
            if (changed) {
                panel->render_frame();
            }
            return G_SOURCE_CONTINUE;
        }, this);

    g_main_loop_run(main_loop_);
    if (event_source != 0) {
        g_source_remove(event_source);
    }
    g_source_remove(update_source);
    g_main_loop_unref(main_loop_);
    main_loop_ = nullptr;
}

void Panel::stop()
{
    running_ = false;
    if (main_loop_ != nullptr) {
        g_main_loop_quit(main_loop_);
    }
}

void Panel::calculate_layout(cairo_t* cr)
{
    constexpr int panel_padding = 10;
    constexpr int gap = 5;
    const int axis_length = vertical_ ? backend_->height() : backend_->width();
    const int thickness = vertical_ ? backend_->width() : backend_->height();
    std::vector<int> widths(modules_.size(), 0);
    int occupied_length = 0;
    int module_count = 0;
    const int content_length = std::max(0, axis_length - 2 * panel_padding);
    for (std::size_t index = 0; index < modules_.size(); ++index) {
        auto& item = modules_[index];
        if (item.alignment == Alignment::Center) {
            continue;
        }
        item.module->set_available_width(content_length);
        widths[index] = item.module->measure_width(cr, thickness);
        occupied_length += widths[index];
        ++module_count;
    }
    if (occupied_length + gap * module_count > content_length) {
        for (std::size_t index = 0; index < modules_.size(); ++index) {
            auto& item = modules_[index];
            if (item.alignment == Alignment::Center) {
                continue;
            }
            const int available_width = std::max(0, content_length -
                (occupied_length - widths[index]) - gap * module_count);
            item.module->set_available_width(available_width);
            widths[index] = item.module->measure_width(cr, thickness);
        }
    }
    occupied_length = 0;
    int expanding_count = 0;
    for (std::size_t index = 0; index < modules_.size(); ++index) {
        const auto& item = modules_[index];
        if (item.alignment == Alignment::Center) {
            continue;
        }
        occupied_length += widths[index];
        if (item.module->expands()) {
            ++expanding_count;
        }
    }
    int expandable_space = std::max(0,
        content_length - occupied_length - gap * module_count);
    for (std::size_t index = 0; index < modules_.size(); ++index) {
        auto& item = modules_[index];
        if (item.alignment == Alignment::Center || !item.module->expands()) {
            continue;
        }
        const int share = expanding_count == 0 ? 0 :
            expandable_space / expanding_count;
        widths[index] += share;
        item.module->set_available_width(widths[index]);
        static_cast<void>(item.module->measure_width(cr, thickness));
        expandable_space -= share;
        --expanding_count;
    }

    int start = panel_padding;
    int end = axis_length - panel_padding;

    for (std::size_t index = 0; index < modules_.size(); ++index) {
        auto& item = modules_[index];
        if (item.alignment != Alignment::Start) {
            continue;
        }
        const int module_width = widths[index];
        item.geometry = vertical_
            ? Geometry{0, start, thickness, module_width, true}
            : Geometry{start, 0, module_width, thickness, false};
        start += module_width + gap;
    }

    for (std::size_t offset = 0; offset < modules_.size(); ++offset) {
        const std::size_t index = modules_.size() - offset - 1;
        auto& item = modules_[index];
        if (item.alignment != Alignment::End) {
            continue;
        }
        const int module_width = widths[index];
        end -= module_width;
        item.geometry = vertical_
            ? Geometry{0, end, thickness, module_width, true}
            : Geometry{end, 0, module_width, thickness, false};
        end -= gap;
    }

    int center_width = 0;
    int center_count = 0;
    for (auto& item : modules_) {
        if (item.alignment == Alignment::Center) {
            const int extent = item.module->measure_width(cr, thickness);
            item.geometry.width = vertical_ ? thickness : extent;
            item.geometry.height = vertical_ ? extent : thickness;
            item.geometry.vertical = vertical_;
            center_width += extent;
            ++center_count;
        }
    }
    if (center_count > 0) {
        center_width += (center_count - 1) * gap;
        int center = (axis_length - center_width) / 2;
        for (auto& item : modules_) {
            if (item.alignment == Alignment::Center) {
                if (vertical_) {
                    item.geometry.x = 0;
                    item.geometry.y = center;
                } else {
                    item.geometry.x = center;
                    item.geometry.y = 0;
                }
                center += (vertical_ ? item.geometry.height : item.geometry.width) + gap;
            }
        }
    }
}

void Panel::render_frame()
{
    cairo_t* cr = backend_->get_cairo();
    if (cr == nullptr) {
        return;
    }

    cairo_set_source_rgb(cr, PanelTheme::background.red,
                         PanelTheme::background.green, PanelTheme::background.blue);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, PanelTheme::border.red,
                         PanelTheme::border.green, PanelTheme::border.blue);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 0.5, 0.5, backend_->width() - 1.0, backend_->height() - 1.0);
    cairo_stroke(cr);
    calculate_layout(cr);
    for (auto& item : modules_) {
        item.module->render(cr, item.geometry);
    }
    backend_->commit();
    rendered_width_ = backend_->width();
    rendered_height_ = backend_->height();
}

void Panel::handle_click(int x, int y, int button)
{
    for (auto& item : modules_) {
        if (item.geometry.contains(x, y)) {
            item.module->on_click(button, x - item.geometry.x, y - item.geometry.y);
            render_frame();
            return;
        }
    }
}

void Panel::handle_motion(int x, int y)
{
    if (x < 0 || y < 0) {
        for (auto& item : modules_) {
            item.module->on_motion(-1, -1);
        }
        backend_->hide_tooltip();
        return;
    }

    for (const auto& item : modules_) {
        if (!item.geometry.contains(x, y)) {
            continue;
        }
        item.module->on_motion(x - item.geometry.x, y - item.geometry.y);
        const std::string text = item.module->tooltip_text();
        if (text.empty()) {
            backend_->hide_tooltip();
        } else {
            backend_->show_tooltip(text, x, y);
        }
        return;
    }
    backend_->hide_tooltip();
}
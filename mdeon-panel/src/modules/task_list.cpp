#include "panel/modules/task_list.hpp"

#include "panel/icon_theme.hpp"
#include "panel/theme.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>

#include <pango/pangocairo.h>

namespace {

std::string normalized_setting(std::string value)
{
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool setting_bool(const std::map<std::string, std::string>& values,
                  const std::string& key, bool fallback)
{
    const auto found = values.find(key);
    if (found == values.end()) {
        return fallback;
    }
    const std::string value = normalized_setting(found->second);
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

int setting_int(const std::map<std::string, std::string>& values,
                const std::string& key, int fallback)
{
    const auto found = values.find(key);
    if (found == values.end()) {
        return fallback;
    }
    char* end = nullptr;
    const long value = std::strtol(found->second.c_str(), &end, 10);
    return end != found->second.c_str() && *end == '\0'
        ? static_cast<int>(value) : fallback;
}

std::vector<int> setting_ints(const std::map<std::string, std::string>& values,
                              const std::string& key,
                              std::vector<int> fallback)
{
    const auto found = values.find(key);
    if (found == values.end()) {
        return fallback;
    }
    std::istringstream input(found->second);
    std::vector<int> parsed;
    int value = 0;
    while (input >> value) {
        parsed.push_back(value);
    }
    return parsed.empty() ? fallback : parsed;
}

std::string task_tooltip_label(const TaskWindowInfo& window)
{
    if (!window.title.empty() && window.title != "Window") {
        return window.title;
    }
    if (!window.icon_name.empty() && window.icon_name != "application-x-executable") {
        return window.icon_name;
    }
    return window.title.empty() ? "Window" : window.title;
}

RgbColor setting_color(const std::map<std::string, std::string>& values,
                       const std::string& key, RgbColor fallback,
                       double* alpha = nullptr)
{
    const auto found = values.find(key);
    if (found == values.end()) {
        return fallback;
    }
    std::istringstream input(found->second);
    std::string color;
    if (!(input >> color) || color.size() != 7 || color.front() != '#') {
        return fallback;
    }
    try {
        const unsigned long value = std::stoul(color.substr(1), nullptr, 16);
        int opacity = 100;
        input >> opacity;
        if (alpha != nullptr) {
            *alpha = std::clamp(opacity, 0, 100) / 100.0;
        }
        return {((value >> 16) & 0xff) / 255.0,
                ((value >> 8) & 0xff) / 255.0,
                (value & 0xff) / 255.0};
    } catch (const std::exception&) {
        return fallback;
    }
}

}

TaskListModule::TaskListModule(IBackend& backend,
                               std::map<std::string, std::string> config, bool vertical)
    : backend_(backend), config_(std::move(config)), vertical_(vertical)
{
    show_icon_ = setting_bool(config_, "task_icon", true);
    show_text_ = setting_bool(config_, "task_text", true);
    centered_ = setting_bool(config_, "task_centered", true);
    borderless_ = setting_bool(config_, "task_borderless", false);
    const std::vector<int> task_padding = setting_ints(config_, "task_padding", {2, 0, 2});
    padding_x_ = std::max(0, task_padding[0]);
    padding_y_ = std::max(0, task_padding.size() > 1 ? task_padding[1] : 0);
    spacing_ = std::max(0, task_padding.size() > 2 ? task_padding[2] : 2);
    const std::vector<int> maximum = setting_ints(config_, "task_maximum_size", {140, 30});
    max_label_width_ = std::max(24, maximum[0]);
    const int panel_height = std::max(24, setting_int(config_, "panel_thickness", 30));
    const std::vector<int> panel_padding = setting_ints(config_, "panel_padding", {0, 0, 0});
    const int usable_height = std::max(12, panel_height - 2 *
        std::max(0, panel_padding.size() > 1 ? panel_padding[1] : 0) - 2);
    const int requested_icon_size = setting_int(config_, "task_icon_size", 0);
    icon_size_ = requested_icon_size <= 0 ? usable_height :
        std::max(12, std::min(requested_icon_size, usable_height));
    text_color_ = setting_color(config_, "task_font_color", text_color_);
    active_text_color_ = setting_color(config_, "task_active_font_color", active_text_color_);
    iconified_text_color_ = setting_color(config_, "task_iconified_font_color",
                                           iconified_text_color_, &iconified_text_alpha_);
    const auto font = config_.find("task_font");
    if (font != config_.end() && !font->second.empty()) {
        font_name_ = font->second;
    }
}

bool TaskListModule::update()
{
    const auto now = std::chrono::steady_clock::now();
    if (now < next_refresh_) {
        return false;
    }
    next_refresh_ = now + std::chrono::seconds(1);
    const auto current = backend_.list_task_windows();
    bool changed = current.size() != tasks_.size();
    if (!changed) {
        for (std::size_t index = 0; index < current.size(); ++index) {
            const auto& previous = tasks_[index].window;
            const auto& next = current[index];
            if (previous.id != next.id || previous.title != next.title ||
                previous.icon_name != next.icon_name || previous.icon_hash != next.icon_hash ||
                previous.active != next.active ||
                previous.iconified != next.iconified || previous.maximized != next.maximized) {
                changed = true;
                break;
            }
        }
    }
    if (changed) {
        tasks_.clear();
        tasks_.reserve(current.size());
        for (const auto& window : current) {
            tasks_.push_back({window, {}});
        }
    }
    return changed;
}

int TaskListModule::task_width(cairo_t* cr, const TaskWindowInfo& window, bool compact) const
{
    if (compact) {
        return icon_size_ + 10;
    }
    if (!show_text_) {
        return show_icon_ ? icon_size_ + 10 : 34;
    }
    PangoLayout* layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, window.title.c_str(), -1);
    PangoFontDescription* font = pango_font_description_from_string(font_name_.c_str());
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    int text_width = 0;
    pango_layout_get_pixel_size(layout, &text_width, nullptr);
    g_object_unref(layout);
    const int label_width = std::min(max_label_width_, std::max(16, text_width));
    return std::max(34, label_width + (show_icon_ ? icon_size_ + 16 : 12));
}

int TaskListModule::measure_width(cairo_t* cr, int)
{
    if (tasks_.empty()) {
        visible_task_count_ = 0;
        return 0;
    }
    if (vertical_) {
        compact_mode_ = false;
        const int item_extent = std::max(24, icon_size_ + 2 * padding_y_ + 2);
        const int stride = item_extent + spacing_;
        visible_task_count_ = available_width_ < item_extent ? 0 :
            std::min(tasks_.size(), static_cast<std::size_t>(
                (available_width_ + spacing_) / stride));
        return visible_task_count_ == 0 ? 0 :
            static_cast<int>(visible_task_count_) * stride - spacing_;
    }
    const int horizontal_margin = std::min(1, padding_x_);
    const auto total_width = [&](bool compact, std::size_t count) {
        if (count == 0) return 0;
        int total = spacing_ * static_cast<int>(count - 1);
        for (std::size_t index = 0; index < count; ++index) {
            total += task_width(cr, tasks_[index].window, compact) + 2 * horizontal_margin;
        }
        return total;
    };

    compact_mode_ = total_width(false, tasks_.size()) > available_width_;
    if (!compact_mode_) {
        visible_task_count_ = tasks_.size();
        return total_width(false, visible_task_count_);
    }

    const int compact_item_width = task_width(cr, tasks_.front().window, true) +
        2 * horizontal_margin;
    const int compact_stride = compact_item_width + spacing_;
    const int fitting_count = available_width_ < compact_item_width ? 0 :
        (available_width_ + spacing_) / compact_stride;
    visible_task_count_ = std::min(tasks_.size(), static_cast<std::size_t>(fitting_count));
    return total_width(true, visible_task_count_);
}

void TaskListModule::set_available_width(int width)
{
    available_width_ = std::max(0, width);
}

void TaskListModule::render(cairo_t* cr, const Geometry& geometry)
{
    geometry_ = geometry;
    int x = geometry.x;
    int y = geometry.y;
    for (std::size_t index = 0; index < tasks_.size(); ++index) {
        auto& task = tasks_[index];
        if (index >= visible_task_count_) {
            task.geometry = {};
            continue;
        }
        const int width = vertical_ ? std::max(1, geometry.width - 2 * padding_x_)
                                    : task_width(cr, task.window, compact_mode_);
        const int horizontal_margin = std::min(1, padding_x_);
        const int item_extent = std::max(24, icon_size_ + 2 * padding_y_ + 2);
        task.geometry = vertical_
            ? Geometry{geometry.x + horizontal_margin, y, width, item_extent, true}
            : Geometry{x + horizontal_margin, geometry.y + padding_y_, width,
                       std::max(1, geometry.height - 2 * padding_y_), false};
        const bool active = task.window.active;
        const RgbColor background = active
            ? RgbColor{221.0 / 255.0, 221.0 / 255.0, 221.0 / 255.0}
            : RgbColor{238.0 / 255.0, 238.0 / 255.0, 238.0 / 255.0};
        const RgbColor border = active
            ? RgbColor{153.0 / 255.0, 153.0 / 255.0, 153.0 / 255.0}
            : RgbColor{238.0 / 255.0, 238.0 / 255.0, 238.0 / 255.0};
        cairo_set_source_rgb(cr, background.red, background.green, background.blue);
        cairo_rectangle(cr, task.geometry.x + 0.5, task.geometry.y + 0.5,
                        width - 1.0, task.geometry.height - 1.0);
        cairo_fill_preserve(cr);
        if (!borderless_) {
            cairo_set_source_rgb(cr, border.red, border.green, border.blue);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
        } else {
            cairo_new_path(cr);
        }

        const double icon_center_x = vertical_
            ? task.geometry.x + task.geometry.width / 2.0
            : task.geometry.x + 4.0 + icon_size_ / 2.0;
        const double icon_center_y = task.geometry.y + task.geometry.height / 2.0;
        const bool display_icon = show_icon_ || compact_mode_;
        const bool display_text = show_text_ && !compact_mode_ && !vertical_;
        if (display_icon && !task.window.icon_argb.empty() && task.window.icon_width > 0 &&
            task.window.icon_height > 0) {
            const int stride = static_cast<int>(task.window.icon_width * sizeof(uint32_t));
            cairo_surface_t* icon_surface = cairo_image_surface_create_for_data(
                reinterpret_cast<unsigned char*>(task.window.icon_argb.data()),
                CAIRO_FORMAT_ARGB32, static_cast<int>(task.window.icon_width),
                static_cast<int>(task.window.icon_height), stride);
            if (cairo_surface_status(icon_surface) == CAIRO_STATUS_SUCCESS) {
                cairo_save(cr);
                cairo_translate(cr, icon_center_x - icon_size_ / 2.0,
                                icon_center_y - icon_size_ / 2.0);
                cairo_scale(cr, static_cast<double>(icon_size_) / task.window.icon_width,
                            static_cast<double>(icon_size_) / task.window.icon_height);
                cairo_set_source_surface(cr, icon_surface, 0.0, 0.0);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
                cairo_paint(cr);
                cairo_restore(cr);
            }
            cairo_surface_destroy(icon_surface);
        } else if (display_icon) {
            paint_theme_icon(cr, task.window.icon_name.empty()
                ? "application-x-executable" : task.window.icon_name,
                icon_center_x, icon_center_y, icon_size_);
        }

        if (!display_text) {
            if (vertical_) y += item_extent + spacing_;
            else x += width + 2 * horizontal_margin + spacing_;
            continue;
        }
        PangoLayout* layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, task.window.title.c_str(), -1);
        PangoFontDescription* font = pango_font_description_from_string(font_name_.c_str());
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        const int text_x = task.geometry.x + (display_icon ? icon_size_ + 8 : 6);
        const int text_width = std::max(1, task.geometry.x + width - text_x - 4);
        int natural_text_width = 0;
        int text_height = 0;
        pango_layout_get_pixel_size(layout, &natural_text_width, &text_height);
        pango_layout_set_width(layout, text_width * PANGO_SCALE);
        cairo_save(cr);
        cairo_rectangle(cr, text_x, task.geometry.y, text_width, task.geometry.height);
        cairo_clip(cr);
        if (task.window.active) {
            cairo_set_source_rgb(cr, active_text_color_.red, active_text_color_.green,
                                 active_text_color_.blue);
        } else if (task.window.iconified) {
            cairo_set_source_rgba(cr, iconified_text_color_.red,
                                  iconified_text_color_.green,
                                  iconified_text_color_.blue, iconified_text_alpha_);
        } else {
            cairo_set_source_rgb(cr, text_color_.red, text_color_.green, text_color_.blue);
        }
        const int centered_x = centered_ && text_width > 0
            ? text_x + std::max(0, (text_width - std::min(text_width, natural_text_width)) / 2)
            : text_x;
        cairo_move_to(cr, centered_x,
                      task.geometry.y + (task.geometry.height - text_height) / 2.0);
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);
        g_object_unref(layout);
        if (vertical_) y += item_extent + spacing_;
        else x += width + 2 * horizontal_margin + spacing_;
    }
}

void TaskListModule::on_click(int button, int rel_x, int rel_y)
{
    const int panel_x = geometry_.x + rel_x;
    const int panel_y = geometry_.y + rel_y;
    const auto found = std::find_if(tasks_.begin(), tasks_.end(), [panel_x, panel_y](const TaskEntry& task) {
        return task.geometry.contains(panel_x, panel_y);
    });
    if (found == tasks_.end()) {
        return;
    }
    const char* setting_key = button == 1 ? "mouse_left" :
        button == 2 ? "mouse_middle" : button == 3 ? "mouse_right" : nullptr;
    if (setting_key == nullptr) {
        return;
    }
    const auto configured = config_.find(setting_key);
    const std::string action = normalized_setting(configured != config_.end()
        ? configured->second : button == 1 ? "toggle_iconify" :
          button == 3 ? "menu" : "none");
    if (action == "none" || action.empty()) {
        return;
    }
    if (action == "menu" || action == "context_menu" || action == "alternative") {
        if (backend_.supports_task_context_menu()) {
            backend_.show_task_menu(found->window.id, panel_x, panel_y,
                                    found->window.maximized);
        }
    } else if (action == "activate") {
        backend_.perform_task_action(found->window.id, TaskAction::Activate);
    } else if (action == "iconify" || action == "minimize") {
        backend_.perform_task_action(found->window.id, TaskAction::Minimize);
    } else if (action == "close") {
        backend_.perform_task_action(found->window.id, TaskAction::Close);
    } else if (action == "maximize" || action == "toggle_maximize") {
        backend_.perform_task_action(found->window.id, TaskAction::ToggleMaximize);
    } else if (action == "toggle_iconify" || action == "toggle") {
        backend_.perform_task_action(found->window.id, TaskAction::ToggleMinimize);
    }
}

void TaskListModule::on_motion(int rel_x, int rel_y)
{
    const int panel_x = geometry_.x + rel_x;
    const int panel_y = geometry_.y + rel_y;
    const auto found = std::find_if(tasks_.begin(), tasks_.end(), [panel_x, panel_y](const TaskEntry& task) {
        return task.geometry.contains(panel_x, panel_y);
    });
    if (found == tasks_.end()) {
        hovered_task_title_.clear();
        return;
    }
    hovered_task_title_ = task_tooltip_label(found->window);
}

std::string TaskListModule::tooltip_text() const
{
    if (!hovered_task_title_.empty()) {
        return hovered_task_title_;
    }
    return backend_.supports_task_context_menu()
        ? "Open windows\nLeft-click: toggle minimize\nMiddle-click: close\nRight-click: window actions"
        : "Open windows\nLeft-click: toggle minimize\nMiddle-click: close";
}
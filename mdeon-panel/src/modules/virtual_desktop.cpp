#include "panel/modules/virtual_desktop.hpp"

#include "panel/icon_theme.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>

#include <pango/pangocairo.h>

VirtualDesktopModule::VirtualDesktopModule(IBackend& backend, int workspace_count,
                                           std::string display_mode, int icon_size)
    : backend_(backend), configured_count_(std::max(1, workspace_count)),
      display_mode_(std::move(display_mode)), icon_size_(std::max(12, icon_size))
{
    if (display_mode_ != "icon" && display_mode_ != "number" &&
        display_mode_ != "icon_number") {
        display_mode_ = "icon_number";
    }
}

bool VirtualDesktopModule::update()
{
    const auto now = std::chrono::steady_clock::now();
    if (now < next_refresh_) {
        return false;
    }
    next_refresh_ = now + std::chrono::seconds(2);
    const int previous_current = current_desktop_;
    const int previous_count = desktop_count_;
    const bool previous_available = available_;
    const bool requested_change = pending_change_;
    pending_change_ = false;

    if (!count_configured_) {
        backend_.configure_workspace_count(configured_count_);
        count_configured_ = true;
    }
    const WorkspaceSnapshot snapshot = backend_.get_workspaces();
    workspace_ids_ = snapshot.ids;
    desktop_count_ = static_cast<int>(workspace_ids_.size());
    available_ = desktop_count_ > 0;
    if (available_) {
        const auto current = std::find(workspace_ids_.begin(), workspace_ids_.end(),
                                       snapshot.current_id);
        current_desktop_ = current == workspace_ids_.end() ? 0 :
            static_cast<int>(current - workspace_ids_.begin());
    }
    return current_desktop_ != previous_current || desktop_count_ != previous_count ||
        available_ != previous_available || requested_change;
}

int VirtualDesktopModule::measure_width(cairo_t* cr, int)
{
    PangoLayout* layout = pango_cairo_create_layout(cr);
    const std::string number = std::to_string(current_desktop_ + 1);
    pango_layout_set_text(layout, number.c_str(), -1);
    PangoFontDescription* font = pango_font_description_from_string("Sans 8");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    int text_width = 0;
    pango_layout_get_pixel_size(layout, &text_width, nullptr);
    g_object_unref(layout);
    return (display_mode_ == "icon" || display_mode_ == "icon_number" ? icon_size_ + 6 : 0) +
        (display_mode_ == "number" || display_mode_ == "icon_number" ? text_width + 6 : 0);
}

void VirtualDesktopModule::render(cairo_t* cr, const Geometry& geometry)
{
    const double center_y = geometry.y + geometry.height / 2.0;
    if (geometry.vertical) {
        if (display_mode_ != "number") {
            const std::filesystem::path custom_icon("/usr/share/icons/virtual-screen.png");
            paint_theme_icon(cr, std::filesystem::exists(custom_icon)
                ? custom_icon.string() : "view-grid-symbolic",
                geometry.x + geometry.width / 2.0, center_y,
                std::min(icon_size_, std::max(12, geometry.width - 4)));
            return;
        }
        PangoLayout* layout = pango_cairo_create_layout(cr);
        const std::string number = available_ ? std::to_string(current_desktop_ + 1) : "?";
        pango_layout_set_text(layout, number.c_str(), -1);
        PangoFontDescription* font = pango_font_description_from_string("Sans 8");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        int text_width = 0;
        int text_height = 0;
        pango_layout_get_pixel_size(layout, &text_width, &text_height);
        cairo_set_source_rgb(cr, 34.0 / 255.0, 34.0 / 255.0, 34.0 / 255.0);
        cairo_move_to(cr, geometry.x + (geometry.width - text_width) / 2.0,
                      center_y - text_height / 2.0);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
        return;
    }
    int content_x = geometry.x;
    if (display_mode_ == "icon" || display_mode_ == "icon_number") {
        const std::filesystem::path custom_icon("/usr/share/icons/virtual-screen.png");
        paint_theme_icon(cr, std::filesystem::exists(custom_icon)
            ? custom_icon.string() : "view-grid-symbolic",
            content_x + icon_size_ / 2.0 + 3.0, center_y, icon_size_);
        content_x += icon_size_ + 6;
    }

    if (display_mode_ == "number" || display_mode_ == "icon_number") {
        PangoLayout* layout = pango_cairo_create_layout(cr);
        const std::string number = available_ ? std::to_string(current_desktop_ + 1) : "?";
        pango_layout_set_text(layout, number.c_str(), -1);
        PangoFontDescription* font = pango_font_description_from_string("Sans 8");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        int text_height = 0;
        pango_layout_get_pixel_size(layout, nullptr, &text_height);
        cairo_set_source_rgb(cr, 34.0 / 255.0, 34.0 / 255.0, 34.0 / 255.0);
        cairo_move_to(cr, content_x, center_y - text_height / 2.0);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }
}

void VirtualDesktopModule::on_click(int button, int, int)
{
    if (button == 1) {
        switch_desktop(1);
    } else if (button == 4) {
        switch_desktop(-1);
    } else if (button == 5) {
        switch_desktop(1);
    }
}

std::string VirtualDesktopModule::tooltip_text() const
{
    if (!available_) {
        return "Virtual desktops unavailable";
    }
    return "Desktop " + std::to_string(current_desktop_ + 1) + " of " +
        std::to_string(desktop_count_) + "\nLeft-click: next desktop\nScroll: switch desktops";
}

void VirtualDesktopModule::switch_desktop(int offset)
{
    if (!available_ || desktop_count_ < 1) {
        return;
    }
    const int target = (current_desktop_ + offset + desktop_count_) % desktop_count_;
    if (backend_.switch_workspace(workspace_ids_[static_cast<std::size_t>(target)])) {
        current_desktop_ = target;
        pending_change_ = true;
        next_refresh_ = {};
    }
}
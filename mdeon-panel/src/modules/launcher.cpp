#include "panel/modules/launcher.hpp"

#include "panel/icon_theme.hpp"

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

#include <algorithm>
#include <filesystem>

LauncherModule::LauncherModule(std::vector<std::string> desktop_files, int icon_size,
                               int max_icons, bool tooltips, bool vertical)
        : icon_size_(std::max(12, icon_size)), max_icons_(std::max(1, max_icons)),
            tooltips_(tooltips), vertical_(vertical)
{
    for (const auto& path : desktop_files) {
        GKeyFile* key_file = g_key_file_new();
        if (!g_key_file_load_from_file(key_file, path.c_str(), G_KEY_FILE_NONE, nullptr)) {
            g_key_file_unref(key_file);
            continue;
        }
        gchar* name = g_key_file_get_locale_string(
            key_file, "Desktop Entry", "Name", nullptr, nullptr);
        gchar* icon = g_key_file_get_string(key_file, "Desktop Entry", "Icon", nullptr);
        entries_.push_back({path, name != nullptr ? name : std::filesystem::path(path).stem().string(),
                            icon != nullptr ? icon : "application-x-executable"});
        g_free(name);
        g_free(icon);
        g_key_file_unref(key_file);
    }
}

bool LauncherModule::update()
{
    return false;
}

int LauncherModule::measure_width(cairo_t*, int)
{
    const std::size_t visible_count = expanded_ ? entries_.size() :
        std::min(entries_.size(), static_cast<std::size_t>(max_icons_));
    const int arrow_width = entries_.size() > static_cast<std::size_t>(max_icons_)
        ? icon_size_ + 8 : 0;
    return static_cast<int>(visible_count) * (icon_size_ + 8) + arrow_width;
}

void LauncherModule::render(cairo_t* cr, const Geometry& geometry)
{
    const int cell_width = icon_size_ + 8;
    const std::size_t visible_count = expanded_ ? entries_.size() :
        std::min(entries_.size(), static_cast<std::size_t>(max_icons_));
    for (std::size_t index = 0; index < visible_count; ++index) {
        const double center_x = vertical_ ? geometry.x + geometry.width / 2.0
            : geometry.x + static_cast<double>(index * cell_width) + cell_width / 2.0;
        const double center_y = vertical_
            ? geometry.y + static_cast<double>(index * cell_width) + cell_width / 2.0
            : geometry.y + geometry.height / 2.0;
        paint_theme_icon(cr, entries_[index].icon, center_x, center_y, icon_size_);
    }
    if (entries_.size() > static_cast<std::size_t>(max_icons_)) {
        const double center_x = vertical_ ? geometry.x + geometry.width / 2.0
            : geometry.x + static_cast<double>(visible_count * cell_width) + cell_width / 2.0;
        const double center_y = vertical_
            ? geometry.y + static_cast<double>(visible_count * cell_width) + cell_width / 2.0
            : geometry.y + geometry.height / 2.0;
        const double direction = expanded_ ? -1.0 : 1.0;
        cairo_set_source_rgb(cr, 0.25, 0.27, 0.30);
        cairo_set_line_width(cr, 2.0);
        if (vertical_) {
            cairo_move_to(cr, center_x - 5.0, center_y - direction * 3.0);
            cairo_line_to(cr, center_x, center_y + direction * 3.0);
            cairo_line_to(cr, center_x + 5.0, center_y - direction * 3.0);
        } else {
            cairo_move_to(cr, center_x - direction * 3.0, center_y - 5.0);
            cairo_line_to(cr, center_x + direction * 3.0, center_y);
            cairo_line_to(cr, center_x - direction * 3.0, center_y + 5.0);
        }
        cairo_stroke(cr);
    }
}

void LauncherModule::on_click(int button, int rel_x, int rel_y)
{
    const int position = vertical_ ? rel_y : rel_x;
    if (button != 1 || position < 0) {
        return;
    }
    const int cell_width = icon_size_ + 8;
    if (entries_.size() > static_cast<std::size_t>(max_icons_)) {
        const std::size_t visible_count = expanded_ ? entries_.size() :
            std::min(entries_.size(), static_cast<std::size_t>(max_icons_));
        if (static_cast<std::size_t>(position / cell_width) == visible_count) {
            expanded_ = !expanded_;
            return;
        }
    }
    const std::size_t index = static_cast<std::size_t>(position / cell_width);
    const std::size_t visible_count = expanded_ ? entries_.size() :
        std::min(entries_.size(), static_cast<std::size_t>(max_icons_));
    if (index >= visible_count) {
        return;
    }
    GDesktopAppInfo* app = g_desktop_app_info_new_from_filename(entries_[index].path.c_str());
    if (app == nullptr) {
        return;
    }
    g_app_info_launch(G_APP_INFO(app), nullptr, nullptr, nullptr);
    g_object_unref(app);
    expanded_ = false;
}

void LauncherModule::on_motion(int rel_x, int rel_y)
{
    const int position = vertical_ ? rel_y : rel_x;
    const int cell_width = icon_size_ + 8;
    if (position < 0) {
        hover_index_ = -1;
        return;
    }
    const std::size_t visible_count = expanded_ ? entries_.size() :
        std::min(entries_.size(), static_cast<std::size_t>(max_icons_));
    const int hovered_cell = position / cell_width;
    hover_index_ = static_cast<std::size_t>(hovered_cell) < visible_count
        ? hovered_cell : -1;
}

std::string LauncherModule::tooltip_text() const
{
    if (!tooltips_ || hover_index_ < 0 ||
        static_cast<std::size_t>(hover_index_) >= entries_.size()) {
        return {};
    }
    return entries_[static_cast<std::size_t>(hover_index_)].name;
}
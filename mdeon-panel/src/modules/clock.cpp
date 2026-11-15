#include "panel/modules/clock.hpp"

#include "panel/theme.hpp"

#include <pango/pangocairo.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace {

PangoLayout* create_clock_layout(cairo_t* cr, const std::string& text, const char* font_name)
{
    PangoLayout* layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text.c_str(), -1);
    PangoFontDescription* font = pango_font_description_from_string(font_name);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    return layout;
}

}

ClockModule::ClockModule(std::function<void(int, int)> on_activate, bool vertical)
    : on_activate_(std::move(on_activate)), vertical_(vertical)
{
}

bool ClockModule::update()
{
    const std::time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm local_time{};
    localtime_r(&now, &local_time);
    std::ostringstream formatted;
    formatted << std::put_time(&local_time, "%H:%M");
    const std::string new_time = formatted.str();
    std::ostringstream date;
    date << std::put_time(&local_time, "%A %d %B");
    const std::string new_date = date.str();
    const bool changed = new_time != cached_time_ || new_date != cached_date_;
    cached_time_ = new_time;
    cached_date_ = new_date;
    return changed;
}

int ClockModule::measure_width(cairo_t* cr, int)
{
    PangoLayout* time_layout = create_clock_layout(cr, cached_time_, "Sans Bold 8");
    PangoLayout* date_layout = create_clock_layout(cr, cached_date_, "Sans 7");
    int time_width = 0;
    int date_width = 0;
    pango_layout_get_pixel_size(time_layout, &time_width, nullptr);
    pango_layout_get_pixel_size(date_layout, &date_width, nullptr);
    if (vertical_) {
        int time_height = 0;
        pango_layout_get_pixel_size(time_layout, nullptr, &time_height);
        g_object_unref(time_layout);
        g_object_unref(date_layout);
        return time_height + 2;
    }
    g_object_unref(time_layout);
    g_object_unref(date_layout);
    return std::max(time_width, date_width) + 2;
}

void ClockModule::render(cairo_t* cr, const Geometry& geometry)
{
    geometry_ = geometry;
    PangoLayout* time_layout = create_clock_layout(cr, cached_time_, "Sans Bold 8");
    PangoLayout* date_layout = create_clock_layout(cr, cached_date_, "Sans 7");
    int time_width = 0;
    int time_height = 0;
    int date_width = 0;
    int date_height = 0;
    pango_layout_get_pixel_size(time_layout, &time_width, &time_height);
    pango_layout_get_pixel_size(date_layout, &date_width, &date_height);
    cairo_set_source_rgb(cr, PanelTheme::text.red, PanelTheme::text.green, PanelTheme::text.blue);
    if (vertical_) {
        cairo_move_to(cr, geometry.x + (geometry.width - time_width) / 2.0,
                      geometry.y + (geometry.height - time_height) / 2.0);
        pango_cairo_show_layout(cr, time_layout);
        g_object_unref(time_layout);
        g_object_unref(date_layout);
        return;
    }
    const int total_height = time_height + date_height + 1;
    const double top = geometry.y + (geometry.height - total_height) / 2.0;
    cairo_move_to(cr, geometry.x + (geometry.width - time_width) / 2.0, top);
    pango_cairo_show_layout(cr, time_layout);
    cairo_move_to(cr, geometry.x + (geometry.width - date_width) / 2.0,
                  top + time_height + 1);
    pango_cairo_show_layout(cr, date_layout);
    g_object_unref(time_layout);
    g_object_unref(date_layout);
}

void ClockModule::on_click(int button, int, int)
{
    if (button == 1 && on_activate_) {
        on_activate_(vertical_ ? geometry_.y : geometry_.x,
                 vertical_ ? geometry_.height : geometry_.width);
    }
}
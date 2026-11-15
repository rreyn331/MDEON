#include "panel/modules/battery.hpp"

#include "panel/icon_theme.hpp"
#include "panel/theme.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include <pango/pangocairo.h>

namespace {

PangoLayout* create_label(cairo_t* cr, const std::string& text)
{
    PangoLayout* layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text.c_str(), -1);
    PangoFontDescription* font = pango_font_description_from_string("Sans 8");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    return layout;
}

}

bool BatteryModule::update()
{
    const auto now = std::chrono::steady_clock::now();
    if (now < next_refresh_) {
        return false;
    }
    const int previous_capacity = capacity_;
    const bool previous_available = available_;
    const bool previous_plugged_in = plugged_in_;
    const bool previous_charging = charging_;
    next_refresh_ = now + std::chrono::seconds(60);
    available_ = false;
    bool external_power = false;
    std::string battery_status;

    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/power_supply", error)) {
        std::ifstream type_file(entry.path() / "type");
        std::string type;
        if (!std::getline(type_file, type)) {
            continue;
        }

        if (type == "Mains") {
            std::ifstream online_file(entry.path() / "online");
            int online = 0;
            external_power = external_power || (online_file >> online && online != 0);
            continue;
        }
        if (type != "Battery") {
            continue;
        }
        if (available_) {
            continue;
        }

        std::ifstream status_file(entry.path() / "status");
        std::getline(status_file, battery_status);
        std::ifstream capacity_file(entry.path() / "capacity");
        int capacity = 0;
        if (capacity_file >> capacity) {
            capacity_ = std::clamp(capacity, 0, 100);
            available_ = true;
        }
    }
    charging_ = battery_status == "Charging";
    plugged_in_ = external_power || charging_;
    return capacity_ != previous_capacity || available_ != previous_available ||
        plugged_in_ != previous_plugged_in || charging_ != previous_charging;
}

int BatteryModule::measure_width(cairo_t* cr, int)
{
    if (!available_) {
        return 0;
    }
    if (vertical_) {
        return 30;
    }

    const std::string label = std::to_string(capacity_) + "%";
    PangoLayout* layout = create_label(cr, label);
    int text_width = 0;
    pango_layout_get_pixel_size(layout, &text_width, nullptr);
    g_object_unref(layout);
    return text_width + 42;
}

void BatteryModule::render(cairo_t* cr, const Geometry& geometry)
{
    if (!available_) {
        return;
    }

    const int level = std::clamp((capacity_ / 10) * 10, 0, 100);
    std::string icon_name = "battery-level-" + std::to_string(level);
    if (charging_) {
        icon_name += "-charging";
    } else if (capacity_ >= 100) {
        icon_name += "-charged";
    }
    icon_name += "-symbolic";
    paint_theme_icon(cr, icon_name,
        vertical_ ? geometry.x + geometry.width / 2.0 : geometry.x + 14.0,
        geometry.y + geometry.height / 2.0, 28);

    if (vertical_) {
        return;
    }

    PangoLayout* layout = create_label(cr, std::to_string(capacity_) + "%");
    int text_width = 0;
    int text_height = 0;
    pango_layout_get_pixel_size(layout, &text_width, &text_height);
    cairo_set_source_rgb(cr, PanelTheme::text.red, PanelTheme::text.green, PanelTheme::text.blue);
    cairo_move_to(cr, geometry.x + 34.0,
                  geometry.y + (geometry.height - text_height) / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);

}

void BatteryModule::on_click(int, int, int)
{
}

std::string BatteryModule::tooltip_text() const
{
    if (!available_) {
        return {};
    }
    const std::string status = charging_ ? "Charging" :
        (plugged_in_ ? "Plugged in" : "On battery");
    return status + " - " + std::to_string(capacity_) + "%";
}
#include "panel/modules/custom_button.hpp"

#include "panel/icon_theme.hpp"
#include "panel/modules/system_commands.hpp"
#include "panel/theme.hpp"

#include <algorithm>

#include <pango/pangocairo.h>

CustomButtonModule::CustomButtonModule(std::map<std::string, std::string> config,
                                       int number, int icon_size)
    : config_(std::move(config)), number_(number), icon_size_(std::max(12, icon_size))
{
}

bool CustomButtonModule::update()
{
    return false;
}

int CustomButtonModule::measure_width(cairo_t* cr, int)
{
    const std::string text = configured("button_text");
    const std::string icon = configured("button_icon");
    if (text.empty() && icon.empty()) {
        return icon_size_ + 6;
    }
    if (text.empty()) {
        return icon_size_ + 6;
    }

    PangoLayout* layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text.c_str(), -1);
    PangoFontDescription* font = pango_font_description_from_string("Sans 8");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    int text_width = 0;
    pango_layout_get_pixel_size(layout, &text_width, nullptr);
    g_object_unref(layout);
    return text_width + (icon.empty() ? 10 : icon_size_ + 8);
}

void CustomButtonModule::render(cairo_t* cr, const Geometry& geometry)
{
    const std::string icon = configured("button_icon");
    std::string text = configured("button_text");
    if (icon.empty() && text.empty()) {
        text = "B" + std::to_string(number_);
    }

    const bool has_icon = !icon.empty();
    const bool has_text = !text.empty();
    if (geometry.vertical) {
        if (has_icon) {
            paint_theme_icon(cr, icon, geometry.x + geometry.width / 2.0,
                geometry.y + geometry.height / 2.0,
                std::min(icon_size_, std::max(12, geometry.width - 4)));
            return;
        }
        PangoLayout* layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, text.c_str(), -1);
        PangoFontDescription* font = pango_font_description_from_string("Sans 8");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        pango_layout_set_width(layout, std::max(1, geometry.width - 4) * PANGO_SCALE);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        int text_width = 0;
        int text_height = 0;
        pango_layout_get_pixel_size(layout, &text_width, &text_height);
        cairo_set_source_rgb(cr, PanelTheme::text.red, PanelTheme::text.green,
                             PanelTheme::text.blue);
        cairo_move_to(cr, geometry.x + (geometry.width - text_width) / 2.0,
                      geometry.y + (geometry.height - text_height) / 2.0);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
        return;
    }
    const double center_y = geometry.y + geometry.height / 2.0;
    const double content_width = geometry.width;
    double cursor = geometry.x + (has_icon && has_text ? 2.0 :
                                  has_icon ? (content_width - icon_size_) / 2.0 : 0.0);
    if (has_icon) {
        paint_theme_icon(cr, icon, cursor + icon_size_ / 2.0, center_y, icon_size_);
        cursor += icon_size_ + (has_text ? 3.0 : 0.0);
    }
    if (has_text) {
        PangoLayout* layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, text.c_str(), -1);
        PangoFontDescription* font = pango_font_description_from_string("Sans 8");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        int text_width = 0;
        int text_height = 0;
        pango_layout_get_pixel_size(layout, &text_width, &text_height);
        if (!has_icon) {
            cursor = geometry.x + (geometry.width - text_width) / 2.0;
        }
        cairo_set_source_rgb(cr, PanelTheme::text.red, PanelTheme::text.green, PanelTheme::text.blue);
        cairo_move_to(cr, cursor, center_y - text_height / 2.0);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }
}

void CustomButtonModule::on_click(int button, int, int)
{
    switch (button) {
    case 1:
        run_command("button_lclick_command");
        break;
    case 2:
        run_command("button_mclick_command");
        break;
    case 3:
        run_command("button_rclick_command");
        break;
    case 4:
        run_command("button_uwheel_command");
        break;
    case 5:
        run_command("button_dwheel_command");
        break;
    default:
        break;
    }
}

std::string CustomButtonModule::tooltip_text() const
{
    const std::string tooltip = configured("button_tooltip");
    return tooltip.empty() ? "Button " + std::to_string(number_) : tooltip;
}

std::string CustomButtonModule::configured(const std::string& key) const
{
    const auto value = config_.find(key);
    return value == config_.end() ? std::string{} : value->second;
}

void CustomButtonModule::run_command(const std::string& key) const
{
    SystemCommands::launch_shell(configured(key));
}
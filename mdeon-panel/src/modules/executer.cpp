#include "panel/modules/executer.hpp"

#include "panel/modules/system_commands.hpp"
#include "panel/theme.hpp"

#include <pango/pangocairo.h>

ExecuterModule::ExecuterModule(std::map<std::string, std::string> config)
    : config_(std::move(config))
{
}

bool ExecuterModule::update()
{
    return false;
}

int ExecuterModule::measure_width(cairo_t* cr, int)
{
    const std::string text = configured("executer_text");
    if (text.empty()) {
        return 24;
    }
    PangoLayout* layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text.c_str(), -1);
    PangoFontDescription* font = pango_font_description_from_string("Sans 8");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    int width = 0;
    pango_layout_get_pixel_size(layout, &width, nullptr);
    g_object_unref(layout);
    return width + 14;
}

void ExecuterModule::render(cairo_t* cr, const Geometry& geometry)
{
    std::string text = configured("executer_text");
    if (text.empty()) {
        text = ">_";
    }
    PangoLayout* layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text.c_str(), -1);
    PangoFontDescription* font = pango_font_description_from_string("Sans 8");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    int width = 0;
    int height = 0;
    pango_layout_get_pixel_size(layout, &width, &height);
    cairo_set_source_rgb(cr, PanelTheme::text.red, PanelTheme::text.green, PanelTheme::text.blue);
    cairo_move_to(cr, geometry.x + (geometry.width - width) / 2.0,
                  geometry.y + (geometry.height - height) / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

void ExecuterModule::on_click(int button, int, int)
{
    if (button == 1) {
        SystemCommands::launch_shell(configured("executer_command"));
    }
}

std::string ExecuterModule::tooltip_text() const
{
    return configured("executer_tooltip", "Executor");
}

std::string ExecuterModule::configured(const std::string& key,
                                       const std::string& fallback) const
{
    const auto value = config_.find(key);
    return value == config_.end() ? fallback : value->second;
}
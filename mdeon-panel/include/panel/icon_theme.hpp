#pragma once

#include <cairo.h>

#include <functional>
#include <string>

void paint_theme_icon(cairo_t* cr, const std::string& icon_name,
                      double center_x, double center_y, int size);
void set_theme_icon_redraw_callback(std::function<void()> callback);
void warm_theme_icon_index();
[[nodiscard]] bool theme_icon_index_ready();
[[nodiscard]] std::string resolve_theme_icon_name(const std::string& icon_name);
[[nodiscard]] std::string resolve_application_icon(const std::string& app_id);
#include "panel/modules/spacer.hpp"

#include "panel/theme.hpp"

#include <algorithm>

SpacerModule::SpacerModule(int length, bool show_bar, bool bar_at_start,
                                                     bool bar_at_middle, bool bar_at_end)
    : length_(std::clamp(length, 0, 2000)), show_bar_(show_bar),
            bar_at_start_(bar_at_start), bar_at_middle_(bar_at_middle),
            bar_at_end_(bar_at_end)
{
}

bool SpacerModule::update()
{
    return false;
}

int SpacerModule::measure_width(cairo_t*, int)
{
    if (expands()) {
        return 0;
    }
    const int marker_width = show_bar_ && (bar_at_start_ || bar_at_middle_ || bar_at_end_)
        ? 8 : 0;
    return std::min(std::max(length_, marker_width), available_width_);
}

void SpacerModule::set_available_width(int width)
{
    available_width_ = std::max(0, width);
}

bool SpacerModule::expands() const
{
    return length_ == 0 && !show_bar_;
}

void SpacerModule::render(cairo_t* cr, const Geometry& geometry)
{
    if (!show_bar_) {
        return;
    }
    cairo_set_source_rgba(cr, PanelTheme::text.red, PanelTheme::text.green,
                          PanelTheme::text.blue, 0.55);
    cairo_set_line_width(cr, 1.0);
    if (geometry.vertical) {
        const double center_x = geometry.x + geometry.width / 2.0;
        if (bar_at_start_) {
            cairo_move_to(cr, center_x - geometry.width * 0.25, geometry.y + 3.5);
            cairo_line_to(cr, center_x + geometry.width * 0.25, geometry.y + 3.5);
            cairo_stroke(cr);
        }
        if (bar_at_middle_) {
            const double center_y = geometry.y + geometry.height / 2.0;
            cairo_move_to(cr, center_x - geometry.width * 0.25, center_y);
            cairo_line_to(cr, center_x + geometry.width * 0.25, center_y);
            cairo_stroke(cr);
        }
        if (bar_at_end_) {
            cairo_move_to(cr, center_x - geometry.width * 0.25,
                          geometry.y + geometry.height - 3.5);
            cairo_line_to(cr, center_x + geometry.width * 0.25,
                          geometry.y + geometry.height - 3.5);
            cairo_stroke(cr);
        }
        return;
    }
    const double center_y = geometry.y + geometry.height / 2.0;
    if (bar_at_start_) {
        cairo_move_to(cr, geometry.x + 3.5, center_y - geometry.height * 0.25);
        cairo_line_to(cr, geometry.x + 3.5, center_y + geometry.height * 0.25);
        cairo_stroke(cr);
    }
    if (bar_at_middle_) {
        const double center_x = geometry.x + geometry.width / 2.0;
        cairo_move_to(cr, center_x, center_y - geometry.height * 0.25);
        cairo_line_to(cr, center_x, center_y + geometry.height * 0.25);
        cairo_stroke(cr);
    }
    if (bar_at_end_) {
        cairo_move_to(cr, geometry.x + geometry.width - 3.5, center_y - geometry.height * 0.25);
        cairo_line_to(cr, geometry.x + geometry.width - 3.5, center_y + geometry.height * 0.25);
        cairo_stroke(cr);
    }
}

void SpacerModule::on_click(int, int, int)
{
}
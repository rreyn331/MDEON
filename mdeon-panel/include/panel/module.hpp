#pragma once

#include <cairo.h>
#include <string>

struct Geometry {
    int x{0};
    int y{0};
    int width{0};
    int height{0};
    bool vertical{false};

    [[nodiscard]] bool contains(int px, int py) const
    {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};

class IModule {
public:
    virtual ~IModule() = default;
    [[nodiscard]] virtual bool update() = 0;
    virtual int measure_width(cairo_t* cr, int panel_height) = 0;
    virtual void set_available_width(int) {}
    [[nodiscard]] virtual bool expands() const { return false; }
    virtual void render(cairo_t* cr, const Geometry& geometry) = 0;
    virtual void on_click(int button, int rel_x, int rel_y) = 0;
    virtual void on_motion(int, int) {}
    [[nodiscard]] virtual std::string tooltip_text() const { return {}; }
};
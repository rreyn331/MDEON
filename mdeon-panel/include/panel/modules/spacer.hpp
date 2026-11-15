#pragma once

#include "panel/module.hpp"

class SpacerModule final : public IModule {
public:
    SpacerModule(int length, bool show_bar, bool bar_at_start, bool bar_at_middle,
                 bool bar_at_end);
    [[nodiscard]] bool update() override;
    int measure_width(cairo_t* cr, int panel_height) override;
    void set_available_width(int width) override;
    [[nodiscard]] bool expands() const override;
    void render(cairo_t* cr, const Geometry& geometry) override;
    void on_click(int button, int rel_x, int rel_y) override;

private:
    int length_;
    int available_width_{0};
    bool show_bar_;
    bool bar_at_start_;
    bool bar_at_middle_;
    bool bar_at_end_;
};
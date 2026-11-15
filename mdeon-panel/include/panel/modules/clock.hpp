#pragma once

#include "panel/module.hpp"

#include <functional>
#include <string>

class ClockModule final : public IModule {
public:
    explicit ClockModule(std::function<void(int, int)> on_activate = {}, bool vertical = false);
    [[nodiscard]] bool update() override;
    int measure_width(cairo_t* cr, int panel_height) override;
    void render(cairo_t* cr, const Geometry& geometry) override;
    void on_click(int button, int rel_x, int rel_y) override;

private:
    std::function<void(int, int)> on_activate_;
    std::string cached_time_;
    std::string cached_date_;
    Geometry geometry_;
    bool vertical_{false};
    int text_width_{0};
    int text_height_{0};
};
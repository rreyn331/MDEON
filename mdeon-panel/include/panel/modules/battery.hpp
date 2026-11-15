#pragma once

#include "panel/module.hpp"

#include <chrono>

class BatteryModule final : public IModule {
public:
    explicit BatteryModule(bool vertical = false) : vertical_(vertical) {}
    [[nodiscard]] bool update() override;
    int measure_width(cairo_t* cr, int panel_height) override;
    void render(cairo_t* cr, const Geometry& geometry) override;
    void on_click(int button, int rel_x, int rel_y) override;
    [[nodiscard]] std::string tooltip_text() const override;

private:
    int capacity_{0};
    bool available_{false};
    bool plugged_in_{false};
    bool charging_{false};
    bool vertical_{false};
    std::chrono::steady_clock::time_point next_refresh_{};
};
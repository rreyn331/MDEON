#pragma once

#include "panel/module.hpp"

#include <chrono>
#include <string>

class BluetoothModule final : public IModule {
public:
    [[nodiscard]] bool update() override;
    int measure_width(cairo_t* cr, int panel_height) override;
    void render(cairo_t* cr, const Geometry& geometry) override;
    void on_click(int button, int rel_x, int rel_y) override;
    [[nodiscard]] std::string tooltip_text() const override;

private:
    bool utility_available_{false};
    bool controller_available_{false};
    bool powered_{false};
    std::string connected_devices_;
    std::chrono::steady_clock::time_point next_refresh_{};

    void toggle_power();
};
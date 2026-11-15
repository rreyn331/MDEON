#pragma once

#include "panel/module.hpp"

#include <chrono>
#include <string>

class NetworkModule final : public IModule {
public:
    [[nodiscard]] bool update() override;
    int measure_width(cairo_t* cr, int panel_height) override;
    void render(cairo_t* cr, const Geometry& geometry) override;
    void on_click(int button, int rel_x, int rel_y) override;
    [[nodiscard]] std::string tooltip_text() const override;

private:
    enum class Connection { Unknown, Offline, Wireless, Wired };

    Connection connection_{Connection::Unknown};
    std::string device_;
    bool wifi_enabled_{true};
    std::string action_notice_;
    std::chrono::steady_clock::time_point next_refresh_{};

    void toggle_wifi();
};
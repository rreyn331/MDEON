#pragma once

#include "panel/backend.hpp"
#include "panel/module.hpp"

#include <chrono>

class VirtualDesktopModule final : public IModule {
public:
    VirtualDesktopModule(IBackend& backend, int workspace_count = 2,
                         std::string display_mode = "icon_number", int icon_size = 22);
    [[nodiscard]] bool update() override;
    int measure_width(cairo_t* cr, int panel_height) override;
    void render(cairo_t* cr, const Geometry& geometry) override;
    void on_click(int button, int rel_x, int rel_y) override;
    [[nodiscard]] std::string tooltip_text() const override;

private:
    IBackend& backend_;
    int configured_count_;
    std::string display_mode_;
    int icon_size_;
    int current_desktop_{0};
    int desktop_count_{0};
    std::vector<int> workspace_ids_;
    bool available_{false};
    bool count_configured_{false};
    bool pending_change_{false};
    std::chrono::steady_clock::time_point next_refresh_{};

    void switch_desktop(int offset);
};
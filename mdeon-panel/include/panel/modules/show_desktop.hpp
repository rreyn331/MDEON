#pragma once

#include "panel/backend.hpp"
#include "panel/module.hpp"

#include <chrono>
#include <glib.h>

#include <sys/types.h>

class ShowDesktopModule final : public IModule {
public:
    explicit ShowDesktopModule(IBackend& backend);
    ~ShowDesktopModule() override;
    [[nodiscard]] bool update() override;
    int measure_width(cairo_t* cr, int panel_height) override;
    void render(cairo_t* cr, const Geometry& geometry) override;
    void on_click(int button, int rel_x, int rel_y) override;
    [[nodiscard]] std::string tooltip_text() const override;

private:
    IBackend& backend_;
    bool showing_desktop_{false};
    bool state_available_{false};
    bool kill_mode_active_{false};
    std::chrono::steady_clock::time_point next_refresh_{};
    guint pending_xkill_source_{0};
    guint xkill_watch_id_{0};
    GPid xkill_pid_{0};

    gboolean start_xkill_after_click();
    void terminate_xkill();
};
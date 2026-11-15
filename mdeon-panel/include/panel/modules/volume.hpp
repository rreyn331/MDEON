#pragma once

#include "panel/module.hpp"

#include <chrono>
#include <string>

class VolumeModule final : public IModule {
public:
    [[nodiscard]] bool update() override;
    int measure_width(cairo_t* cr, int panel_height) override;
    void render(cairo_t* cr, const Geometry& geometry) override;
    void on_click(int button, int rel_x, int rel_y) override;
    [[nodiscard]] std::string tooltip_text() const override;

private:
    enum class Backend { None, PulseAudio, PipeWire, Alsa };

    Backend backend_{Backend::None};
    int volume_{0};
    bool muted_{false};
    bool restore_attempted_{false};
    bool restore_pending_{false};
    std::chrono::steady_clock::time_point next_refresh_{};

    void detect_backend();
    void persist_state() const;
    void adjust(int delta);
    void toggle_mute();
};
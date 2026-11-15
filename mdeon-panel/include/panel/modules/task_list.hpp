#pragma once

#include "panel/backend.hpp"
#include "panel/module.hpp"
#include "panel/theme.hpp"

#include <chrono>
#include <cstddef>
#include <limits>
#include <map>
#include <string>
#include <vector>

class TaskListModule final : public IModule {
public:
    TaskListModule(IBackend& backend, std::map<std::string, std::string> config = {},
                   bool vertical = false);
    [[nodiscard]] bool update() override;
    int measure_width(cairo_t* cr, int panel_height) override;
    void set_available_width(int width) override;
    [[nodiscard]] bool expands() const override { return true; }
    void render(cairo_t* cr, const Geometry& geometry) override;
    void on_click(int button, int rel_x, int rel_y) override;
    void on_motion(int rel_x, int rel_y) override;
    [[nodiscard]] std::string tooltip_text() const override;

private:
    struct TaskEntry {
        TaskWindowInfo window;
        Geometry geometry;
    };

    IBackend& backend_;
    std::map<std::string, std::string> config_;
    bool vertical_{false};
    std::vector<TaskEntry> tasks_;
    Geometry geometry_;
    std::string hovered_task_title_;
    std::chrono::steady_clock::time_point next_refresh_{};
    int available_width_{std::numeric_limits<int>::max()};
    std::size_t visible_task_count_{0};
    int spacing_{2};
    int max_label_width_{140};
    int icon_size_{16};
    bool show_icon_{true};
    bool show_text_{true};
    bool compact_mode_{false};
    bool centered_{true};
    bool borderless_{false};
    int padding_x_{0};
    int padding_y_{0};
    RgbColor text_color_{34.0 / 255.0, 34.0 / 255.0, 34.0 / 255.0};
    RgbColor active_text_color_{1.0, 1.0, 1.0};
    RgbColor iconified_text_color_{208.0 / 255.0, 208.0 / 255.0, 208.0 / 255.0};
    double iconified_text_alpha_{0.8};
    std::string font_name_{"Sans 8"};

    [[nodiscard]] int task_width(cairo_t* cr, const TaskWindowInfo& window,
                                 bool compact = false) const;
};
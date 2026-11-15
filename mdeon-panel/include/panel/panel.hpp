#pragma once

#include "panel/backend.hpp"
#include "panel/module.hpp"

#include <memory>
#include <vector>
#include <glib.h>

enum class Alignment { Start, Center, End };

struct ModuleContainer {
    std::unique_ptr<IModule> module;
    Alignment alignment;
    Geometry geometry;
};

class Panel {
public:
    explicit Panel(std::unique_ptr<IBackend> backend, bool vertical = false);
    void add_module(std::unique_ptr<IModule> module, Alignment alignment);
    void request_redraw();
    void run_loop();
    void stop();

private:
    std::unique_ptr<IBackend> backend_;
    std::vector<ModuleContainer> modules_;
    bool vertical_{false};
    int rendered_width_{0};
    int rendered_height_{0};
    bool running_{true};
    GMainLoop* main_loop_{nullptr};

    void calculate_layout(cairo_t* cr);
    void render_frame();
    void handle_click(int x, int y, int button);
    void handle_motion(int x, int y);
};
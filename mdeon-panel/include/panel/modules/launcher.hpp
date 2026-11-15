#pragma once

#include "panel/module.hpp"

#include <string>
#include <vector>

struct LauncherEntry {
    std::string path;
    std::string name;
    std::string icon;
};

class LauncherModule final : public IModule {
public:
    LauncherModule(std::vector<std::string> desktop_files, int icon_size,
                   int max_icons, bool tooltips, bool vertical = false);
    [[nodiscard]] bool update() override;
    int measure_width(cairo_t* cr, int panel_height) override;
    void render(cairo_t* cr, const Geometry& geometry) override;
    void on_click(int button, int rel_x, int rel_y) override;
    void on_motion(int rel_x, int rel_y) override;
    [[nodiscard]] std::string tooltip_text() const override;

private:
    std::vector<LauncherEntry> entries_;
    int icon_size_;
    int max_icons_;
    bool tooltips_;
    bool vertical_{false};
    bool expanded_{false};
    int hover_index_{-1};
};
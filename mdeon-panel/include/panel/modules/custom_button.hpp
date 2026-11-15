#pragma once

#include "panel/module.hpp"

#include <map>
#include <string>

class CustomButtonModule final : public IModule {
public:
    CustomButtonModule(std::map<std::string, std::string> config,
                       int number, int icon_size = 28);
    [[nodiscard]] bool update() override;
    int measure_width(cairo_t* cr, int panel_height) override;
    void render(cairo_t* cr, const Geometry& geometry) override;
    void on_click(int button, int rel_x, int rel_y) override;
    [[nodiscard]] std::string tooltip_text() const override;

private:
    std::map<std::string, std::string> config_;
    int number_;
    int icon_size_;

    [[nodiscard]] std::string configured(const std::string& key) const;
    void run_command(const std::string& key) const;
};
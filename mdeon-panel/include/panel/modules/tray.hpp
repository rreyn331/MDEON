#pragma once

#include "panel/module.hpp"

#include <gio/gio.h>

#include <memory>
#include <string>
#include <vector>

class TrayModule final : public IModule {
public:
    explicit TrayModule(bool vertical = false);
    ~TrayModule() override;
    void register_item(const std::string& sender, const std::string& service);
    [[nodiscard]] GVariant* registered_items_variant() const;
    [[nodiscard]] bool update() override;
    int measure_width(cairo_t* cr, int panel_height) override;
    void render(cairo_t* cr, const Geometry& geometry) override;
    void on_click(int button, int rel_x, int rel_y) override;
    [[nodiscard]] std::string tooltip_text() const override;

private:
    struct Item {
        std::string id;
        std::string service;
        std::string path;
        std::string icon_name;
        std::string title;
        std::string tooltip;
        GDBusProxy* proxy{nullptr};
        bool visible{true};
        TrayModule* owner{nullptr};

        ~Item();
    };

    GDBusConnection* connection_{nullptr};
    GDBusNodeInfo* node_info_{nullptr};
    std::vector<std::unique_ptr<Item>> items_;
    guint owner_id_{0};
    guint signal_id_{0};
    guint registration_id_{0};
    bool watcher_owned_{false};
    bool pending_change_{false};
    bool vertical_{false};

    void setup_watcher();
    void unregister_item(const std::string& sender, const std::string& service);
    void load_registered_items();
    void refresh_item(Item& item);
    [[nodiscard]] bool should_hide(const Item& item) const;
};
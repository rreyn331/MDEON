#pragma once

#include "panel/backend.hpp"
#include "panel/module.hpp"

#include <map>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <gio/gio.h>

class ApplicationMenuModule final : public IModule {
public:
    ApplicationMenuModule(IBackend& backend, std::map<std::string, std::string> config,
                          bool button_visible = true, bool vertical = false,
                          std::function<void()> request_panel_redraw = {});
    ~ApplicationMenuModule() override;
    void open_menu_from_command();
    [[nodiscard]] bool update() override;
    int measure_width(cairo_t* cr, int panel_height) override;
    void render(cairo_t* cr, const Geometry& geometry) override;
    void on_click(int button, int rel_x, int rel_y) override;
    [[nodiscard]] std::string tooltip_text() const override;

private:
    enum class RowKind { Application, Power, Confirm, Cancel, Status };

    struct Application {
        GAppInfo* info{nullptr};
        std::string name;
        std::string description;
        std::string executable;
        std::string icon;
        std::set<std::string> categories;
    };

    struct Row {
        std::string label;
        std::string icon;
        RowKind kind{RowKind::Application};
        std::size_t application_index{0};
        std::string action;
    };

    struct ApplicationLoadState {
        std::mutex mutex;
        std::vector<Application> applications;
        bool complete{false};
        ~ApplicationLoadState();
    };

    IBackend& backend_;
    std::map<std::string, std::string> config_;
    std::function<void()> request_panel_redraw_;
    bool button_visible_{true};
    bool vertical_{false};
    Geometry geometry_;
    std::vector<Application> applications_;
    std::shared_ptr<ApplicationLoadState> application_load_state_;
    std::shared_ptr<std::atomic<bool>> alive_{std::make_shared<std::atomic<bool>>(true)};
    guint load_poll_source_id_{0};
    bool application_load_started_{false};
    bool applications_loaded_{false};
    bool icon_refresh_notified_{false};
    std::vector<std::string> visible_categories_;
    std::vector<Row> visible_rows_;
    std::string active_category_{"All"};
    std::string search_text_;
    std::string pending_power_action_;
    std::string pending_power_label_;
    int confirmation_seconds_{0};
    std::chrono::steady_clock::time_point next_confirmation_tick_{};
    std::vector<std::string> place_targets_;
    std::vector<std::string> system_commands_;
    bool confirming_power_{false};

    void load_applications();
    void resolve_application_icons();
    [[nodiscard]] static std::vector<Application> discover_applications();
    void open_menu();
    void update_visible_categories();
    [[nodiscard]] bool matches_category(const Application& application,
                                        const std::string& category) const;
    void select_category(const std::string& category);
    void search(const std::string& query);
    void refresh_display(std::size_t selected = 0, bool preserve_selection = false);
    void activate(std::size_t index);
    void activate_shortcut(bool system, std::size_t index);
    void clear_power_confirmation();
    void request_power_action(const std::string& action, const std::string& label);
    [[nodiscard]] bool execute_power_action(const std::string& action) const;
    [[nodiscard]] std::vector<ApplicationMenuDisplayItem> display_items() const;
    [[nodiscard]] std::string setting(const std::string& key,
                                      const std::string& fallback = {}) const;
};
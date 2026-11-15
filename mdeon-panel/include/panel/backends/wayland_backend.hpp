#pragma once

#include "panel/backend.hpp"

#include <memory>

class WaylandBackendImpl;

class WaylandBackend final : public IBackend {
public:
    WaylandBackend();
    ~WaylandBackend() override;
    bool init(int width, int height, Edge edge) override;
    bool set_monitor(const std::string& monitor) override;
    void set_task_monitor_filter(bool enabled) override;
    std::vector<std::string> list_monitors() override;
    cairo_t* get_cairo() override;
    void commit() override;
    [[nodiscard]] int event_fd() const override;
    void process_events() override;
    void set_on_click(ClickCallback callback) override;
    void set_on_motion(MotionCallback callback) override;
    void show_tooltip(const std::string& text, int x, int y) override;
    void hide_tooltip() override;
    void open_application_menu(int anchor_x, int anchor_width,
                              ApplicationMenuSettings settings,
                              std::vector<std::string> categories,
                              std::vector<ApplicationMenuDisplayItem> items,
                              std::function<void(const std::string&)> on_category,
                              std::function<void(const std::string&)> on_search,
                              std::function<void(std::size_t)> on_activate,
                              std::function<void(bool, std::size_t)> on_shortcut,
                              std::function<void(const std::string&, const std::string&)> on_power_action,
                              std::function<void()> on_close) override;
    void update_application_menu(std::string query,
                                 std::vector<ApplicationMenuDisplayItem> items,
                                 std::size_t selected, bool preserve_selection = false) override;
    void focus_application_menu_items() override;
    void set_application_menu_countdown_active(bool active) override;
    void set_application_menu_category(std::string category) override;
    void set_application_menu_categories(std::vector<std::string> categories) override;
    void toggle_application_power_menu(
        int anchor_x, int anchor_width,
        std::function<void(const std::string&, const std::string&)> on_power_action) override;
    void close_application_menu() override;
    [[nodiscard]] bool application_menu_visible() const override;
    void toggle_calendar(int anchor_x, int anchor_width) override;
    void open_desktop_hub(int anchor_x, int anchor_width, std::string icon_name,
                          std::vector<HubDisplayItem> items,
                          std::function<void(const std::string&)> on_search,
                          std::function<void(std::size_t)> on_activate,
                          std::function<void()> on_close) override;
    void update_desktop_hub(std::string query, std::vector<HubDisplayItem> items,
                            std::size_t selected, bool preserve_selection = false) override;
    void close_desktop_hub() override;
    void set_desktop_hub_countdown_active(bool active) override;
    [[nodiscard]] bool desktop_hub_visible() const override;
    [[nodiscard]] bool supports_task_list() const override;
    [[nodiscard]] bool supports_task_context_menu() const override;
    [[nodiscard]] bool supports_task_action(TaskAction action) const override;
    std::vector<TaskWindowInfo> list_task_windows() override;
    void perform_task_action(const std::string& id, TaskAction action) override;
    void show_task_menu(const std::string& id, int panel_x, int panel_y,
                        bool maximized) override;
    [[nodiscard]] bool supports_workspaces() const override;
    WorkspaceSnapshot get_workspaces() override;
    bool configure_workspace_count(int count) override;
    bool switch_workspace(int id) override;
    [[nodiscard]] bool supports_show_desktop() const override;
    bool get_show_desktop_state(bool& showing) override;
    bool toggle_show_desktop() override;
    [[nodiscard]] bool supports_confirmed_window_close() const override;
    bool confirm_close_active_window() override;
    [[nodiscard]] int width() const override;
    [[nodiscard]] int height() const override;

private:
    friend class WaylandBackendImpl;
    std::unique_ptr<WaylandBackendImpl> impl_;
    std::string monitor_{"all"};
    bool task_monitor_filter_{true};
    int width_{0};
    int height_{0};
    ClickCallback click_callback_;
    MotionCallback motion_callback_;
};
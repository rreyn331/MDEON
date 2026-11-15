#pragma once

#include "panel/backend.hpp"
#include "panel/backends/wayland_task_backend.hpp"

#include <string>
#include <vector>
#include <functional>
#include <chrono>

#include <xcb/xcb.h>

struct TaskWindow {
    xcb_window_t id{XCB_WINDOW_NONE};
    std::string title;
    std::string icon_name;
    std::vector<uint32_t> icon_argb;
    uint32_t icon_width{0};
    uint32_t icon_height{0};
    uint64_t icon_hash{0};
    bool active{false};
    bool iconified{false};
    bool maximized{false};
};

class X11Backend final : public IBackend {
public:
    ~X11Backend() override;
    bool init(int width, int height, Edge edge) override;
    bool set_monitor(const std::string& monitor) override;
    void set_task_monitor_filter(bool enabled) override;
    std::vector<std::string> list_monitors() override;
    cairo_t* get_cairo() override;
    void commit() override;
    [[nodiscard]] int event_fd() const override;
    void process_events() override;
    [[nodiscard]] bool grab_escape_key(std::function<void()> callback) override;
    void release_escape_key() override;
    void set_on_click(ClickCallback callback) override;
    void set_on_motion(MotionCallback callback) override;
    void show_tooltip(const std::string& text, int x, int y) override;
    void hide_tooltip() override;
    void toggle_calendar(int anchor_x, int anchor_width) override;
    [[nodiscard]] std::vector<TaskWindow> get_task_windows();
    void perform_task_action(xcb_window_t window, TaskAction action);
    void show_task_menu(xcb_window_t window, int panel_x, int panel_y, bool maximized);
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
    [[nodiscard]] bool supports_xkill() const override;
    [[nodiscard]] bool supports_native_window_kill() const override;
    bool start_native_window_kill() override;
    bool native_window_kill_active() const override;
    void open_application_menu(int anchor_x, int anchor_width,
                              ApplicationMenuSettings settings,
                              std::vector<std::string> categories,
                              std::vector<ApplicationMenuDisplayItem> items,
                              std::function<void(const std::string&)> on_category,
                              std::function<void(const std::string&)> on_search,
                              std::function<void(std::size_t)> on_activate,
                              std::function<void(bool, std::size_t)> on_shortcut,
                              std::function<void(const std::string&, const std::string&)> on_power_action,
                              std::function<void()> on_close);
    void update_application_menu(std::string query,
                                 std::vector<ApplicationMenuDisplayItem> items,
                                 std::size_t selected, bool preserve_selection = false);
    void focus_application_menu_items() override;
    void set_application_menu_countdown_active(bool active) override;
    void set_application_menu_category(std::string category);
    void set_application_menu_categories(std::vector<std::string> categories);
    void toggle_application_power_menu(
        int anchor_x, int anchor_width,
        std::function<void(const std::string&, const std::string&)> on_power_action);
    void close_application_menu();
    [[nodiscard]] bool application_menu_visible() const;
    [[nodiscard]] bool application_power_menu_visible() const;
    void close_application_power_menu();
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
    [[nodiscard]] int width() const override;
    [[nodiscard]] int height() const override;

private:
    xcb_connection_t* connection_{nullptr};
    xcb_screen_t* screen_{nullptr};
    xcb_window_t window_{XCB_WINDOW_NONE};
    xcb_window_t calendar_window_{XCB_WINDOW_NONE};
    xcb_window_t tooltip_window_{XCB_WINDOW_NONE};
    xcb_window_t task_menu_window_{XCB_WINDOW_NONE};
    xcb_window_t application_menu_window_{XCB_WINDOW_NONE};
    xcb_window_t application_power_window_{XCB_WINDOW_NONE};
    xcb_window_t desktop_hub_window_{XCB_WINDOW_NONE};
    cairo_surface_t* surface_{nullptr};
    cairo_t* cairo_{nullptr};
    cairo_surface_t* calendar_surface_{nullptr};
    cairo_t* calendar_cairo_{nullptr};
    cairo_surface_t* tooltip_surface_{nullptr};
    cairo_t* tooltip_cairo_{nullptr};
    cairo_surface_t* task_menu_surface_{nullptr};
    cairo_t* task_menu_cairo_{nullptr};
    cairo_surface_t* application_menu_surface_{nullptr};
    cairo_t* application_menu_cairo_{nullptr};
    cairo_surface_t* application_power_surface_{nullptr};
    cairo_t* application_power_cairo_{nullptr};
    cairo_surface_t* desktop_hub_surface_{nullptr};
    cairo_t* desktop_hub_cairo_{nullptr};
    int width_{0};
    int height_{0};
    int monitor_x_{0};
    int monitor_y_{0};
    int monitor_width_{0};
    int monitor_height_{0};
    std::string monitor_{"all"};
    bool task_monitor_filter_{true};
    int panel_x_{0};
    int panel_y_{0};
    int tooltip_x_{0};
    int tooltip_y_{0};
    int tooltip_width_{0};
    int tooltip_height_{0};
    int task_menu_x_{0};
    int task_menu_y_{0};
    int task_menu_width_{190};
    int task_menu_row_height_{28};
    int task_menu_hovered_{-1};
    mutable std::unique_ptr<WaylandTaskBackend> gnome_bridge_;
    bool gnome_wayland_session_{false};
    mutable std::chrono::steady_clock::time_point gnome_bridge_retry_after_{};
    std::string task_menu_target_;
    bool task_menu_maximized_{false};
    bool task_menu_pointer_grabbed_{false};
    bool application_menu_pointer_grabbed_{false};
    bool application_menu_keyboard_grabbed_{false};
    bool application_menu_previous_focus_valid_{false};
    xcb_window_t application_menu_previous_focus_{XCB_NONE};
    bool application_power_visible_{false};
    bool application_power_standalone_{false};
    bool application_power_pointer_grabbed_{false};
    bool application_power_keyboard_grabbed_{false};
    int application_power_x_{0};
    int application_power_y_{0};
    int application_power_width_{190};
    int application_power_height_{176};
    int application_power_selected_{0};
    std::vector<std::string> application_menu_categories_;
    ApplicationMenuSettings application_menu_settings_;
    std::string application_menu_active_category_{"All"};
    std::string application_menu_query_;
    std::vector<ApplicationMenuDisplayItem> application_menu_items_;
    std::size_t application_menu_selected_{0};
    std::size_t application_menu_scroll_{0};
    std::size_t application_menu_category_scroll_{0};
    std::size_t application_menu_selected_category_{0};
    std::size_t application_menu_selected_shortcut_{0};
    int application_menu_shortcut_scroll_{0};
    int application_menu_focus_{0};
    int application_menu_scrollbar_drag_column_{0};
    int application_menu_scrollbar_drag_offset_{0};
    bool application_menu_tab_held_{false};
    int application_menu_x_{0};
    int application_menu_y_{0};
    int application_menu_width_{450};
    int application_menu_height_{500};
    int application_menu_row_height_{28};
    bool application_menu_visible_{false};
    bool application_menu_countdown_active_{false};
    unsigned int popup_leave_timeout_{0};
    std::function<void(const std::string&)> application_menu_category_callback_;
    std::function<void(const std::string&)> application_menu_search_callback_;
    std::function<void(std::size_t)> application_menu_activate_callback_;
    std::function<void(bool, std::size_t)> application_menu_shortcut_callback_;
    std::function<void(const std::string&, const std::string&)> application_menu_power_callback_;
    std::function<void()> application_menu_close_callback_;
    bool desktop_hub_pointer_grabbed_{false};
    std::string desktop_hub_query_;
    std::string desktop_hub_icon_;
    std::vector<HubDisplayItem> desktop_hub_items_;
    std::size_t desktop_hub_selected_{0};
    std::size_t desktop_hub_scroll_{0};
    int desktop_hub_x_{0};
    int desktop_hub_y_{0};
    int desktop_hub_width_{600};
    int desktop_hub_height_{450};
    int desktop_hub_row_height_{30};
    bool desktop_hub_visible_{false};
    bool desktop_hub_countdown_active_{false};
    std::function<void(const std::string&)> desktop_hub_search_callback_;
    std::function<void(std::size_t)> desktop_hub_activate_callback_;
    std::function<void()> desktop_hub_close_callback_;
    std::function<void()> escape_key_callback_;
    xcb_keycode_t escape_keycode_{0};
    int calendar_year_{0};
    int calendar_month_{0};
    Edge edge_{Edge::Top};
    bool calendar_visible_{false};
    bool tooltip_visible_{false};
    std::string tooltip_text_;
    ClickCallback click_callback_;
    MotionCallback motion_callback_;

    void create_calendar();
    bool ensure_gnome_bridge() const;
    void position_calendar(int anchor_x, int anchor_width);
    void draw_calendar();
    void change_calendar_month(int delta);
    void hide_calendar();
    void handle_calendar_click(int x, int y);
    void draw_tooltip();
    void destroy_tooltip();
    void draw_task_menu();
    void hide_task_menu();
    void destroy_task_menu();
    void handle_task_menu_click(int x, int y);
    void draw_application_menu(int redraw_column = 0);
    void grab_application_menu_inputs();
    void draw_application_power_menu();
    void open_application_power_menu();
    void hide_application_power_menu();
    void hide_application_menu(bool notify);
    void handle_popup_crossing(xcb_window_t window, bool entering,
                               uint8_t mode, uint8_t detail);
    void schedule_popup_dismiss();
    void cancel_popup_dismiss();
    void destroy_application_menu();
    void handle_application_menu_key(const xcb_key_press_event_t* event);
    void handle_application_power_key(const xcb_key_press_event_t* event);
    void handle_application_power_click(int x, int y, int root_x, int root_y, uint8_t button);
    void handle_application_power_motion(int y);
    void handle_application_menu_click(int x, int y, int root_x, int root_y, uint8_t button);
    void handle_application_menu_motion(int x, int y, int root_x, int root_y);
    void select_application_menu_category(const std::string& category);
    void update_application_menu_scroll();
    void update_application_menu_category_scroll();
    void update_application_menu_scrollbar_drag(int y);
    void update_application_menu_shortcut_scroll();
    void draw_desktop_hub();
    void hide_desktop_hub(bool notify);
    void destroy_desktop_hub();
    void handle_desktop_hub_key(const xcb_key_press_event_t* event);
    void handle_desktop_hub_click(int x, int y, uint8_t button);
    void update_desktop_hub_scroll();
    void set_dock_properties(Edge edge);
    void position_popup(int anchor_position, int anchor_extent,
                        int popup_width, int popup_height, int& x, int& y) const;
};
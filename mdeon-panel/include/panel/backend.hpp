#pragma once

#include <cairo.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

enum class Edge { Top, Bottom, Left, Right };

struct ApplicationMenuDisplayItem {
    std::string label;
    std::string icon_name;
};

struct ApplicationMenuSettings {
    int width{700};
    int height{500};
    int places_width{138};
    int categories_width{128};
    int applications_width{434};
    bool show_categories{true};
    bool show_places{true};
    std::vector<ApplicationMenuDisplayItem> places;
    std::vector<ApplicationMenuDisplayItem> system;
};

struct HubDisplayItem {
    std::string label;
    std::string icon_name;
};

struct TaskWindowInfo {
    std::string id;
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

enum class TaskAction { Activate, Minimize, ToggleMinimize, ToggleMaximize, Close };

struct WorkspaceSnapshot {
    std::vector<int> ids;
    int current_id{-1};
};

inline std::string lowercase_task_text(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

inline bool task_is_excluded(const std::string& app_id, const std::string& title)
{
    static const std::vector<std::string> defaults = {
        "jgmenu", "jgmenu_run", "jgmenu-apps", "bemenu", "rofi", "wofi",
        "nwg-menu", "xfce4-appfinder"
    };
    std::vector<std::string> excluded = defaults;
    for (const char* variable : {"SIMPANEL_TASK_EXCLUDE", "MDEON_PANEL_TASK_EXCLUDE"}) {
        const char* configured = std::getenv(variable);
        if (configured == nullptr) {
            continue;
        }
        std::stringstream values(configured);
        std::string value;
        while (std::getline(values, value, ',')) {
            const std::size_t first = value.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                continue;
            }
            const std::size_t last = value.find_last_not_of(" \t\r\n");
            excluded.push_back(lowercase_task_text(value.substr(first, last - first + 1)));
        }
    }
    const std::string normalized_app = lowercase_task_text(app_id);
    const std::string normalized_title = lowercase_task_text(title);
    return std::any_of(excluded.begin(), excluded.end(), [&](const std::string& value) {
        return normalized_app == value || normalized_title == value;
    });
}

class IBackend {
public:
    using ClickCallback = std::function<void(int x, int y, int button)>;
    using MotionCallback = std::function<void(int x, int y)>;

    virtual ~IBackend() = default;
    virtual bool init(int width, int height, Edge edge) = 0;
    virtual bool set_monitor(const std::string&) { return true; }
    virtual void set_task_monitor_filter(bool) {}
    virtual std::vector<std::string> list_monitors() { return {}; }
    virtual cairo_t* get_cairo() = 0;
    virtual void commit() = 0;
    [[nodiscard]] virtual int event_fd() const = 0;
    virtual void process_events() = 0;
    virtual void set_on_click(ClickCallback callback) = 0;
    virtual void set_on_motion(MotionCallback callback) = 0;
    virtual void show_tooltip(const std::string& text, int x, int y) = 0;
    virtual void hide_tooltip() = 0;
    virtual void open_application_menu(
        int, int, ApplicationMenuSettings, std::vector<std::string>,
        std::vector<ApplicationMenuDisplayItem>,
        std::function<void(const std::string&)>, std::function<void(const std::string&)>,
        std::function<void(std::size_t)>, std::function<void(bool, std::size_t)>,
        std::function<void(const std::string&, const std::string&)>,
        std::function<void()>) {}
    virtual void update_application_menu(std::string,
        std::vector<ApplicationMenuDisplayItem>, std::size_t, bool = false) {}
    virtual void focus_application_menu_items() {}
    virtual void set_application_menu_countdown_active(bool) {}
    virtual void set_application_menu_category(std::string) {}
    virtual void set_application_menu_categories(std::vector<std::string>) {}
    virtual void toggle_application_power_menu(
        int, int, std::function<void(const std::string&, const std::string&)>) {}
    virtual void close_application_menu() {}
    [[nodiscard]] virtual bool application_menu_visible() const { return false; }
    virtual void toggle_calendar(int, int) {}
    virtual void open_desktop_hub(int, int, std::string, std::vector<HubDisplayItem>,
        std::function<void(const std::string&)>, std::function<void(std::size_t)>,
        std::function<void()>) {}
    virtual void update_desktop_hub(std::string, std::vector<HubDisplayItem>,
                                    std::size_t, bool = false) {}
    virtual void close_desktop_hub() {}
    virtual void set_desktop_hub_countdown_active(bool) {}
    [[nodiscard]] virtual bool desktop_hub_visible() const { return false; }
    [[nodiscard]] virtual bool supports_task_list() const { return false; }
    [[nodiscard]] virtual bool supports_task_context_menu() const { return false; }
    [[nodiscard]] virtual bool supports_task_action(TaskAction) const { return false; }
    virtual std::vector<TaskWindowInfo> list_task_windows() { return {}; }
    virtual void perform_task_action(const std::string&, TaskAction) {}
    virtual void show_task_menu(const std::string&, int, int, bool) {}
    [[nodiscard]] virtual bool supports_workspaces() const { return false; }
    virtual WorkspaceSnapshot get_workspaces() { return {}; }
    virtual bool configure_workspace_count(int) { return false; }
    virtual bool switch_workspace(int) { return false; }
    [[nodiscard]] virtual bool supports_show_desktop() const { return false; }
    virtual bool get_show_desktop_state(bool&) { return false; }
    virtual bool toggle_show_desktop() { return false; }
    [[nodiscard]] virtual bool supports_confirmed_window_close() const { return false; }
    virtual bool confirm_close_active_window() { return false; }
    [[nodiscard]] virtual bool supports_xkill() const { return false; }
    [[nodiscard]] virtual bool supports_native_window_kill() const { return false; }
    virtual bool start_native_window_kill() { return false; }
    virtual bool native_window_kill_active() const { return false; }
    virtual bool grab_escape_key(std::function<void()>) { return false; }
    virtual void release_escape_key() {}
    [[nodiscard]] virtual int width() const = 0;
    [[nodiscard]] virtual int height() const = 0;
};
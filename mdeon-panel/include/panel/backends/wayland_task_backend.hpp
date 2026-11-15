#pragma once

#include "panel/backend.hpp"

#include <memory>
#include <string>
#include <vector>

class WaylandTaskBackend {
public:
    static std::unique_ptr<WaylandTaskBackend> create();
    static std::unique_ptr<WaylandTaskBackend> create_gnome_bridge();
    ~WaylandTaskBackend();

    WaylandTaskBackend(const WaylandTaskBackend&) = delete;
    WaylandTaskBackend& operator=(const WaylandTaskBackend&) = delete;

    [[nodiscard]] bool supports_context_menu() const;
    [[nodiscard]] bool supports_action(TaskAction action) const;
    void set_monitor_filter(std::string monitor, bool enabled);
    std::vector<TaskWindowInfo> list_windows();
    bool perform_action(const std::string& id, TaskAction action);
    bool close_active_window();
    [[nodiscard]] bool supports_workspaces() const;
    WorkspaceSnapshot get_workspaces();
    bool configure_workspace_count(int count);
    bool switch_workspace(int id);
    [[nodiscard]] bool supports_show_desktop() const;
    bool get_show_desktop_state(bool& showing);
    bool toggle_show_desktop();
    [[nodiscard]] bool supports_window_kill_mode() const;
    bool begin_window_kill_mode();
    bool window_kill_mode_active() const;

private:
    class Impl;
    explicit WaylandTaskBackend(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

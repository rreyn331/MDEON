#include "panel/backends/wayland_task_backend.hpp"

#include "panel/icon_theme.hpp"
#include "panel/logging.hpp"
#include "panel/modules/system_commands.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <memory>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef SIMPANEL_HAVE_JSON_C
#include <json-c/json.h>
#endif

#ifdef SIMPANEL_HAVE_FOREIGN_TOPLEVEL
#include <wayland-client.h>
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#endif

namespace {

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

#ifdef SIMPANEL_HAVE_JSON_C
std::string json_string(json_object* object, const char* key)
{
    json_object* field = nullptr;
    if (!json_object_object_get_ex(object, key, &field) || field == nullptr ||
        json_object_is_type(field, json_type_null)) {
        return {};
    }
    const char* value = json_object_get_string(field);
    return value != nullptr ? value : "";
}

bool json_bool(json_object* object, const char* key)
{
    json_object* field = nullptr;
    return json_object_object_get_ex(object, key, &field) && field != nullptr &&
        json_object_get_boolean(field);
}

void enable_gnome_extension_if_disabled()
{
    GError* error = nullptr;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    g_clear_error(&error);
    if (connection == nullptr) return;

    GVariant* owner_reply = g_dbus_connection_call_sync(connection,
        "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "NameHasOwner", g_variant_new("(s)", "org.mdeon.Panel.WindowManager"),
        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 200, nullptr, &error);
    g_clear_error(&error);
    if (owner_reply != nullptr) {
        gboolean bridge_owned = FALSE;
        g_variant_get(owner_reply, "(b)", &bridge_owned);
        g_variant_unref(owner_reply);
        if (bridge_owned) {
            g_object_unref(connection);
            return;
        }
    }

    constexpr char destination[] = "org.gnome.Shell.Extensions";
    constexpr char object_path[] = "/org/gnome/Shell/Extensions";
    constexpr char interface_name[] = "org.gnome.Shell.Extensions";
    constexpr char extension_id[] = "mdeon-panel-window-manager@roy";
    GVariant* info = g_dbus_connection_call_sync(connection, destination, object_path,
        interface_name, "GetExtensionInfo", g_variant_new("(s)", extension_id),
        G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 200, nullptr, &error);
    g_clear_error(&error);
    if (info == nullptr) {
        g_object_unref(connection);
        return;
    }

    GVariant* properties = nullptr;
    g_variant_get(info, "(@a{sv})", &properties);
    gboolean enabled = FALSE;
    const bool has_enabled_state = g_variant_lookup(properties, "enabled", "b", &enabled);
    g_variant_unref(properties);
    g_variant_unref(info);
    if (has_enabled_state && !enabled) {
        GVariant* result = g_dbus_connection_call_sync(connection, destination, object_path,
            interface_name, "EnableExtension", g_variant_new("(s)", extension_id),
            G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 200, nullptr, &error);
        g_clear_error(&error);
        if (result != nullptr) g_variant_unref(result);
    }
    g_object_unref(connection);
}

std::vector<TaskWindowInfo> parse_task_array(const std::string& data)
{
    std::vector<TaskWindowInfo> result;
    json_object* root = json_tokener_parse(data.c_str());
    if (root == nullptr || !json_object_is_type(root, json_type_array)) {
        if (root != nullptr) json_object_put(root);
        return result;
    }
    const std::size_t count = json_object_array_length(root);
    for (std::size_t index = 0; index < count; ++index) {
        json_object* object = json_object_array_get_idx(root, index);
        if (object == nullptr || !json_object_is_type(object, json_type_object)) continue;
        TaskWindowInfo task;
        task.id = json_string(object, "xid");
        task.title = json_string(object, "title");
        task.icon_name = resolve_application_icon(json_string(object, "app_id"));
        if (task.icon_name.empty()) task.icon_name = "application-x-executable";
        if (task.title.empty()) task.title = "Window";
        if (task.id.empty() || task_is_excluded(json_string(object, "app_id"), task.title)) continue;
        task.active = json_bool(object, "active");
        task.iconified = json_bool(object, "iconified");
        task.maximized = json_bool(object, "maximized");
        result.push_back(std::move(task));
    }
    json_object_put(root);
    return result;
}
#endif

class Provider {
public:
    virtual ~Provider() = default;
    [[nodiscard]] virtual bool supports(TaskAction action) const = 0;
    virtual void set_monitor_filter(std::string, bool) {}
    virtual std::vector<TaskWindowInfo> list_windows() = 0;
    virtual bool perform_action(const std::string& id, TaskAction action) = 0;
    [[nodiscard]] virtual bool supports_workspaces() const { return false; }
    virtual WorkspaceSnapshot get_workspaces() { return {}; }
    virtual bool configure_workspace_count(int) { return false; }
    virtual bool switch_workspace(int) { return false; }
    [[nodiscard]] virtual bool supports_show_desktop() const { return false; }
    virtual bool get_show_desktop_state(bool&) { return false; }
    virtual bool toggle_show_desktop() { return false; }
    [[nodiscard]] virtual bool supports_window_kill_mode() const { return false; }
    virtual bool begin_window_kill_mode() { return false; }
    virtual bool window_kill_mode_active() const { return false; }
    virtual bool close_active_window() { return false; }
};

#ifdef SIMPANEL_HAVE_JSON_C
class GnomeDbusProvider final : public Provider {
public:
    GnomeDbusProvider()
    {
        GError* error = nullptr;
        connection_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (connection_ == nullptr) {
            const std::string message = error != nullptr ? error->message : "session bus unavailable";
            g_clear_error(&error);
            throw std::runtime_error(message);
        }
        GVariant* reply = call("GetWindows", nullptr, G_VARIANT_TYPE("(s)"));
        if (reply == nullptr) {
            g_clear_object(&connection_);
            throw std::runtime_error("GNOME window-manager bridge is unavailable");
        }
        g_variant_unref(reply);
    }

    ~GnomeDbusProvider() override { g_clear_object(&connection_); }

    [[nodiscard]] bool supports(TaskAction) const override { return true; }

    std::vector<TaskWindowInfo> list_windows() override
    {
        GVariant* reply = call("GetWindows", nullptr, G_VARIANT_TYPE("(s)"));
        if (reply == nullptr) return last_windows_;
        const gchar* json = nullptr;
        g_variant_get(reply, "(&s)", &json);
        last_windows_ = parse_task_array(json != nullptr ? json : "[]");
        g_variant_unref(reply);
        return last_windows_;
    }

    bool perform_action(const std::string& id, TaskAction action) override
    {
        if (action == TaskAction::ToggleMinimize) {
            const auto windows = list_windows();
            const auto found = std::find_if(windows.begin(), windows.end(), [&](const auto& task) {
                return task.id == id;
            });
            if (found == windows.end()) return false;
            action = found->active ? TaskAction::Minimize : TaskAction::Activate;
        }
        const char* method = action == TaskAction::Activate || action == TaskAction::ToggleMinimize
            ? "Activate" : action == TaskAction::Minimize ? "Minimize" :
            action == TaskAction::ToggleMaximize ? "Maximize" : "Close";
        char* end = nullptr;
        const guint32 window_id = static_cast<guint32>(g_ascii_strtoull(id.c_str(), &end, 10));
        if (end == id.c_str() || *end != '\0') return false;
        GVariant* reply = call(method, g_variant_new("(u)", window_id), G_VARIANT_TYPE("()"));
        if (reply == nullptr) return false;
        g_variant_unref(reply);
        return true;
    }

    [[nodiscard]] bool supports_workspaces() const override
    {
        return true;
    }

    WorkspaceSnapshot get_workspaces() override
    {
        WorkspaceSnapshot snapshot;
        GVariant* reply = call("GetWorkspaces", nullptr, G_VARIANT_TYPE("(ii)"));
        if (reply == nullptr) return snapshot;
        gint32 current_index = 0;
        gint32 workspace_count = 0;
        g_variant_get(reply, "(ii)", &current_index, &workspace_count);
        g_variant_unref(reply);
        if (workspace_count < 1 || current_index < 0 || current_index >= workspace_count) {
            return snapshot;
        }
        snapshot.ids.reserve(static_cast<std::size_t>(workspace_count));
        for (int index = 0; index < workspace_count; ++index) {
            snapshot.ids.push_back(index);
        }
        snapshot.current_id = current_index;
        return snapshot;
    }

    bool configure_workspace_count(int count) override
    {
        return count > 0;
    }

    bool switch_workspace(int id) override
    {
        GVariant* reply = call("SwitchWorkspace", g_variant_new("(i)", id),
                               G_VARIANT_TYPE("(b)"));
        if (reply == nullptr) return false;
        gboolean switched = FALSE;
        g_variant_get(reply, "(b)", &switched);
        g_variant_unref(reply);
        return switched;
    }

    [[nodiscard]] bool supports_show_desktop() const override
    {
        return true;
    }

    bool get_show_desktop_state(bool& showing) override
    {
        GVariant* reply = call("GetShowingDesktop", nullptr, G_VARIANT_TYPE("(b)"));
        if (reply == nullptr) return false;
        gboolean state = FALSE;
        g_variant_get(reply, "(b)", &state);
        g_variant_unref(reply);
        showing = state;
        return true;
    }

    bool toggle_show_desktop() override
    {
        bool showing = false;
        if (!get_show_desktop_state(showing)) return false;
        GVariant* reply = call("SetShowingDesktop", g_variant_new("(b)", !showing),
                               G_VARIANT_TYPE("(b)"));
        if (reply == nullptr) return false;
        gboolean changed = FALSE;
        g_variant_get(reply, "(b)", &changed);
        g_variant_unref(reply);
        return changed;
    }

    [[nodiscard]] bool supports_window_kill_mode() const override
    {
        return true;
    }

    bool begin_window_kill_mode() override
    {
        GVariant* reply = call("BeginWindowKill", nullptr, G_VARIANT_TYPE("(b)"));
        if (reply == nullptr) return false;
        gboolean started = FALSE;
        g_variant_get(reply, "(b)", &started);
        g_variant_unref(reply);
        return started;
    }

    bool window_kill_mode_active() const override
    {
        GVariant* reply = call("GetWindowKillActive", nullptr, G_VARIANT_TYPE("(b)"));
        if (reply == nullptr) return false;
        gboolean active = FALSE;
        g_variant_get(reply, "(b)", &active);
        g_variant_unref(reply);
        return active;
    }

private:
    GDBusConnection* connection_{nullptr};
    std::vector<TaskWindowInfo> last_windows_;

    GVariant* call(const char* method, GVariant* parameters,
                   const GVariantType* result_type) const
    {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(connection_,
            "org.mdeon.Panel.WindowManager", "/org/mdeon/Panel/WindowManager",
            "org.mdeon.Panel.WindowManager", method, parameters, result_type,
            G_DBUS_CALL_FLAGS_NONE, 100, nullptr, &error);
        g_clear_error(&error);
        return reply;
    }
};

class KWinDbusProvider final : public Provider {
public:
    KWinDbusProvider()
    {
        GError* error = nullptr;
        connection_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (connection_ == nullptr) {
            const std::string message = error != nullptr ? error->message : "session bus unavailable";
            g_clear_error(&error);
            throw std::runtime_error(message);
        }
        GVariant* ping = g_dbus_connection_call_sync(connection_, "org.kde.KWin", "/KWin",
            "org.freedesktop.DBus.Peer", "Ping", nullptr, G_VARIANT_TYPE("()"),
            G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
        if (ping == nullptr) {
            const std::string message = error != nullptr ? error->message : "KWin unavailable";
            g_clear_error(&error);
            g_clear_object(&connection_);
            throw std::runtime_error(message);
        }
        g_variant_unref(ping);

        static const char interface_xml[] =
            "<node><interface name='org.mdeon.Simpanel.TaskList'>"
            "<method name='Exchange'><arg type='s' direction='in'/>"
            "<arg type='s' direction='out'/></method>"
            "<method name='CloseActiveResult'><arg type='s' direction='in'/></method>"
            "<method name='GetSnapshot'><arg type='s' direction='out'/></method>"
            "</interface></node>";
        node_info_ = g_dbus_node_info_new_for_xml(interface_xml, &error);
        if (node_info_ == nullptr) {
            const std::string message = error != nullptr ? error->message : "invalid D-Bus interface";
            g_clear_error(&error);
            cleanup();
            throw std::runtime_error(message);
        }
        registration_id_ = g_dbus_connection_register_object(connection_,
            "/org/mdeon/Simpanel/TaskList",
            g_dbus_node_info_lookup_interface(node_info_, "org.mdeon.Simpanel.TaskList"),
            &interface_vtable_, this, nullptr, &error);
        if (registration_id_ == 0) {
            const std::string message = error != nullptr ? error->message : "D-Bus registration failed";
            g_clear_error(&error);
            cleanup();
            throw std::runtime_error(message);
        }

        const gchar* bus_name = g_dbus_connection_get_unique_name(connection_);
        plugin_name_ = "simpanel-task-list-" + std::to_string(getpid());
        std::string script = R"JS((function() {
    var service = "__BUS_NAME__";
    var path = "/org/mdeon/Simpanel/TaskList";
    var iface = "org.mdeon.Simpanel.TaskList";

    function windows() {
        if (typeof workspace.windowList === "function") return workspace.windowList();
        if (typeof workspace.clientList === "function") return workspace.clientList();
        return workspace.stackingOrder || [];
    }

    var last_active_id = "";
    function isApplicationWindow(client) {
        if (!client || client.normalWindow === false || client.dock === true ||
            client.desktopWindow === true || client.specialWindow === true) return false;
        var appId = String(client.desktopFileName || client.resourceClass ||
            client.resourceName || "").toLowerCase();
        var title = String(client.caption || client.title || "").toLowerCase();
        return appId.indexOf("mdeon-panel") === -1 && title !== "mdeon-panel";
    }
    function rememberActiveWindow(client) {
        if (isApplicationWindow(client)) last_active_id = String(client.internalId);
    }
    function requestWindowClose(client, source) {
        var result = "no eligible window";
        if (client) {
            var title = String(client.caption || client.resourceClass || "(untitled)");
            if (client.closeable === false) {
                result = "window is not closeable: '" + title + "'";
            } else {
                try {
                    if (typeof client.closeWindow === "function") {
                        client.closeWindow();
                        result = "called closeWindow for '" + title + "'";
                    } else if (typeof client.close === "function") {
                        client.close();
                        result = "called close for '" + title + "'";
                    } else {
                        result = "window has no close method: '" + title + "'";
                    }
                } catch (error) {
                    result = "close call threw for '" + title + "': " + String(error);
                }
                result += " (source=" + source + ", id=" + String(client.internalId) +
                    ", pid=" + String(client.pid) + ")";
            }
        }
        callDBus(service, path, iface, "CloseActiveResult", result, function() {});
    }

    rememberActiveWindow(workspace.activeWindow);
    if (workspace.windowActivated && workspace.windowActivated.connect) {
        workspace.windowActivated.connect(rememberActiveWindow);
    }

    function applyAction(action) {
        var clients = windows();
        for (var index = 0; index < clients.length; index++) {
            var client = clients[index];
            if (String(client.internalId) !== action.id) continue;
            if (action.action === "activate") {
                if (client.minimized) client.minimized = false;
                workspace.activeWindow = client;
            } else if (action.action === "minimize") {
                client.minimized = true;
            } else if (action.action === "toggle_minimize") {
                if (client.active || workspace.activeWindow === client) {
                    client.minimized = true;
                } else {
                    if (client.minimized) client.minimized = false;
                    workspace.activeWindow = client;
                }
            } else if (action.action === "toggle_maximize") {
                var maximized = client.maximized === true ||
                    (client.maximizedHorizontally === true && client.maximizedVertically === true) ||
                    client.maximizeMode === 3;
                if (typeof client.setMaximize === "function") {
                    client.setMaximize(!maximized, !maximized);
                } else if (typeof client.maximize === "function") {
                    if (maximized && typeof client.restore === "function") client.restore();
                    else client.maximize();
                }
            } else if (action.action === "close") {
                requestWindowClose(client, "task id");
            }
            return;
        }
        if (action.action === "close_active") {
            var client = workspace.activeWindow;
            var source = "active window";
            if (!isApplicationWindow(client)) {
                client = null;
                var clients = windows();
                for (var index = 0; index < clients.length; index++) {
                    if (String(clients[index].internalId) === last_active_id &&
                        isApplicationWindow(clients[index])) {
                        client = clients[index];
                        source = "last active application";
                        break;
                    }
                }
            }
            requestWindowClose(client, source);
        }
    }

    function exchange() {
        rememberActiveWindow(workspace.activeWindow);
        var clients = windows();
        var result = [];
        for (var index = 0; index < clients.length; index++) {
            var client = clients[index];
            if (client.normalWindow === false || client.skipTaskbar === true) continue;
            var maximized = client.maximized === true ||
                (client.maximizedHorizontally === true && client.maximizedVertically === true) ||
                client.maximizeMode === 3;
            result.push({xid: String(client.internalId),
                title: client.caption || client.title || "Window",
                app_id: String(client.resourceClass || client.resourceName ||
                    client.desktopFileName || ""),
                active: !!client.active || workspace.activeWindow === client,
                iconified: !!client.minimized, maximized: maximized});
        }
        callDBus(service, path, iface, "Exchange", JSON.stringify(result), function(reply) {
            try {
                var actions = JSON.parse(reply || "[]");
                if (actions === null) return;
                for (var index = 0; index < actions.length; index++) applyAction(actions[index]);
            } catch (error) {}
            exchange();
        });
    }

    exchange();
})();
)JS";
        const std::string service_placeholder = "__BUS_NAME__";
        script.replace(script.find(service_placeholder), service_placeholder.size(),
                       bus_name != nullptr ? bus_name : "");

        int script_fd = g_file_open_tmp("simpanel-kwin-tasklist-XXXXXX", &script_path_, &error);
        if (script_fd < 0 || error != nullptr) {
            const std::string message = error != nullptr ? error->message : "temporary script creation failed";
            g_clear_error(&error);
            cleanup();
            throw std::runtime_error(message);
        }
        close(script_fd);
        if (!g_file_set_contents(script_path_, script.c_str(), -1, &error)) {
            const std::string message = error != nullptr ? error->message : "temporary script write failed";
            g_clear_error(&error);
            cleanup();
            throw std::runtime_error(message);
        }

        try {
            GVariant* loaded = scripting_call("loadScript",
                g_variant_new("(ss)", script_path_, plugin_name_.c_str()), G_VARIANT_TYPE("(i)"));
            g_variant_unref(loaded);
            loaded_ = true;
            GVariant* started = scripting_call("start", nullptr, G_VARIANT_TYPE("()"));
            g_variant_unref(started);
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~KWinDbusProvider() override { cleanup(); }

    [[nodiscard]] bool supports(TaskAction) const override { return true; }

    std::vector<TaskWindowInfo> list_windows() override
    {
        return parse_task_array(snapshot_json_);
    }

    bool perform_action(const std::string& id, TaskAction action) override
    {
        if (id.empty()) return false;
        pending_actions_.push_back({id, action, false});
        complete_exchange();
        return true;
    }

    bool close_active_window() override
    {
        pending_actions_.push_back({{}, TaskAction::Close, true});
        panel_debug("close-tracker: KWin close-active request queued",
            pending_exchange_ != nullptr ? " for current exchange" : "; awaiting next exchange");
        complete_exchange();
        return true;
    }

private:
    struct PendingAction {
        std::string id;
        TaskAction action;
        bool active_window{false};
    };

    GDBusConnection* connection_{nullptr};
    GDBusNodeInfo* node_info_{nullptr};
    guint registration_id_{0};
    gchar* script_path_{nullptr};
    std::string plugin_name_;
    std::string snapshot_json_{"[]"};
    std::vector<PendingAction> pending_actions_;
    GDBusMethodInvocation* pending_exchange_{nullptr};
    guint exchange_timeout_{0};
    bool loaded_{false};
    bool stopping_{false};

    static const GDBusInterfaceVTable interface_vtable_;

    static void method_call(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                            const gchar* method, GVariant* parameters,
                            GDBusMethodInvocation* invocation, gpointer data)
    {
        auto* self = static_cast<KWinDbusProvider*>(data);
        if (std::strcmp(method, "CloseActiveResult") == 0) {
            const gchar* result = nullptr;
            g_variant_get(parameters, "(&s)", &result);
            panel_debug("close-tracker: KWin close result: ",
                result != nullptr ? result : "(empty)");
            g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
            return;
        }
        if (std::strcmp(method, "GetSnapshot") == 0) {
            g_dbus_method_invocation_return_value(invocation,
                g_variant_new("(s)", self->snapshot_json_.c_str()));
            return;
        }
        if (std::strcmp(method, "Exchange") != 0) {
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown task-list method");
            return;
        }
        const gchar* json = nullptr;
        g_variant_get(parameters, "(&s)", &json);
        self->snapshot_json_ = json != nullptr ? json : "[]";
        self->pending_exchange_ = G_DBUS_METHOD_INVOCATION(g_object_ref(invocation));
        if (self->stopping_ || !self->pending_actions_.empty()) {
            self->complete_exchange();
        } else {
            self->exchange_timeout_ = g_timeout_add(500, exchange_timeout, self);
        }
    }

    static gboolean exchange_timeout(gpointer data)
    {
        auto* self = static_cast<KWinDbusProvider*>(data);
        self->exchange_timeout_ = 0;
        self->complete_exchange();
        return G_SOURCE_REMOVE;
    }

    void complete_exchange()
    {
        if (pending_exchange_ == nullptr) return;
        if (exchange_timeout_ != 0) {
            g_source_remove(exchange_timeout_);
            exchange_timeout_ = 0;
        }
        json_object* actions = json_object_new_array();
        const std::size_t action_count = pending_actions_.size();
        for (const auto& pending : pending_actions_) {
            json_object* action = json_object_new_object();
            json_object_object_add(action, "id", json_object_new_string(pending.id.c_str()));
            const char* name = pending.active_window ? "close_active" :
                pending.action == TaskAction::Activate ? "activate" :
                pending.action == TaskAction::Minimize ? "minimize" :
                pending.action == TaskAction::ToggleMinimize ? "toggle_minimize" :
                pending.action == TaskAction::ToggleMaximize ? "toggle_maximize" : "close";
            json_object_object_add(action, "action", json_object_new_string(name));
            json_object_array_add(actions, action);
        }
        pending_actions_.clear();
        const char* response = stopping_ ? "null" :
            json_object_to_json_string_ext(actions, JSON_C_TO_STRING_PLAIN);
        if (action_count > 0) {
            panel_debug("close-tracker: sending ", action_count,
                " action(s) to KWin: ", response != nullptr ? response : "(empty)");
        }
        g_dbus_method_invocation_return_value(pending_exchange_,
            g_variant_new("(s)", response != nullptr ? response : "[]"));
        g_object_unref(pending_exchange_);
        pending_exchange_ = nullptr;
        json_object_put(actions);
    }

    GVariant* scripting_call(const char* method, GVariant* parameters,
                             const GVariantType* result_type)
    {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(connection_, "org.kde.KWin", "/Scripting",
            "org.kde.kwin.Scripting", method, parameters, result_type,
            G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error);
        if (reply == nullptr) {
            const std::string message = error != nullptr ? error->message : "KWin scripting failed";
            g_clear_error(&error);
            throw std::runtime_error(message);
        }
        g_clear_error(&error);
        return reply;
    }

    void cleanup()
    {
        stopping_ = true;
        complete_exchange();
        if (connection_ != nullptr) {
            GError* error = nullptr;
            g_dbus_connection_flush_sync(connection_, nullptr, &error);
            g_clear_error(&error);
        }
        if (loaded_ && connection_ != nullptr) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(connection_, "org.kde.KWin", "/Scripting",
                "org.kde.kwin.Scripting", "unloadScript",
                g_variant_new("(s)", plugin_name_.c_str()), G_VARIANT_TYPE("(b)"),
                G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
            if (reply != nullptr) g_variant_unref(reply);
            g_clear_error(&error);
            loaded_ = false;
        }
        if (connection_ != nullptr) {
            GError* error = nullptr;
            g_dbus_connection_flush_sync(connection_, nullptr, &error);
            g_clear_error(&error);
        }
        if (registration_id_ != 0 && connection_ != nullptr) {
            g_dbus_connection_unregister_object(connection_, registration_id_);
            registration_id_ = 0;
        }
        if (script_path_ != nullptr) {
            unlink(script_path_);
            g_clear_pointer(&script_path_, g_free);
        }
        g_clear_pointer(&node_info_, g_dbus_node_info_unref);
        g_clear_object(&connection_);
    }
};

const GDBusInterfaceVTable KWinDbusProvider::interface_vtable_ = {
    KWinDbusProvider::method_call, nullptr, nullptr, {nullptr}
};

class SwayIpcProvider final : public Provider {
public:
    SwayIpcProvider()
    {
        const char* socket = std::getenv("SWAYSOCK");
        if (socket == nullptr || *socket == '\0') socket = std::getenv("I3SOCK");
        if (socket == nullptr || *socket == '\0' || access(socket, F_OK) != 0) {
            throw std::runtime_error("Sway/i3 IPC socket unavailable");
        }
        executable_ = SystemCommands::find_executable("swaymsg");
        if (executable_.empty()) executable_ = SystemCommands::find_executable("i3-msg");
        if (executable_.empty() || SystemCommands::run({executable_, "-t", "get_tree", "-r"}).exit_code != 0) {
            throw std::runtime_error("Sway/i3 IPC tree query failed");
        }
    }

    [[nodiscard]] bool supports(TaskAction action) const override
    {
        return action == TaskAction::Activate || action == TaskAction::Close;
    }

    std::vector<TaskWindowInfo> list_windows() override
    {
        const auto output = SystemCommands::run({executable_, "-t", "get_tree", "-r"});
        if (output.exit_code != 0) return {};
        json_object* root = json_tokener_parse(output.output.c_str());
        std::vector<TaskWindowInfo> result;
        append_tree(root, result);
        if (root != nullptr) json_object_put(root);
        return result;
    }

    bool perform_action(const std::string& id, TaskAction action) override
    {
        if (!supports(action)) return false;
        const std::string verb = action == TaskAction::Close ? "kill" : "focus";
        const auto result = SystemCommands::run({executable_, "[con_id=" + id + "]", verb});
        return result.exit_code == 0;
    }

private:
    std::string executable_;

    static void append_tree(json_object* node, std::vector<TaskWindowInfo>& tasks)
    {
        if (node == nullptr || !json_object_is_type(node, json_type_object)) return;
        const std::string type = json_string(node, "type");
        const std::string title = json_string(node, "name");
        json_object* nodes = nullptr;
        json_object* floating = nullptr;
        json_object_object_get_ex(node, "nodes", &nodes);
        json_object_object_get_ex(node, "floating_nodes", &floating);
        const bool has_children = (nodes != nullptr && json_object_array_length(nodes) > 0) ||
            (floating != nullptr && json_object_array_length(floating) > 0);
        if (type == "con" && !title.empty() && !has_children) {
            std::string app_id = json_string(node, "app_id");
            json_object* properties = nullptr;
            if (app_id.empty() && json_object_object_get_ex(node, "window_properties", &properties)) {
                app_id = json_string(properties, "class");
            }
            if (!task_is_excluded(app_id, title)) {
                json_object* id = nullptr;
                json_object_object_get_ex(node, "id", &id);
                TaskWindowInfo task;
                task.id = id == nullptr ? "" : std::to_string(json_object_get_int64(id));
                task.title = title;
                task.icon_name = resolve_application_icon(app_id);
                if (task.icon_name.empty()) task.icon_name = "application-x-executable";
                task.active = json_bool(node, "focused");
                if (!task.id.empty()) tasks.push_back(std::move(task));
            }
        }
        for (json_object* children : {nodes, floating}) {
            if (children == nullptr || !json_object_is_type(children, json_type_array)) continue;
            const std::size_t count = json_object_array_length(children);
            for (std::size_t index = 0; index < count; ++index) {
                append_tree(json_object_array_get_idx(children, index), tasks);
            }
        }
    }
};
#endif

#ifdef SIMPANEL_HAVE_FOREIGN_TOPLEVEL
class ForeignToplevelProvider final : public Provider {
public:
    ForeignToplevelProvider()
    {
        display_ = wl_display_connect(nullptr);
        if (display_ == nullptr) throw std::runtime_error("Wayland connection failed");
        registry_ = wl_display_get_registry(display_);
        static const wl_registry_listener listener{global, global_remove};
        wl_registry_add_listener(registry_, &listener, this);
        if (wl_display_roundtrip(display_) < 0 || manager_ == nullptr) {
            if (manager_ != nullptr) zwlr_foreign_toplevel_manager_v1_destroy(manager_);
            if (seat_ != nullptr) wl_seat_destroy(seat_);
            if (registry_ != nullptr) wl_registry_destroy(registry_);
            wl_display_disconnect(display_);
            display_ = nullptr;
            throw std::runtime_error("foreign-toplevel manager is not advertised");
        }
        static const zwlr_foreign_toplevel_manager_v1_listener manager_listener{
            new_toplevel, manager_finished
        };
        zwlr_foreign_toplevel_manager_v1_add_listener(manager_, &manager_listener, this);
        if (wl_display_roundtrip(display_) < 0) {
            zwlr_foreign_toplevel_manager_v1_destroy(manager_);
            if (seat_ != nullptr) wl_seat_destroy(seat_);
            wl_registry_destroy(registry_);
            wl_display_disconnect(display_);
            display_ = nullptr;
            throw std::runtime_error("toplevel enumeration failed");
        }
    }

    ~ForeignToplevelProvider() override
    {
        for (auto& [id, handle] : handles_) {
            if (handle.proxy != nullptr) zwlr_foreign_toplevel_handle_v1_destroy(handle.proxy);
        }
        if (manager_ != nullptr) {
            zwlr_foreign_toplevel_manager_v1_stop(manager_);
            zwlr_foreign_toplevel_manager_v1_destroy(manager_);
        }
        if (seat_ != nullptr) wl_seat_destroy(seat_);
        for (auto& output : outputs_) wl_output_destroy(output.proxy);
        if (registry_ != nullptr) wl_registry_destroy(registry_);
        if (display_ != nullptr) {
            wl_display_roundtrip(display_);
            wl_display_disconnect(display_);
        }
    }

    [[nodiscard]] bool supports(TaskAction) const override { return true; }

    void set_monitor_filter(std::string monitor, bool enabled) override
    {
        monitor_filter_ = std::move(monitor);
        monitor_filter_enabled_ = enabled;
    }

    std::vector<TaskWindowInfo> list_windows() override
    {
        if (wl_display_roundtrip(display_) < 0) return {};
        const std::string selected_monitor = monitor_filter_ == "primary" && !outputs_.empty()
            ? outputs_.front().name : monitor_filter_;
        std::vector<TaskWindowInfo> result;
        result.reserve(handles_.size());
        for (const auto& [id, handle] : handles_) {
            if (task_is_excluded(handle.app_id, handle.title)) continue;
            if (monitor_filter_enabled_ && selected_monitor != "all" &&
                !handle.output_names.empty() &&
                std::find(handle.output_names.begin(), handle.output_names.end(),
                          selected_monitor) == handle.output_names.end()) {
                continue;
            }
            TaskWindowInfo task;
            task.id = id;
            task.title = handle.title.empty() ? "Window" : handle.title;
            task.icon_name = resolve_application_icon(handle.app_id);
            if (task.icon_name.empty()) task.icon_name = "application-x-executable";
            task.active = handle.active;
            task.iconified = handle.minimized;
            task.maximized = handle.maximized;
            result.push_back(std::move(task));
        }
        std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
            return left.id < right.id;
        });
        return result;
    }

    bool perform_action(const std::string& id, TaskAction action) override
    {
        const auto found = handles_.find(id);
        if (found == handles_.end()) return false;
        auto* proxy = found->second.proxy;
        switch (action) {
        case TaskAction::Activate:
            if (seat_ == nullptr) return false;
            zwlr_foreign_toplevel_handle_v1_activate(proxy, seat_);
            break;
        case TaskAction::Minimize:
            zwlr_foreign_toplevel_handle_v1_set_minimized(proxy);
            break;
        case TaskAction::ToggleMinimize:
            if (found->second.active) {
                zwlr_foreign_toplevel_handle_v1_set_minimized(proxy);
            } else {
                if (found->second.minimized) zwlr_foreign_toplevel_handle_v1_unset_minimized(proxy);
                if (seat_ == nullptr) return false;
                zwlr_foreign_toplevel_handle_v1_activate(proxy, seat_);
            }
            break;
        case TaskAction::ToggleMaximize:
            if (found->second.maximized) zwlr_foreign_toplevel_handle_v1_unset_maximized(proxy);
            else zwlr_foreign_toplevel_handle_v1_set_maximized(proxy);
            break;
        case TaskAction::Close:
            zwlr_foreign_toplevel_handle_v1_close(proxy);
            break;
        }
        return wl_display_flush(display_) >= 0;
    }

private:
    struct Handle {
        ForeignToplevelProvider* owner{nullptr};
        zwlr_foreign_toplevel_handle_v1* proxy{nullptr};
        std::string id;
        std::string title{"Window"};
        std::string app_id;
        std::vector<std::string> output_names;
        bool active{false};
        bool minimized{false};
        bool maximized{false};
    };

    wl_display* display_{nullptr};
    wl_registry* registry_{nullptr};
    wl_seat* seat_{nullptr};
    struct Output {
        wl_output* proxy;
        std::string name;
    };
    std::vector<Output> outputs_;
    zwlr_foreign_toplevel_manager_v1* manager_{nullptr};
    std::string monitor_filter_{"all"};
    bool monitor_filter_enabled_{false};
    std::uint64_t next_id_{1};
    std::map<std::string, Handle> handles_;

    static void global(void* data, wl_registry* registry, uint32_t name,
                       const char* interface, uint32_t version)
    {
        auto* self = static_cast<ForeignToplevelProvider*>(data);
        if (std::strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0 &&
            self->manager_ == nullptr) {
            self->manager_ = static_cast<zwlr_foreign_toplevel_manager_v1*>(wl_registry_bind(
                registry, name, &zwlr_foreign_toplevel_manager_v1_interface,
                std::min(version, 3u)));
        } else if (std::strcmp(interface, wl_seat_interface.name) == 0 && self->seat_ == nullptr) {
            self->seat_ = static_cast<wl_seat*>(wl_registry_bind(
                registry, name, &wl_seat_interface, std::min(version, 7u)));
        } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
            wl_output* output = static_cast<wl_output*>(wl_registry_bind(
                registry, name, &wl_output_interface, std::min(version, 4u)));
            self->outputs_.push_back({output, "output-" + std::to_string(name)});
            static const wl_output_listener listener{
                output_geometry, output_mode, output_done, output_scale,
                output_name, output_description
            };
            wl_output_add_listener(output, &listener, self);
        }
    }

    static void global_remove(void*, wl_registry*, uint32_t) {}

    static void new_toplevel(void* data, zwlr_foreign_toplevel_manager_v1*,
                             zwlr_foreign_toplevel_handle_v1* proxy)
    {
        auto* self = static_cast<ForeignToplevelProvider*>(data);
        Handle state;
        state.owner = self;
        state.proxy = proxy;
        state.id = "foreign-" + std::to_string(self->next_id_++);
        const std::string id = state.id;
        auto [found, inserted] = self->handles_.emplace(id, std::move(state));
        (void)inserted;
        static const zwlr_foreign_toplevel_handle_v1_listener listener{
            title, app_id, output_enter, output_leave, state_changed, done, closed, parent
        };
        zwlr_foreign_toplevel_handle_v1_add_listener(proxy, &listener, &found->second);
    }

    static void manager_finished(void*, zwlr_foreign_toplevel_manager_v1*) {}
    static void title(void* data, zwlr_foreign_toplevel_handle_v1*, const char* value)
    {
        static_cast<Handle*>(data)->title = value != nullptr && *value != '\0' ? value : "Window";
    }
    static void app_id(void* data, zwlr_foreign_toplevel_handle_v1*, const char* value)
    {
        static_cast<Handle*>(data)->app_id = value != nullptr ? value : "";
    }
    static void output_enter(void* data, zwlr_foreign_toplevel_handle_v1*, wl_output* proxy)
    {
        auto* handle = static_cast<Handle*>(data);
        const auto found = std::find_if(handle->owner->outputs_.begin(),
            handle->owner->outputs_.end(),
            [proxy](const Output& output) { return output.proxy == proxy; });
        if (found != handle->owner->outputs_.end() &&
            std::find(handle->output_names.begin(), handle->output_names.end(),
                      found->name) == handle->output_names.end()) {
            handle->output_names.push_back(found->name);
        }
    }

    static void output_leave(void* data, zwlr_foreign_toplevel_handle_v1*, wl_output* proxy)
    {
        auto* handle = static_cast<Handle*>(data);
        const auto found = std::find_if(handle->owner->outputs_.begin(),
            handle->owner->outputs_.end(),
            [proxy](const Output& output) { return output.proxy == proxy; });
        if (found != handle->owner->outputs_.end()) {
            std::erase(handle->output_names, found->name);
        }
    }

    static void output_geometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t,
                                int32_t, const char*, const char*, int32_t) {}

    static void output_mode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}

    static void output_done(void*, wl_output*) {}

    static void output_scale(void*, wl_output*, int32_t) {}

    static void output_name(void* data, wl_output* proxy, const char* name)
    {
        auto* self = static_cast<ForeignToplevelProvider*>(data);
        const auto found = std::find_if(self->outputs_.begin(), self->outputs_.end(),
            [proxy](const Output& output) { return output.proxy == proxy; });
        if (found != self->outputs_.end()) {
            found->name = name;
        }
    }

    static void output_description(void*, wl_output*, const char*) {}
    static void state_changed(void* data, zwlr_foreign_toplevel_handle_v1*, wl_array* states)
    {
        auto* handle = static_cast<Handle*>(data);
        handle->active = handle->minimized = handle->maximized = false;
        if (states == nullptr) return;
        const auto* values = static_cast<const uint32_t*>(states->data);
        const std::size_t count = states->size / sizeof(uint32_t);
        for (std::size_t index = 0; index < count; ++index) {
            if (values[index] == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) handle->active = true;
            else if (values[index] == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED) handle->minimized = true;
            else if (values[index] == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED) handle->maximized = true;
        }
    }
    static void done(void*, zwlr_foreign_toplevel_handle_v1*) {}
    static void closed(void* data, zwlr_foreign_toplevel_handle_v1*)
    {
        auto* handle = static_cast<Handle*>(data);
        auto* self = handle->owner;
        if (self == nullptr) return;
        const auto found = self->handles_.find(handle->id);
        if (found != self->handles_.end()) {
            zwlr_foreign_toplevel_handle_v1_destroy(found->second.proxy);
            self->handles_.erase(found);
        }
    }
    static void parent(void*, zwlr_foreign_toplevel_handle_v1*,
                       zwlr_foreign_toplevel_handle_v1*) {}
};
#endif

} // namespace

class WaylandTaskBackend::Impl {
public:
    explicit Impl(std::unique_ptr<Provider> provider) : provider_(std::move(provider)) {}
    std::unique_ptr<Provider> provider_;
};

WaylandTaskBackend::WaylandTaskBackend(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WaylandTaskBackend::~WaylandTaskBackend() = default;

std::unique_ptr<WaylandTaskBackend> WaylandTaskBackend::create()
{
    const char* desktop_value = std::getenv("XDG_CURRENT_DESKTOP");
    const std::string desktop = lowercase(desktop_value != nullptr ? desktop_value : "");
    const char* compositor = std::getenv("MDEON_SESSION_COMPOSITOR");
#ifdef SIMPANEL_HAVE_JSON_C
    if (desktop.find("gnome") != std::string::npos ||
        (compositor != nullptr && std::string(compositor) == "gnome-shell")) {
        auto bridge = create_gnome_bridge();
        if (bridge != nullptr) return bridge;
    }
    try {
        return std::unique_ptr<WaylandTaskBackend>(new WaylandTaskBackend(
            std::make_unique<Impl>(std::make_unique<KWinDbusProvider>())));
    } catch (const std::exception&) {}
#endif
    if (SystemCommands::find_executable("wlrctl").empty() ||
        SystemCommands::run({"wlrctl", "toplevel", "list"}).exit_code != 0) {
#ifdef SIMPANEL_HAVE_JSON_C
        if (std::getenv("SWAYSOCK") != nullptr || std::getenv("I3SOCK") != nullptr) {
            try {
                return std::unique_ptr<WaylandTaskBackend>(new WaylandTaskBackend(
                    std::make_unique<Impl>(std::make_unique<SwayIpcProvider>())));
            } catch (const std::exception&) {}
        }
#endif
#ifdef SIMPANEL_HAVE_FOREIGN_TOPLEVEL
        try {
            return std::unique_ptr<WaylandTaskBackend>(new WaylandTaskBackend(
                std::make_unique<Impl>(std::make_unique<ForeignToplevelProvider>())));
        } catch (const std::exception&) {}
#endif
    }
    return nullptr;
}

std::unique_ptr<WaylandTaskBackend> WaylandTaskBackend::create_gnome_bridge()
{
#ifdef SIMPANEL_HAVE_JSON_C
    static std::once_flag enable_checked;
    try {
        return std::unique_ptr<WaylandTaskBackend>(new WaylandTaskBackend(
            std::make_unique<Impl>(std::make_unique<GnomeDbusProvider>())));
    } catch (const std::exception&) {
        std::call_once(enable_checked, enable_gnome_extension_if_disabled);
    }
#endif
    return nullptr;
}

bool WaylandTaskBackend::supports_context_menu() const
{
    return impl_ != nullptr && impl_->provider_ != nullptr;
}

bool WaylandTaskBackend::supports_action(TaskAction action) const
{
    return impl_ != nullptr && impl_->provider_ != nullptr && impl_->provider_->supports(action);
}

void WaylandTaskBackend::set_monitor_filter(std::string monitor, bool enabled)
{
    if (impl_ != nullptr && impl_->provider_ != nullptr) {
        impl_->provider_->set_monitor_filter(std::move(monitor), enabled);
    }
}

std::vector<TaskWindowInfo> WaylandTaskBackend::list_windows()
{
    return impl_ != nullptr ? impl_->provider_->list_windows() : std::vector<TaskWindowInfo>{};
}

bool WaylandTaskBackend::perform_action(const std::string& id, TaskAction action)
{
    return impl_ != nullptr && impl_->provider_ != nullptr &&
        impl_->provider_->perform_action(id, action);
}

bool WaylandTaskBackend::close_active_window()
{
    return impl_ != nullptr && impl_->provider_ != nullptr &&
        impl_->provider_->close_active_window();
}

bool WaylandTaskBackend::supports_workspaces() const
{
    return impl_ != nullptr && impl_->provider_ != nullptr &&
        impl_->provider_->supports_workspaces();
}

WorkspaceSnapshot WaylandTaskBackend::get_workspaces()
{
    return impl_ != nullptr && impl_->provider_ != nullptr
        ? impl_->provider_->get_workspaces() : WorkspaceSnapshot{};
}

bool WaylandTaskBackend::configure_workspace_count(int count)
{
    return impl_ != nullptr && impl_->provider_ != nullptr &&
        impl_->provider_->configure_workspace_count(count);
}

bool WaylandTaskBackend::switch_workspace(int id)
{
    return impl_ != nullptr && impl_->provider_ != nullptr &&
        impl_->provider_->switch_workspace(id);
}

bool WaylandTaskBackend::supports_show_desktop() const
{
    return impl_ != nullptr && impl_->provider_ != nullptr &&
        impl_->provider_->supports_show_desktop();
}

bool WaylandTaskBackend::get_show_desktop_state(bool& showing)
{
    return impl_ != nullptr && impl_->provider_ != nullptr &&
        impl_->provider_->get_show_desktop_state(showing);
}

bool WaylandTaskBackend::toggle_show_desktop()
{
    return impl_ != nullptr && impl_->provider_ != nullptr &&
        impl_->provider_->toggle_show_desktop();
}

bool WaylandTaskBackend::supports_window_kill_mode() const
{
    return impl_ != nullptr && impl_->provider_ != nullptr &&
        impl_->provider_->supports_window_kill_mode();
}

bool WaylandTaskBackend::begin_window_kill_mode()
{
    return impl_ != nullptr && impl_->provider_ != nullptr &&
        impl_->provider_->begin_window_kill_mode();
}

bool WaylandTaskBackend::window_kill_mode_active() const
{
    return impl_ != nullptr && impl_->provider_ != nullptr &&
        impl_->provider_->window_kill_mode_active();
}

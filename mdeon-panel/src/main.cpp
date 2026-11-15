#include "panel/backends/x11_backend.hpp"
#ifdef SIMPANEL_HAVE_WAYLAND
#include "panel/backends/wayland_backend.hpp"
#endif
#include "panel/config.hpp"
#include "panel/icon_theme.hpp"
#include "panel/logging.hpp"
#include "panel/modules/battery.hpp"
#include "panel/modules/bluetooth.hpp"
#include "panel/modules/clock.hpp"
#include "panel/modules/custom_button.hpp"
#include "panel/modules/executer.hpp"
#include "panel/modules/launcher.hpp"
#include "panel/modules/application_menu.hpp"
#include "panel/modules/desktop_hub.hpp"
#include "panel/modules/network.hpp"
#include "panel/modules/notifications.hpp"
#include "panel/modules/spacer.hpp"
#include "panel/modules/show_desktop.hpp"
#include "panel/modules/system_commands.hpp"
#include "panel/modules/task_list.hpp"
#include "panel/modules/tray.hpp"
#include "panel/modules/volume.hpp"
#include "panel/modules/virtual_desktop.hpp"
#include "panel/panel.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cerrno>
#include <csignal>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>

#include <glib-unix.h>

namespace {

constexpr char panel_control_name[] = "org.simpanel.Panel";
constexpr char panel_control_path[] = "/org/simpanel/Panel";
constexpr char panel_control_interface[] = "org.simpanel.Panel";
constexpr char panel_control_xml[] =
    "<node><interface name='org.simpanel.Panel'>"
    "<method name='OpenMenu'/>"
    "</interface></node>";

void panel_control_method_call(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                               const gchar* method, GVariant*,
                               GDBusMethodInvocation* invocation, gpointer data)
{
    auto* menu = static_cast<ApplicationMenuModule*>(data);
    if (g_str_equal(method, "OpenMenu")) {
        menu->open_menu_from_command();
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }
}

const GDBusInterfaceVTable panel_control_vtable = {
    panel_control_method_call, nullptr, nullptr, {nullptr}
};

bool forward_menu_request()
{
    GError* error = nullptr;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (connection == nullptr) {
        g_clear_error(&error);
        return false;
    }
    GVariant* result = g_dbus_connection_call_sync(connection, panel_control_name,
        panel_control_path, panel_control_interface, "OpenMenu", nullptr, nullptr,
        G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
    const bool forwarded = result != nullptr;
    if (result != nullptr) g_variant_unref(result);
    g_clear_error(&error);
    g_object_unref(connection);
    return forwarded;
}

struct PanelSupervisorState {
    IBackend* backend;
    std::string executable;
    GMainLoop* loop;
    std::unordered_map<std::string, pid_t> children;
};

void stop_panel_child(pid_t child)
{
    if (child <= 0 || kill(child, SIGTERM) != 0) {
        return;
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
}

gboolean refresh_panel_children(gpointer data)
{
    auto* state = static_cast<PanelSupervisorState*>(data);
    const std::vector<std::string> outputs = state->backend->list_monitors();
    if (outputs.empty()) {
        return G_SOURCE_CONTINUE;
    }
    const std::set<std::string> active_outputs(outputs.begin(), outputs.end());

    for (auto child = state->children.begin(); child != state->children.end();) {
        int status = 0;
        const pid_t result = waitpid(child->second, &status, WNOHANG);
        const bool exited = result == child->second || (result < 0 && errno == ECHILD);
        const bool disconnected = active_outputs.find(child->first) == active_outputs.end();
        if (exited || disconnected) {
            if (!exited) {
                stop_panel_child(child->second);
            }
            child = state->children.erase(child);
        } else {
            ++child;
        }
    }

    for (const auto& output : active_outputs) {
        if (state->children.find(output) != state->children.end()) {
            continue;
        }
        const pid_t child = fork();
        if (child == 0) {
            setenv("MDEON_PANEL_MONITOR", output.c_str(), 1);
            setenv("MDEON_PANEL_SUPERVISED", "1", 1);
            execl(state->executable.c_str(), state->executable.c_str(),
                  static_cast<char*>(nullptr));
            _exit(127);
        }
        if (child > 0) {
            state->children.emplace(output, child);
            panel_debug("started panel for output '", output, "' pid=", child);
        } else {
            panel_debug("failed to start panel for output '", output, "'");
        }
    }
    return G_SOURCE_CONTINUE;
}

gboolean stop_panel_supervisor(gpointer data)
{
    auto* state = static_cast<PanelSupervisorState*>(data);
    g_main_loop_quit(state->loop);
    return G_SOURCE_CONTINUE;
}

bool run_panel_supervisor(IBackend& backend)
{
    if (backend.list_monitors().empty()) {
        return false;
    }
    std::array<char, 4096> executable_path{};
    const ssize_t path_length = readlink("/proc/self/exe", executable_path.data(),
                                         executable_path.size() - 1);
    if (path_length <= 0) {
        return false;
    }
    PanelSupervisorState state{
        &backend, std::string(executable_path.data(), static_cast<std::size_t>(path_length)),
        g_main_loop_new(nullptr, FALSE), {}};
    refresh_panel_children(&state);
    const guint refresh_source = g_timeout_add_seconds(2, refresh_panel_children, &state);
    const guint term_source = g_unix_signal_add(SIGTERM, stop_panel_supervisor, &state);
    const guint int_source = g_unix_signal_add(SIGINT, stop_panel_supervisor, &state);
    panel_debug("monitor supervisor started");
    g_main_loop_run(state.loop);
    g_source_remove(refresh_source);
    if (term_source != 0) g_source_remove(term_source);
    if (int_source != 0) g_source_remove(int_source);
    for (const auto& [output, child] : state.children) {
        panel_debug("stopping panel for output '", output, "' pid=", child);
        stop_panel_child(child);
    }
    g_main_loop_unref(state.loop);
    return true;
}

}

int main(int argc, char** argv)
{
    bool open_menu_on_start = false;
    if (argc > 1) {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "Usage: mdeon-panel [--menu]\n";
            return 0;
        }
        if (argc != 2 || std::string(argv[1]) != "--menu") {
            std::cerr << "Usage: mdeon-panel [--menu]\n";
            return 2;
        }
        if (forward_menu_request()) {
            return 0;
        }
        open_menu_on_start = true;
    }

    const PanelConfiguration config = load_panel_configuration();
    const auto panel_items_setting = config.values.find("panel_items");
    const std::string panel_items = panel_items_setting == config.values.end()
        ? "PPPLTSUVNKBC" : panel_items_setting->second;
    for (const char item : {'J'}) {
        if (panel_items.find(item) != std::string::npos) {
            std::cerr << "Configured plugin '" << item
                      << "' is retained in the project but is not yet ported to the Cairo runtime.\n";
        }
    }
    const char* requested_backend = std::getenv("MDEON_PANEL_BACKEND");
    if (requested_backend == nullptr || *requested_backend == '\0') {
        requested_backend = std::getenv("SIMPANEL_BACKEND");
    }
    const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
    const char* session = std::getenv("XDG_SESSION_TYPE");
    std::string desktop_names = desktop != nullptr ? desktop : "";
    const char* session_desktop = std::getenv("XDG_SESSION_DESKTOP");
    if (session_desktop != nullptr) {
        desktop_names += ":";
        desktop_names += session_desktop;
    }
    std::transform(desktop_names.begin(), desktop_names.end(), desktop_names.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    const char* compositor = std::getenv("MDEON_SESSION_COMPOSITOR");
    const bool gnome_wayland_session = std::getenv("WAYLAND_DISPLAY") != nullptr &&
        (desktop_names.find("gnome") != std::string::npos ||
         (compositor != nullptr && std::string(compositor) == "gnome-shell") ||
         std::getenv("GNOME_SETUP_DISPLAY") != nullptr);
    const std::string backend_name = requested_backend != nullptr && *requested_backend != '\0'
        ? (gnome_wayland_session ? "x11" : requested_backend)
        : (gnome_wayland_session ? "x11"
            : (std::getenv("WAYLAND_DISPLAY") != nullptr ? "wayland" : "x11"));
    panel_debug("startup environment: backend=", backend_name,
        " desktop=", desktop != nullptr ? desktop : "(unset)",
        " session=", session != nullptr ? session : "(unset)",
        " gnome_wayland_session=", gnome_wayland_session ? "yes" : "no",
        " wayland_display=", std::getenv("WAYLAND_DISPLAY") != nullptr ? "set" : "unset",
        " display=", std::getenv("DISPLAY") != nullptr ? "set" : "unset");
    panel_debug("configuration loaded: panel_items='", panel_items, "'");
    std::unique_ptr<IBackend> backend;

    if (backend_name == "wayland") {
#ifdef SIMPANEL_HAVE_WAYLAND
        backend = std::make_unique<WaylandBackend>();
#else
        std::cerr << "Wayland support was not built; install libwayland-dev, wayland-protocols, and libwayland-bin.\n";
        return 1;
#endif
    } else if (backend_name == "x11") {
        if (std::getenv("DISPLAY") == nullptr) {
            std::cerr << "No X11 display detected.\n";
            return 1;
        }
        auto x11 = std::make_unique<X11Backend>();
        backend = std::move(x11);
    } else {
        std::cerr << "SIMPANEL_BACKEND must be 'x11' or 'wayland'.\n";
        return 1;
    }

    const auto position_setting = config.values.find("panel_position");
    const std::string panel_position = position_setting == config.values.end()
        ? "bottom center horizontal" : position_setting->second;
    std::istringstream position_parts(panel_position);
    std::string panel_edge_name;
    position_parts >> panel_edge_name;
    std::transform(panel_edge_name.begin(), panel_edge_name.end(), panel_edge_name.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    const Edge panel_edge = panel_edge_name == "top" ? Edge::Top :
        panel_edge_name == "left" ? Edge::Left :
        panel_edge_name == "right" ? Edge::Right : Edge::Bottom;
    const bool panel_vertical = panel_edge == Edge::Left || panel_edge == Edge::Right;

    int configured_panel_height = 30;
    auto panel_thickness_setting = config.values.find("panel_thickness");
    try {
        if (panel_thickness_setting != config.values.end()) {
            configured_panel_height = std::max(24, std::stoi(panel_thickness_setting->second));
        } else {
            const auto panel_size_setting = config.values.find("panel_size");
            if (panel_size_setting != config.values.end()) {
                std::istringstream panel_size(panel_size_setting->second);
                std::string panel_width;
                int panel_height = 0;
                if (panel_size >> panel_width >> panel_height) {
                    configured_panel_height = std::max(24, panel_height);
                }
            }
        }
    } catch (...) {
        configured_panel_height = 30;
    }
    const auto monitor_setting = config.values.find("panel_monitor");
    const char* output_override = std::getenv("MDEON_PANEL_MONITOR");
    const std::string panel_monitor = output_override != nullptr && *output_override != '\0'
        ? output_override : monitor_setting == config.values.end()
            ? "all" : monitor_setting->second;
    const auto monitor_filter_setting = config.values.find("task_monitor_filter");
    const bool task_monitor_filter = monitor_filter_setting == config.values.end() ||
        (monitor_filter_setting->second != "0" &&
         monitor_filter_setting->second != "false" &&
         monitor_filter_setting->second != "no" &&
         monitor_filter_setting->second != "off");
    const char* supervisor_child = std::getenv("MDEON_PANEL_SUPERVISED");
    if (argc == 1 && panel_monitor == "all" && output_override == nullptr &&
        supervisor_child == nullptr && run_panel_supervisor(*backend)) {
        return 0;
    }
    backend->set_task_monitor_filter(task_monitor_filter);
    if (!backend->set_monitor(panel_monitor) ||
        !backend->init(panel_vertical ? configured_panel_height : 0,
                       panel_vertical ? 0 : configured_panel_height, panel_edge)) {
        return 1;
    }
    panel_debug("backend ready: name=", backend_name, " size=", backend->width(), "x",
        backend->height(), " task_list=", backend->supports_task_list(),
        " workspaces=", backend->supports_workspaces(),
        " show_desktop=", backend->supports_show_desktop());

    IBackend* panel_backend = backend.get();
    Panel panel(std::move(backend), panel_vertical);
    const bool menu_button_visible = panel_items.find('M') != std::string::npos;
    auto application_menu = std::make_unique<ApplicationMenuModule>(
        *panel_backend, config.values, menu_button_visible, panel_vertical,
        [&panel] { panel.request_redraw(); });
    ApplicationMenuModule* application_menu_ptr = application_menu.get();
    std::size_t button_index = 0;
    std::size_t executer_index = 0;
    const auto configured = [&config](const std::string& key, const std::string& fallback) {
        const auto value = config.values.find(key);
        return value == config.values.end() ? fallback : value->second;
    };
    const auto enabled = [](const std::string& value) {
        return value == "1" || value == "true" || value == "yes" || value == "on";
    };
    for (const char item : panel_items) {
        switch (item) {
        case 'M':
            if (application_menu != nullptr) {
                panel.add_module(std::move(application_menu), Alignment::Start);
            }
            break;
        case 'D':
            if (panel_backend->supports_show_desktop()) {
                panel.add_module(std::make_unique<ShowDesktopModule>(*panel_backend), Alignment::Start);
            }
            break;
        case 'W':
            if (panel_backend->supports_workspaces()) {
                const auto count = config.values.find("virtual_desktop_workspaces");
                int workspace_count = 2;
                if (count != config.values.end()) {
                    try { workspace_count = std::max(1, std::stoi(count->second)); } catch (...) {}
                }
                const auto mode = config.values.find("virtual_desktop_display");
                const auto icon_size_setting = config.values.find("module_icon_size");
                int vertical_padding = 0;
                const auto panel_padding_setting = config.values.find("panel_padding");
                if (panel_padding_setting != config.values.end()) {
                    std::istringstream padding(panel_padding_setting->second);
                    int horizontal = 0;
                    padding >> horizontal >> vertical_padding;
                    vertical_padding = std::max(0, vertical_padding);
                }
                const int usable_icon_size = std::max(12,
                    configured_panel_height - 2 * vertical_padding - 2);
                int icon_size = usable_icon_size;
                if (icon_size_setting != config.values.end()) {
                    try {
                        const int configured_size = std::stoi(icon_size_setting->second);
                        if (configured_size > 0) {
                            icon_size = std::max(12, std::min(configured_size, usable_icon_size));
                        }
                    } catch (...) {}
                }
                panel.add_module(std::make_unique<VirtualDesktopModule>(*panel_backend,
                    workspace_count, mode == config.values.end() ? "icon_number" : mode->second,
                    icon_size), Alignment::Start);
            }
            break;
        case 'H':
            panel.add_module(std::make_unique<DesktopHubModule>(*panel_backend, config.values),
                             Alignment::Start);
            break;
        case 'P':
            if (button_index < config.buttons.size()) {
                panel.add_module(std::make_unique<CustomButtonModule>(
                    config.buttons[button_index], static_cast<int>(button_index + 1)),
                    Alignment::Start);
            }
            ++button_index;
            break;
        case 'L': {
            int launcher_icon_size = std::max(12, configured_panel_height - 8);
            const auto size = config.values.find("launcher_icon_size");
            if (size != config.values.end()) {
                try {
                    const int configured_size = std::stoi(size->second);
                    if (configured_size > 0) launcher_icon_size = configured_size;
                } catch (...) {}
            }
            const auto tooltip_setting = config.values.find("launcher_tooltip");
            const bool show_tooltips = tooltip_setting == config.values.end() ||
                (tooltip_setting->second != "0" && tooltip_setting->second != "false");
            int launcher_max_icons = 5;
            const auto max_icons = config.values.find("launcher_max_icons");
            if (max_icons != config.values.end()) {
                try { launcher_max_icons = std::max(1, std::stoi(max_icons->second)); } catch (...) {}
            }
            panel.add_module(std::make_unique<LauncherModule>(config.launcher_apps,
                launcher_icon_size, launcher_max_icons, show_tooltips, panel_vertical),
                Alignment::Start);
            break;
        }
        case 'F': {
            int spacer_length = 0;
            try { spacer_length = std::stoi(configured("spacer_length", "0")); } catch (...) {}
            panel.add_module(std::make_unique<SpacerModule>(spacer_length,
                enabled(configured("spacer_bar", "0")),
                enabled(configured("spacer_bar_start", "1")),
                enabled(configured("spacer_bar_middle", "0")),
                enabled(configured("spacer_bar_end", "0"))), Alignment::Start);
            break;
        }
        case 'E': {
            auto executer_config = config.values;
            if (executer_index < config.executers.size()) {
                for (const auto& [key, value] : config.executers[executer_index]) {
                    executer_config.insert_or_assign(key, value);
                }
            }
            panel.add_module(std::make_unique<ExecuterModule>(std::move(executer_config)),
                             Alignment::Start);
            ++executer_index;
            break;
        }
        case 'T':
            if (panel_backend->supports_task_list()) {
                panel.add_module(std::make_unique<TaskListModule>(
                    *panel_backend, config.values, panel_vertical), Alignment::Start);
            }
            break;
        case 'S':
            panel.add_module(std::make_unique<TrayModule>(panel_vertical), Alignment::End);
            break;
        case 'U': {
            const auto proxy_destination = config.values.find("notification_proxy_destination");
            panel.add_module(std::make_unique<NotificationsModule>(
                [panel_backend](const std::string& text, int x, int y) {
                    panel_backend->show_tooltip(text, x, y);
                }, proxy_destination == config.values.end() ? std::string{} : proxy_destination->second,
                panel_vertical), Alignment::End);
            break;
        }
        case 'V':
            panel.add_module(std::make_unique<VolumeModule>(), Alignment::End);
            break;
        case 'N':
            panel.add_module(std::make_unique<NetworkModule>(), Alignment::End);
            break;
        case 'K':
            panel.add_module(std::make_unique<BluetoothModule>(), Alignment::End);
            break;
        case 'B':
            panel.add_module(std::make_unique<BatteryModule>(panel_vertical), Alignment::End);
            break;
        case 'C':
            panel.add_module(std::make_unique<ClockModule>([panel_backend](int x, int width) {
                panel_backend->toggle_calendar(x, width);
            }, panel_vertical), Alignment::End);
            break;
        default:
            break;
        }
    }

    GError* control_error = nullptr;
    GDBusConnection* control_connection = g_bus_get_sync(
        G_BUS_TYPE_SESSION, nullptr, &control_error);
    GDBusNodeInfo* control_node = control_connection != nullptr
        ? g_dbus_node_info_new_for_xml(panel_control_xml, &control_error)
        : nullptr;
    guint control_registration = 0;
    guint control_owner = 0;
    if (control_connection != nullptr && control_node != nullptr) {
        control_registration = g_dbus_connection_register_object(
            control_connection, panel_control_path, control_node->interfaces[0],
            &panel_control_vtable, application_menu_ptr, nullptr, &control_error);
        if (control_registration != 0) {
            control_owner = g_bus_own_name_on_connection(
                control_connection, panel_control_name, G_BUS_NAME_OWNER_FLAGS_NONE,
                nullptr, nullptr, nullptr, nullptr);
        }
    }
    if (control_error != nullptr) {
        g_warning("Could not register panel menu control: %s", control_error->message);
        g_error_free(control_error);
    }
    if (open_menu_on_start) {
        const auto open_menu = [](gpointer data) -> gboolean {
            static_cast<ApplicationMenuModule*>(data)->open_menu_from_command();
            return G_SOURCE_REMOVE;
        };
        if (backend_name == "wayland") {
            g_timeout_add(1500, open_menu, application_menu_ptr);
        } else {
            g_idle_add(open_menu, application_menu_ptr);
        }
    }
    panel_debug("starting panel event loop: configured items='", panel_items, "'");
    set_theme_icon_redraw_callback([&panel] { panel.request_redraw(); });
    panel.run_loop();
    set_theme_icon_redraw_callback({});

    if (control_owner != 0) g_bus_unown_name(control_owner);
    if (control_registration != 0 && control_connection != nullptr) {
        g_dbus_connection_unregister_object(control_connection, control_registration);
    }
    if (control_node != nullptr) g_dbus_node_info_unref(control_node);
    if (control_connection != nullptr) g_object_unref(control_connection);
    return 0;
}
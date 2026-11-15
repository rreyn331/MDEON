#include "panel/modules/show_desktop.hpp"

#include "panel/icon_theme.hpp"
#include "panel/logging.hpp"
#include "panel/modules/system_commands.hpp"

#include <chrono>
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

ShowDesktopModule::ShowDesktopModule(IBackend& backend) : backend_(backend)
{
}

ShowDesktopModule::~ShowDesktopModule()
{
    if (pending_xkill_source_ != 0) {
        g_source_remove(pending_xkill_source_);
        pending_xkill_source_ = 0;
    }
    if (xkill_watch_id_ != 0) {
        g_source_remove(xkill_watch_id_);
        xkill_watch_id_ = 0;
    }
    backend_.release_escape_key();
    if (xkill_pid_ > 0) {
        kill(xkill_pid_, SIGTERM);
        int status = 0;
        while (waitpid(xkill_pid_, &status, 0) < 0 && errno == EINTR) {
        }
        g_spawn_close_pid(xkill_pid_);
        xkill_pid_ = 0;
    }
}

bool ShowDesktopModule::update()
{
    const auto now = std::chrono::steady_clock::now();
    if (now < next_refresh_) {
        return false;
    }
    next_refresh_ = now + (kill_mode_active_ ? std::chrono::milliseconds(300)
                                             : std::chrono::seconds(2));
    const bool previous_showing = showing_desktop_;
    const bool previous_available = state_available_;
    const bool previous_kill_mode_active = kill_mode_active_;
    if (!kill_mode_active_) {
        bool showing = showing_desktop_;
        state_available_ = backend_.get_show_desktop_state(showing);
        if (state_available_) {
            showing_desktop_ = showing;
        }
    }
    if (backend_.supports_native_window_kill()) {
        kill_mode_active_ = backend_.native_window_kill_active();
    } else {
        kill_mode_active_ = pending_xkill_source_ != 0 || xkill_pid_ > 0;
    }
    return previous_showing != showing_desktop_ || previous_available != state_available_ ||
        previous_kill_mode_active != kill_mode_active_;
}

int ShowDesktopModule::measure_width(cairo_t*, int)
{
    return 30;
}

void ShowDesktopModule::render(cairo_t* cr, const Geometry& geometry)
{
    if (kill_mode_active_) {
        cairo_set_source_rgba(cr, 0.85, 0.15, 0.15, 0.38);
        cairo_rectangle(cr, geometry.x + 1, geometry.y + 1,
                        std::max(0, geometry.width - 2),
                        std::max(0, geometry.height - 2));
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.95, 0.2, 0.2);
        cairo_set_line_width(cr, 2.0);
        cairo_stroke(cr);
    }
    paint_theme_icon(cr, "/usr/share/icons/ExposeAir/vertical/places/user-folders/user-desktop.svg",
                     geometry.x + geometry.width / 2.0,
                     geometry.y + geometry.height / 2.0, 28);
}

void ShowDesktopModule::on_click(int button, int, int)
{
    if (button == 1 && backend_.supports_show_desktop()) {
        backend_.toggle_show_desktop();
        next_refresh_ = {};
    } else if (button == 3) {
        panel_debug("close-tracker: right-click received; opening window-close action");
        if (backend_.supports_native_window_kill()) {
            if (!backend_.start_native_window_kill()) {
                g_warning("GNOME native window-kill mode could not start");
            } else {
                kill_mode_active_ = true;
                next_refresh_ = {};
                panel_debug("close-tracker: native window-kill mode started");
            }
            return;
        }
        if (backend_.supports_xkill()) {
            if (pending_xkill_source_ != 0 || xkill_pid_ > 0) {
                return;
            }
            if (!backend_.grab_escape_key([this] { terminate_xkill(); })) {
                g_warning("Could not grab Escape; xkill was not started");
                return;
            }
            kill_mode_active_ = true;
            next_refresh_ = {};
            pending_xkill_source_ = g_timeout_add(150, [](gpointer data) -> gboolean {
                return static_cast<ShowDesktopModule*>(data)->start_xkill_after_click();
            }, this);
        } else if (backend_.supports_confirmed_window_close()) {
            panel_debug("close-tracker: opening confirmed window-close menu");
            backend_.confirm_close_active_window();
        } else {
            panel_debug("close-tracker: no supported window-close action on this backend");
        }
    }
}

gboolean ShowDesktopModule::start_xkill_after_click()
{
    pending_xkill_source_ = 0;
    const std::string executable = SystemCommands::find_executable("xkill");
    if (executable.empty()) {
        g_warning("xkill is not installed");
        backend_.release_escape_key();
        next_refresh_ = {};
        return G_SOURCE_REMOVE;
    }

    const pid_t child = fork();
    if (child == 0) {
        execl(executable.c_str(), "xkill", static_cast<char*>(nullptr));
        _exit(127);
    }
    if (child < 0) {
        g_warning("Could not start xkill");
        backend_.release_escape_key();
        next_refresh_ = {};
        return G_SOURCE_REMOVE;
    }

    xkill_pid_ = child;
    xkill_watch_id_ = g_child_watch_add(xkill_pid_, [](GPid pid, gint, gpointer data) {
        auto* self = static_cast<ShowDesktopModule*>(data);
        if (self->xkill_pid_ == pid) {
            self->xkill_pid_ = 0;
            self->xkill_watch_id_ = 0;
            g_spawn_close_pid(pid);
            self->backend_.release_escape_key();
            self->next_refresh_ = {};
        }
    }, this);
    return G_SOURCE_REMOVE;
}

void ShowDesktopModule::terminate_xkill()
{
    if (pending_xkill_source_ != 0) {
        g_source_remove(pending_xkill_source_);
        pending_xkill_source_ = 0;
        next_refresh_ = {};
    }
    if (xkill_pid_ > 0) {
        kill(xkill_pid_, SIGTERM);
    } else {
        backend_.release_escape_key();
    }
}

std::string ShowDesktopModule::tooltip_text() const
{
    if (backend_.supports_xkill() || backend_.supports_native_window_kill()) {
        return "Show desktop\nLeft-click: toggle desktop\nRight-click: click a window to close it\nEscape: cancel";
    }
    return backend_.supports_confirmed_window_close()
        ? "Show desktop\nLeft-click: toggle desktop\nRight-click: close active window"
        : "Show desktop\nLeft-click: toggle desktop";
}
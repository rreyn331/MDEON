#pragma once

#include "panel/modules/system_commands.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace PowerActions {

inline std::string environment_value(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr ? value : "";
}

inline std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

inline std::vector<std::vector<std::string>> commands_for_action(const std::string& action)
{
    const std::string session_type = lower(environment_value("XDG_SESSION_TYPE"));
    const std::string desktop = lower(environment_value("XDG_CURRENT_DESKTOP") + " " +
        environment_value("DESKTOP_SESSION"));
    const std::string session_id = environment_value("XDG_SESSION_ID");
    std::vector<std::vector<std::string>> candidates;

    if (action == "lock") {
        if (session_type == "wayland") {
            candidates.insert(candidates.end(), {{"swaylock"}, {"loginctl", "lock-session"}});
        }
        if (session_type == "x11") {
            candidates.insert(candidates.end(), {{"xsecurelock"}, {"light-locker-command", "-l"},
                {"xdg-screensaver", "lock"}, {"loginctl", "lock-session"}});
        }
        candidates.insert(candidates.end(), {{"loginctl", "lock-session"},
            {"light-locker-command", "-l"}, {"xdg-screensaver", "lock"},
            {"swaylock"}, {"i3lock", "-c", "000000"}, {"xlock"}});
    } else if (action == "logout") {
        if (desktop.find("mdeon") != std::string::npos) {
            candidates.push_back({"mdeon-session", "--logout"});
        }
        if (desktop.find("gnome") != std::string::npos) {
            candidates.push_back({"gnome-session-quit", "--logout", "--no-prompt", "--force"});
        } else if (desktop.find("kde") != std::string::npos || desktop.find("plasma") != std::string::npos) {
            candidates.insert(candidates.end(), {{"qdbus-qt6", "org.kde.Shutdown", "/Shutdown",
                "org.kde.Shutdown.logout"}, {"qdbus", "org.kde.ksmserver", "/KSMServer", "logout", "0", "0", "0"}});
        } else if (desktop.find("xfce") != std::string::npos) {
            candidates.push_back({"xfce4-session-logout", "--logout", "--fast"});
        } else if (desktop.find("lxqt") != std::string::npos) {
            candidates.push_back({"lxqt-leave", "--logout"});
        } else if (desktop.find("cinnamon") != std::string::npos) {
            candidates.push_back({"cinnamon-session-quit", "--logout", "--force"});
        } else if (desktop.find("mate") != std::string::npos) {
            candidates.push_back({"mate-session-save", "--logout", "--force"});
        }

        if (session_type == "wayland") {
            candidates.insert(candidates.end(), {{"swaymsg", "exit"},
                {"pkill", "-QUIT", "-x", "wayfire"}, {"pkill", "-TERM", "-x", "labwc"}});
        }
        if (session_type == "x11" || !environment_value("DISPLAY").empty()) {
            candidates.insert(candidates.end(), {{"openbox", "--exit"}, {"i3-msg", "exit"},
                {"pkill", "-TERM", "-x", "xfwm4"}});
        }
        if (!session_id.empty()) {
            candidates.push_back({"loginctl", "terminate-session", session_id});
        }
    } else if (action == "suspend") {
        candidates.insert(candidates.end(), {{"systemctl", "suspend-then-hibernate"},
            {"systemctl", "hybrid-sleep"}, {"systemctl", "suspend"}, {"loginctl", "hybrid-sleep"},
            {"loginctl", "suspend"}, {"pkexec", "systemctl", "suspend"},
            {"pkexec", "pm-suspend-hybrid"}, {"pkexec", "pm-suspend"}, {"pkexec", "zzz"},
            {"doas", "pm-suspend-hybrid"}, {"sudo", "pm-suspend-hybrid"},
            {"doas", "pm-suspend"}, {"sudo", "pm-suspend"}, {"doas", "zzz"},
            {"sudo", "zzz"}, {"pkexec", "sh", "-c", "echo mem > /sys/power/state"},
            {"doas", "sh", "-c", "echo mem > /sys/power/state"},
            {"sudo", "sh", "-c", "echo mem > /sys/power/state"}});
    } else if (action == "hibernate" || action == "hibernation") {
        candidates.insert(candidates.end(), {{"systemctl", "hibernate"}, {"loginctl", "hibernate"},
            {"pkexec", "systemctl", "hibernate"}, {"pkexec", "pm-hibernate"},
            {"doas", "pm-hibernate"}, {"sudo", "pm-hibernate"},
            {"pkexec", "sh", "-c", "echo disk > /sys/power/state"},
            {"doas", "sh", "-c", "echo disk > /sys/power/state"},
            {"sudo", "sh", "-c", "echo disk > /sys/power/state"}});
    } else if (action == "reboot") {
        candidates.insert(candidates.end(), {{"systemctl", "reboot"}, {"loginctl", "reboot"},
            {"openrc-shutdown", "-r", "now"}, {"reboot"}, {"pkexec", "systemctl", "reboot"},
            {"pkexec", "reboot"}, {"pkexec", "telinit", "6"}, {"pkexec", "init", "6"},
            {"doas", "reboot"}, {"sudo", "reboot"}, {"doas", "telinit", "6"},
            {"sudo", "telinit", "6"}, {"doas", "init", "6"}, {"sudo", "init", "6"}});
    } else if (action == "shutdown") {
        candidates.insert(candidates.end(), {{"systemctl", "poweroff"}, {"loginctl", "poweroff"},
            {"openrc-shutdown", "-p", "now"}, {"poweroff"}, {"pkexec", "systemctl", "poweroff"},
            {"pkexec", "poweroff"}, {"pkexec", "telinit", "0"}, {"pkexec", "init", "0"},
            {"doas", "poweroff"}, {"sudo", "poweroff"}, {"doas", "telinit", "0"},
            {"sudo", "telinit", "0"}, {"doas", "init", "0"}, {"sudo", "init", "0"}});
    }
    return candidates;
}

inline bool execute(const std::string& action)
{
    const int timeout_seconds = action == "hibernate" || action == "hibernation" ? 60 :
        action == "lock" || action == "logout" ? 5 : 30;
    return SystemCommands::launch_fallback_chain(commands_for_action(action), timeout_seconds);
}

}
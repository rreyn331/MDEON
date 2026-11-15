#include "panel/modules/network.hpp"

#include "panel/icon_theme.hpp"
#include "panel/modules/system_commands.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

bool NetworkModule::update()
{
    const auto now = std::chrono::steady_clock::now();
    if (now < next_refresh_) {
        return false;
    }
    next_refresh_ = now + std::chrono::seconds(5);
    const Connection previous_connection = connection_;
    const std::string previous_device = device_;
    const bool previous_wifi_enabled = wifi_enabled_;
    connection_ = Connection::Offline;
    device_.clear();

    const auto devices = SystemCommands::run(
        {"nmcli", "-t", "-f", "DEVICE,TYPE,STATE", "device", "status"});
    if (devices.exit_code == 0) {
        std::istringstream lines(devices.output);
        std::string line;
        while (std::getline(lines, line)) {
            std::istringstream fields(line);
            std::string device;
            std::string type;
            std::string state;
            if (!std::getline(fields, device, ':') || !std::getline(fields, type, ':') ||
                !std::getline(fields, state, ':') || state.rfind("connected", 0) != 0) {
                continue;
            }
            if (type == "wifi" || type == "wireless" || type == "802-11-wireless") {
                connection_ = Connection::Wireless;
            } else if (type == "ethernet" || type == "802-3-ethernet") {
                connection_ = Connection::Wired;
            } else {
                continue;
            }
            device_ = device;
            break;
        }

        const auto radio = SystemCommands::run({"nmcli", "radio", "wifi"});
        if (radio.exit_code == 0) {
            wifi_enabled_ = radio.output.find("enabled") != std::string::npos;
        }
        return connection_ != previous_connection || device_ != previous_device ||
            wifi_enabled_ != previous_wifi_enabled;
    }

    std::ifstream routes("/proc/net/route");
    std::string line;
    std::getline(routes, line);
    while (std::getline(routes, line)) {
        std::istringstream fields(line);
        std::string device;
        std::string destination;
        fields >> device >> destination;
        if (destination != "00000000") {
            continue;
        }
        device_ = device;
        connection_ = std::filesystem::exists("/sys/class/net/" + device + "/wireless")
            ? Connection::Wireless : Connection::Wired;
        break;
    }
    return connection_ != previous_connection || device_ != previous_device ||
        wifi_enabled_ != previous_wifi_enabled;
}

int NetworkModule::measure_width(cairo_t*, int)
{
    return 30;
}

void NetworkModule::render(cairo_t* cr, const Geometry& geometry)
{
    const char* icon_name = "dialog-warning-symbolic";
    if (connection_ == Connection::Wired) {
        icon_name = "network-wired-symbolic";
    } else if (connection_ == Connection::Wireless) {
        icon_name = "network-wireless-symbolic";
    } else if (connection_ == Connection::Offline && !wifi_enabled_) {
        icon_name = "network-wireless-disabled-symbolic";
    } else if (connection_ == Connection::Offline) {
        icon_name = "network-offline-symbolic";
    }
    paint_theme_icon(cr, icon_name, geometry.x + geometry.width / 2.0,
                     geometry.y + geometry.height / 2.0, 28);
}

void NetworkModule::on_click(int button, int, int)
{
    if (button == 1) {
        SystemCommands::launch_first({
            {"nm-connection-editor"},
            {"gnome-control-center", "wifi"},
            {"plasma-nm"},
            {"systemsettings", "kcm_networkmanagement"},
            {"connman-gtk"}
        });
    } else if (button == 3) {
        toggle_wifi();
    }
}

std::string NetworkModule::tooltip_text() const
{
    std::string status;
    switch (connection_) {
    case Connection::Wireless:
        status = "Wi-Fi connected";
        break;
    case Connection::Wired:
        status = "Ethernet connected";
        break;
    case Connection::Offline:
        status = wifi_enabled_ ? "Network disconnected" : "Wi-Fi off";
        break;
    case Connection::Unknown:
        status = "Network status unavailable";
        break;
    }
    if (!device_.empty()) {
        status += " (" + device_ + ")";
    }
    return status + "\nLeft-click: network controls\nRight-click: toggle Wi-Fi";
}

void NetworkModule::toggle_wifi()
{
    const auto state = SystemCommands::run({"nmcli", "radio", "wifi"});
    if (state.exit_code != 0) {
        return;
    }
    const bool enabled = state.output.find("enabled") != std::string::npos;
    const auto result = SystemCommands::run(
        {"nmcli", "radio", "wifi", enabled ? "off" : "on"});
    if (result.exit_code == 0) {
        wifi_enabled_ = !enabled;
        connection_ = Connection::Offline;
        next_refresh_ = {};
    }
}
#include "panel/modules/bluetooth.hpp"

#include "panel/icon_theme.hpp"
#include "panel/modules/system_commands.hpp"

#include <chrono>
#include <sstream>

bool BluetoothModule::update()
{
    const auto now = std::chrono::steady_clock::now();
    if (now < next_refresh_) {
        return false;
    }
    next_refresh_ = now + std::chrono::seconds(8);
    const bool previous_utility = utility_available_;
    const bool previous_controller = controller_available_;
    const bool previous_powered = powered_;
    const std::string previous_devices = connected_devices_;
    utility_available_ = !SystemCommands::find_executable("bluetoothctl").empty();
    controller_available_ = false;
    powered_ = false;
    connected_devices_.clear();
    if (!utility_available_) {
        return previous_utility != utility_available_ ||
            previous_controller != controller_available_ || previous_powered != powered_ ||
            previous_devices != connected_devices_;
    }

    const auto controller = SystemCommands::run({"bluetoothctl", "show"});
    if (controller.exit_code != 0) {
        return previous_utility != utility_available_ ||
            previous_controller != controller_available_ || previous_powered != powered_ ||
            previous_devices != connected_devices_;
    }

    std::istringstream controller_lines(controller.output);
    std::string line;
    while (std::getline(controller_lines, line)) {
        const auto field = line.find("Powered:");
        if (field != std::string::npos) {
            powered_ = line.find("yes", field) != std::string::npos;
            controller_available_ = true;
            break;
        }
    }
    if (!controller_available_ || !powered_) {
        return previous_utility != utility_available_ ||
            previous_controller != controller_available_ || previous_powered != powered_ ||
            previous_devices != connected_devices_;
    }

    const auto devices = SystemCommands::run({"bluetoothctl", "devices", "Connected"});
    std::istringstream device_lines(devices.output);
    while (std::getline(device_lines, line)) {
        std::istringstream fields(line);
        std::string prefix;
        std::string address;
        std::string name;
        if (!(fields >> prefix >> address) || prefix != "Device") {
            continue;
        }
        std::getline(fields >> std::ws, name);
        if (!name.empty()) {
            connected_devices_ += (connected_devices_.empty() ? "" : ", ") + name;
        }
    }
    return previous_utility != utility_available_ ||
        previous_controller != controller_available_ || previous_powered != powered_ ||
        previous_devices != connected_devices_;
}

int BluetoothModule::measure_width(cairo_t*, int)
{
    return utility_available_ ? 30 : 0;
}

void BluetoothModule::render(cairo_t* cr, const Geometry& geometry)
{
    if (!utility_available_) {
        return;
    }
    const char* icon_name = "dialog-warning-symbolic";
    if (controller_available_ && !powered_) {
        icon_name = "bluetooth-disabled-symbolic";
    } else if (controller_available_ && !connected_devices_.empty()) {
        icon_name = "bluetooth-active-symbolic";
    } else if (controller_available_) {
        icon_name = "bluetooth-symbolic";
    }
    paint_theme_icon(cr, icon_name, geometry.x + geometry.width / 2.0,
                     geometry.y + geometry.height / 2.0, 28);
}

void BluetoothModule::on_click(int button, int, int)
{
    if (button == 1) {
        SystemCommands::launch_first({
            {"blueman-manager"},
            {"blueberry"},
            {"gnome-control-center", "bluetooth"},
            {"systemsettings", "kcm_bluetooth"}
        });
    } else if (button == 3) {
        toggle_power();
    }
}

std::string BluetoothModule::tooltip_text() const
{
    if (!utility_available_) {
        return {};
    }
    std::string status;
    if (!controller_available_) {
        status = "Bluetooth controller unavailable";
    } else if (!powered_) {
        status = "Bluetooth off";
    } else if (!connected_devices_.empty()) {
        status = "Connected: " + connected_devices_;
    } else {
        status = "Bluetooth on";
    }
    return status + "\nLeft-click: Bluetooth controls\nRight-click: toggle Bluetooth";
}

void BluetoothModule::toggle_power()
{
    const auto controller = SystemCommands::run({"bluetoothctl", "show"});
    if (controller.exit_code != 0) {
        return;
    }
    std::istringstream lines(controller.output);
    std::string line;
    bool powered = false;
    bool found_power = false;
    while (std::getline(lines, line)) {
        const auto field = line.find("Powered:");
        if (field != std::string::npos) {
            powered = line.find("yes", field) != std::string::npos;
            found_power = true;
            break;
        }
    }
    if (!found_power) {
        return;
    }
    const auto result = SystemCommands::run(
        {"bluetoothctl", "power", powered ? "off" : "on"});
    if (result.exit_code == 0) {
        next_refresh_ = {};
    }
}
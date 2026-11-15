#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

class ConfigEditorModel {
public:
    using Values = std::map<std::string, std::string>;

    std::filesystem::path path;
    Values values;
    std::vector<std::string> launcher_apps;
    std::vector<Values> buttons;
    std::vector<Values> executers;
    std::map<int, Values> backgrounds;
    bool dirty{false};

    ConfigEditorModel()
    {
        const char* override_path = std::getenv("MDEON_PANEL_CONFIG_PATH");
        if (override_path == nullptr || *override_path == '\0') {
            override_path = std::getenv("MDEON_PANEL_CONFIG");
        }
        if (override_path != nullptr && *override_path != '\0') {
            path = override_path;
        } else {
            const char* xdg_home = std::getenv("XDG_CONFIG_HOME");
            const char* home = std::getenv("HOME");
            const auto root = xdg_home != nullptr && *xdg_home != '\0'
                ? std::filesystem::path(xdg_home)
                : std::filesystem::path(home != nullptr ? home : ".") / ".config";
            path = root / "mdeon-panel" / "config.conf";
        }
    }

    static std::string trim(std::string value)
    {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    std::string get(const std::string& key, const std::string& fallback = {}) const
    {
        const auto found = values.find(key);
        return found == values.end() ? fallback : found->second;
    }

    void set(const std::string& key, std::string value)
    {
        if (get(key) != value || values.find(key) == values.end()) {
            values[key] = std::move(value);
            dirty = true;
        }
    }

    std::vector<char> panel_items() const
    {
        const std::string text = get("panel_items", "LTFNVKUBC");
        std::vector<char> result;
        for (const char code : text) {
            if (item_catalog().count(code) != 0) result.push_back(code);
        }
        return result;
    }

    void set_panel_items(const std::vector<char>& items)
    {
        std::string text;
        for (const char item : items) text.push_back(item);
        set("panel_items", std::move(text));
        sync_buttons();
        sync_executers();
    }

    void load()
    {
        ensure_exists();
        values.clear();
        launcher_apps.clear();
        buttons.clear();
        executers.clear();
        backgrounds.clear();
        std::ifstream input(path);
        if (!input) throw std::runtime_error("Could not read " + path.string());

        int current_background = 0;
        Values* current_button = nullptr;
        Values* current_executer = nullptr;
        bool saw_background = false;
        int next_background = 1;
        std::string raw;
        while (std::getline(input, raw)) {
            const std::string line = trim(raw);
            if (line.empty()) continue;
            if (line.front() == '#') {
                std::string comment = trim(line.substr(1));
                std::transform(comment.begin(), comment.end(), comment.begin(),
                    [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (comment.rfind("background ", 0) == 0) {
                    try {
                        current_background = std::stoi(comment.substr(11));
                        backgrounds[current_background];
                        next_background = std::max(next_background, current_background + 1);
                        current_button = nullptr;
                        saw_background = true;
                    } catch (...) {
                        current_background = 0;
                    }
                }
                continue;
            }
            const auto equals = line.find('=');
            if (equals == std::string::npos) continue;
            const std::string key = trim(line.substr(0, equals));
            const std::string value = trim(line.substr(equals + 1));
            if (key == "executer" && value == "new") {
                current_background = 0;
                current_button = nullptr;
                executers.emplace_back();
                current_executer = &executers.back();
                continue;
            }
            if (key == "button" && value == "new") {
                current_background = 0;
                current_executer = nullptr;
                current_button = nullptr;
                buttons.emplace_back();
                current_button = &buttons.back();
                continue;
            }
            if (current_button != nullptr &&
                std::find(button_keys().begin(), button_keys().end(), key) != button_keys().end()) {
                (*current_button)[key] = value;
                continue;
            }
            if (current_executer != nullptr &&
                std::find(executer_keys().begin(), executer_keys().end(), key) != executer_keys().end()) {
                (*current_executer)[key] = value;
                continue;
            }
            if (background_keys().count(key) != 0) {
                if (current_background == 0 && !saw_background) {
                    current_background = next_background++;
                    backgrounds[current_background];
                } else if (current_background != 0 && key == "rounded" &&
                           backgrounds[current_background].count("rounded") != 0) {
                    current_background = next_background++;
                    backgrounds[current_background];
                }
                if (current_background != 0) {
                    backgrounds[current_background][key] = value;
                    continue;
                }
            }
            if (key == "launcher_item_app") {
                launcher_apps.push_back(value);
                continue;
            }
            current_button = nullptr;
            current_executer = nullptr;
            current_background = 0;
            values[key] = value;
        }
        sync_buttons();
        sync_executers();
        dirty = false;
    }

    std::string dump() const
    {
        std::ostringstream output;
        output << "#--------------------------------------------------\n"
               << "# mdeon-panel configuration\n"
               << "# Written by mdeon-panel-config\n"
               << "#--------------------------------------------------\n\n"
               << "panel_items = " << get("panel_items", "LTFNVKUBC") << "\n\n";

        const auto emit = [this, &output](const std::vector<std::string>& keys) {
            for (const auto& key : keys) {
                const auto found = values.find(key);
                if (found != values.end()) output << key << " = " << found->second << '\n';
            }
        };
        output << "# Panel\n";
        emit({"panel_size", "panel_thickness", "panel_color", "panel_opacity",
              "panel_show_in_task_manager", "panel_margin", "panel_padding",
              "panel_spacing", "panel_background_id", "panel_position",
              "panel_monitor", "module_icon_size"});
        output << '\n';

        if (backgrounds.empty()) {
            output << "# Background 1:\n"
                   << "rounded = 0\n"
                   << "border_width = 0\n"
                   << "background_color = #1e1e1e 90\n"
                   << "border_color = #000000 0\n\n";
        }
        for (const auto& [number, props] : backgrounds) {
            output << "# Background " << number << ":\n";
            for (const auto& key : background_keys()) {
                const auto value = props.find(key);
                if (value != props.end() && !trim(value->second).empty()) {
                    output << key << " = " << value->second << '\n';
                }
            }
            output << '\n';
        }

        const std::vector<std::pair<std::string, std::vector<std::string>>> sections = {
            {"Spacer", {"spacer_length", "spacer_bar", "spacer_bar_start",
                "spacer_bar_middle", "spacer_bar_end"}},
            {"Task list", {"task_icon", "task_text", "task_monitor_filter",
                "task_centered", "task_borderless",
                "task_icon_size", "task_maximum_size", "task_padding", "task_font",
                "task_font_color", "task_active_font_color", "task_iconified_font_color",
                "task_background_id", "task_active_background_id", "task_iconified_background_id",
                "mouse_left", "mouse_middle", "mouse_right"}},
            {"Launcher", {"launcher_icon_size", "launcher_padding", "launcher_background_id",
                "launcher_tooltip", "launcher_max_icons"}},
            {"Desktop Hub", {"desktop_hub_icon", "desktop_hub_text", "desktop_hub_tooltip",
                "desktop_hub_folders"}},
            {"Application Menu", {"menu_text", "menu_icon", "menu_tooltip", "menu_width",
                "menu_places_width", "menu_categories_width", "menu_applications_width",
                "menu_height", "menu_show_categories", "menu_show_places", "menu_places",
                "menu_system"}},
            {"Virtual Desktop", {"virtual_desktop_workspaces", "virtual_desktop_display"}},
            {"Clock", {"time1_format", "time2_format", "time1_font", "time2_font",
                "clock_font_color", "clock_padding", "clock_background_id", "clock_tooltip",
                "clock_lclick_command", "clock_rclick_command"}},
            {"Battery", {"battery_hide_if_absent", "bat1_format", "bat2_format",
                "battery_tooltip", "battery_font", "battery_font_color", "battery_padding",
                "battery_background_id"}},
            {"Volume", {"volume_step", "volume_show_percent", "volume_mixer_command"}},
            {"Network", {"network_show_label", "network_settings_command"}},
            {"Bluetooth", {"bluetooth_hide_if_absent", "bluetooth_settings_command"}},
            {"System tray", {"systray_icon_size", "systray_font_color"}},
        };
        std::set<std::string> known = panel_keys();
        for (const auto& key : background_keys()) known.insert(key);
        for (const auto& [title, keys] : sections) {
            bool has_value = false;
            for (const auto& key : keys) has_value = has_value || values.count(key) != 0;
            if (!has_value) continue;
            output << "# " << title << '\n';
            emit(keys);
            output << '\n';
            known.insert(keys.begin(), keys.end());
        }
        if (!launcher_apps.empty()) {
            output << "# Launcher applications\n";
            for (const auto& app : launcher_apps) output << "launcher_item_app = " << app << '\n';
            output << '\n';
        }
        if (!buttons.empty()) {
            output << "# Custom buttons (panel item P)\n";
            for (std::size_t index = 0; index < buttons.size(); ++index) {
                output << "# Button " << index + 1 << '\n' << "button = new\n";
                for (const auto& key : button_keys()) {
                    const auto value = buttons[index].find(key);
                    if (value != buttons[index].end()) output << key << " = " << value->second << '\n';
                }
                output << '\n';
            }
        }
        if (!executers.empty()) {
            output << "# Executor items (panel item E)\n";
            for (std::size_t index = 0; index < executers.size(); ++index) {
                output << "# Executor " << index + 1 << '\n' << "executer = new\n";
                for (const auto& key : executer_keys()) {
                    const auto value = executers[index].find(key);
                    if (value != executers[index].end()) output << key << " = " << value->second << '\n';
                }
                output << '\n';
            }
        }
        for (const auto& key : button_keys()) known.insert(key);
        for (const auto& key : executer_keys()) known.insert(key);
        for (const auto& app : launcher_apps) (void)app;
        std::set<std::string> section_keys;
        for (const auto& [title, keys] : sections) {
            (void)title;
            section_keys.insert(keys.begin(), keys.end());
        }
        known.insert(section_keys.begin(), section_keys.end());
        std::vector<std::string> extra_keys;
        for (const auto& [key, value] : values) {
            (void)value;
            if (known.count(key) == 0) extra_keys.push_back(key);
        }
        if (!extra_keys.empty()) {
            output << "# Other\n";
            emit(extra_keys);
            output << '\n';
        }
        return output.str();
    }

    void save()
    {
        const std::string text = dump();
        std::filesystem::create_directories(path.parent_path());
        std::error_code error;
        if (std::filesystem::exists(path, error)) {
            const auto backup = std::filesystem::path(path.string() + ".bak");
            const auto older = std::filesystem::path(path.string() + ".bak.1");
            if (std::filesystem::exists(backup, error)) {
                std::filesystem::copy_file(backup, older,
                    std::filesystem::copy_options::overwrite_existing, error);
            }
            error.clear();
            std::filesystem::copy_file(path, backup,
                std::filesystem::copy_options::overwrite_existing, error);
        }
        std::ofstream output(path, std::ios::trunc);
        if (!output) throw std::runtime_error("Could not write " + path.string());
        output << text;
        output.close();
        std::filesystem::permissions(path, std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, error);
        dirty = false;
    }

    static const std::map<char, std::pair<std::string, std::string>>& item_catalog()
    {
        static const std::map<char, std::pair<std::string, std::string>> catalog = {
            {'B', {"Battery", "Battery status"}}, {'C', {"Clock", "Clock and date"}},
            {'D', {"Show desktop", "Toggle showing the desktop"}},
            {'E', {"Executor", "Shell command widget"}}, {'F', {"Spacer", "Expandable spacer"}},
            {'H', {"Desktop Hub", "Folders, applications, and system actions"}},
            {'J', {"Search", "Fuzzy search files and folders"}},
            {'K', {"Bluetooth", "Bluetooth status"}}, {'L', {"Launcher", "Application launchers"}},
            {'M', {"Application Menu", "Keyboard-friendly application launcher"}},
            {'N', {"Network", "Network and Wi-Fi"}}, {'P', {"Custom button", "Tint2 custom action"}},
            {'S', {"System tray", "StatusNotifier system tray"}}, {'T', {"Task list", "Open windows"}},
            {'U', {"Notifications", "Notification center"}}, {'V', {"Volume", "Audio controls"}},
            {'W', {"Virtual Desktop", "Switch virtual desktops"}},
        };
        return catalog;
    }

    static const std::set<std::string>& background_keys()
    {
        static const std::set<std::string> keys = {
            "rounded", "border_width", "border_sides", "background_color", "border_color",
            "background_color_hover", "border_color_hover", "background_color_pressed",
            "border_color_pressed", "gradient_id"
        };
        return keys;
    }

    static const std::vector<std::string>& button_keys()
    {
        static const std::vector<std::string> keys = {
            "button_icon", "button_text", "button_tooltip", "button_lclick_command",
            "button_rclick_command", "button_mclick_command", "button_uwheel_command",
            "button_dwheel_command", "button_font_color", "button_padding",
            "button_background_id", "button_centered", "button_max_icon_size", "button_font"
        };
        return keys;
    }

    static const std::vector<std::string>& executer_keys()
    {
        static const std::vector<std::string> keys = {
            "executer_text", "executer_tooltip", "executer_command"
        };
        return keys;
    }

private:
    static const std::set<std::string>& panel_keys()
    {
        static const std::set<std::string> keys = {
            "panel_items", "panel_size", "panel_thickness", "panel_color", "panel_opacity",
            "panel_show_in_task_manager", "panel_margin", "panel_padding", "panel_spacing",
            "panel_background_id", "panel_position", "panel_monitor", "module_icon_size"
        };
        return keys;
    }

    void ensure_exists()
    {
        std::error_code error;
        if (std::filesystem::exists(path, error)) return;
        std::filesystem::create_directories(path.parent_path());
#if defined(MDEON_PANEL_EDITOR_SOURCE_CONFIG) && defined(MDEON_PANEL_EDITOR_FALLBACK_CONFIG)
        std::filesystem::path default_config = MDEON_PANEL_EDITOR_SOURCE_CONFIG;
        if (!std::filesystem::exists(default_config, error)) {
            error.clear();
            default_config = MDEON_PANEL_EDITOR_FALLBACK_CONFIG;
        }
        if (std::filesystem::exists(default_config, error)) {
            std::filesystem::copy_file(default_config, path,
                std::filesystem::copy_options::none, error);
        } else {
            error = std::make_error_code(std::errc::no_such_file_or_directory);
        }
#else
        std::ofstream output(path);
        output << "panel_items = PPPLTSUVNKBC\n";
#endif
        if (error) throw std::runtime_error("Could not create " + path.string());
        std::filesystem::permissions(path, std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, error);
    }

    void sync_buttons()
    {
        const auto items = panel_items();
        const auto count = static_cast<std::size_t>(std::count(items.begin(), items.end(), 'P'));
        while (buttons.size() < count) {
            const std::size_t number = buttons.size() + 1;
            buttons.push_back({{"button_text", "Btn" + std::to_string(number)},
                {"button_tooltip", "Custom button"}, {"button_lclick_command", ""},
                {"button_max_icon_size", "0"}});
        }
        if (buttons.size() > count) {
            values["panel_items"] += std::string(buttons.size() - count, 'P');
            dirty = true;
        }
    }

    void sync_executers()
    {
        const auto items = panel_items();
        const auto count = static_cast<std::size_t>(std::count(items.begin(), items.end(), 'E'));
        Values defaults;
        for (const auto& key : executer_keys()) {
            const auto value = values.find(key);
            if (value != values.end()) defaults.emplace(key, value->second);
        }
        while (executers.size() < count) executers.push_back(defaults);
        if (executers.size() > count) {
            values["panel_items"] += std::string(executers.size() - count, 'E');
            dirty = true;
        }
        for (auto& executer : executers) {
            for (const auto& [key, value] : defaults) executer.try_emplace(key, value);
        }
    }
};
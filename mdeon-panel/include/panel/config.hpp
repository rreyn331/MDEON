#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct PanelConfiguration {
    std::map<std::string, std::string> values;
    std::vector<std::map<std::string, std::string>> buttons;
    std::vector<std::map<std::string, std::string>> executers;
    std::vector<std::string> launcher_apps;
};

inline std::string config_trim(std::string value)
{
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

inline PanelConfiguration load_panel_configuration()
{
    const char* override_path = std::getenv("MDEON_PANEL_CONFIG_PATH");
    if (override_path == nullptr || *override_path == '\0') {
        override_path = std::getenv("MDEON_PANEL_CONFIG");
    }
    if (override_path == nullptr || *override_path == '\0') {
        override_path = std::getenv("SIMPANEL_CONFIG_PATH");
    }
    std::filesystem::path user_config;
    if (override_path != nullptr && *override_path != '\0') {
        user_config = override_path;
    } else {
        const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME");
        const char* home = std::getenv("HOME");
        const std::filesystem::path config_root = xdg_config_home != nullptr && *xdg_config_home != '\0'
            ? xdg_config_home
            : (home != nullptr ? std::filesystem::path(home) / ".config" : std::filesystem::path("."));
        user_config = config_root / "mdeon-panel" / "config.conf";
    }

    std::ifstream input(user_config);
    if (!input) {
        input.clear();
#ifdef MDEON_PANEL_SOURCE_CONFIG_PATH
        input.open(MDEON_PANEL_SOURCE_CONFIG_PATH);
#endif
    }
    if (!input) {
        input.clear();
#ifdef MDEON_PANEL_FALLBACK_CONFIG_PATH
        input.open(MDEON_PANEL_FALLBACK_CONFIG_PATH);
#else
        input.open("config-example.conf");
#endif
    }
    PanelConfiguration config;
    std::map<std::string, std::string>* current_button = nullptr;
    std::map<std::string, std::string>* current_executer = nullptr;
    std::string line;
    while (std::getline(input, line)) {
        line = config_trim(std::move(line));
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = config_trim(line.substr(0, equals));
        const std::string value = config_trim(line.substr(equals + 1));
        if (key == "executer" && value == "new") {
            config.executers.emplace_back();
            current_executer = &config.executers.back();
            current_button = nullptr;
        } else if (key == "button" && value == "new") {
            config.buttons.emplace_back();
            current_button = &config.buttons.back();
            current_executer = nullptr;
        } else if (key == "launcher_item_app") {
            config.launcher_apps.push_back(value);
            current_executer = nullptr;
        } else if (current_executer != nullptr && key.rfind("executer_", 0) == 0) {
            current_executer->insert_or_assign(key, value);
        } else if (current_button != nullptr && key.rfind("button_", 0) == 0) {
            current_button->insert_or_assign(key, value);
        } else {
            current_button = nullptr;
            current_executer = nullptr;
            config.values.insert_or_assign(key, value);
        }
    }

    const auto legacy_keys = {"executer_text", "executer_tooltip", "executer_command"};
    std::map<std::string, std::string> defaults;
    for (const auto* key : legacy_keys) {
        const auto value = config.values.find(key);
        if (value != config.values.end()) defaults.insert_or_assign(key, value->second);
    }
    const auto panel_items = config.values.find("panel_items");
    const std::size_t executer_count = panel_items == config.values.end()
        ? 0 : static_cast<std::size_t>(std::count(panel_items->second.begin(), panel_items->second.end(), 'E'));
    while (config.executers.size() < executer_count) config.executers.push_back(defaults);
    for (auto& executer : config.executers) {
        for (const auto& [key, value] : defaults) executer.try_emplace(key, value);
    }
    return config;
}
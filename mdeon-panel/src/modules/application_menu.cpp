#include "panel/modules/application_menu.hpp"

#include "panel/icon_theme.hpp"
#include "panel/modules/system_commands.hpp"
#include "panel/modules/power_actions.hpp"
#include "panel/theme.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <thread>
#include <tuple>

#include <gio/gdesktopappinfo.h>
#include <pango/pangocairo.h>

namespace {

using Category = std::pair<std::string, std::vector<std::string>>;

const std::vector<Category>& category_map()
{
    static const std::vector<Category> categories = {
        {"All", {}},
        {"Accessories", {"utility", "texteditor", "filemanager", "calculator", "clock", "archiving"}},
        {"Graphics", {"graphics", "2dgraphics", "photography", "rastergraphics", "vectorgraphics", "viewer"}},
        {"Internet", {"network", "webbrowser", "email", "chat", "ircclient", "feed", "filetransfer", "remoteaccess"}},
        {"Office", {"office", "calendar", "contactmanagement", "finance", "presentation", "spreadsheet", "wordprocessor"}},
        {"Sound & Video", {"audiovideo", "audio", "video", "audiovideoediting", "player", "recorder"}},
        {"Development", {"development", "ide", "building", "debugger"}},
        {"Education", {"education", "science"}},
        {"Games", {"game"}},
        {"System Tools", {"system", "monitor", "emulator", "terminalemulator", "packagemanager"}},
        {"Other", {}}
    };
    return categories;
}

const std::vector<std::tuple<std::string, std::string, std::string, std::string>>& power_actions()
{
    static const std::vector<std::tuple<std::string, std::string, std::string, std::string>> actions = {
        {"Lock Screen", "lock", "lock screen", ""},
        {"Log Out", "logout", "log out logout exit sign out", "you will be logged out"},
        {"Suspend", "suspend", "suspend sleep", "the system will suspend"},
        {"Hibernate", "hibernate", "hibernate hibernation", "the system will hibernate"},
        {"Restart", "reboot", "restart reboot reset", "the system will restart"},
        {"Power Off", "shutdown", "power off shutdown turn off", "the system will power off"}
    };
    return actions;
}

std::string lower(std::string value)
{
    gchar* folded = g_utf8_casefold(value.c_str(), static_cast<gssize>(value.size()));
    if (folded == nullptr) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    }
    std::string result(folded);
    g_free(folded);
    return result;
}

std::string trim(std::string value)
{
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

int menu_button_font_size(const std::string& value)
{
    try {
        return std::clamp(std::stoi(value), 4, 32);
    } catch (...) {
        return 8;
    }
}

std::vector<std::string> terms(const std::string& query)
{
    std::istringstream stream(lower(trim(query)));
    std::vector<std::string> result;
    std::string term;
    while (stream >> term) {
        result.push_back(std::move(term));
    }
    return result;
}

struct ConfiguredMenuEntry {
    ApplicationMenuDisplayItem display;
    std::string target;
};

std::vector<ConfiguredMenuEntry> configured_menu_entries(const std::string& value)
{
    std::vector<ConfiguredMenuEntry> entries;
    std::istringstream records(value);
    std::string record;
    while (std::getline(records, record, ';')) {
        std::istringstream fields_stream(record);
        std::vector<std::string> fields;
        std::string field;
        while (std::getline(fields_stream, field, '|')) {
            fields.push_back(trim(std::move(field)));
        }
        if (fields.size() < 3 || fields[0].empty()) {
            continue;
        }
        entries.push_back({{fields[0], fields[1]}, fields[2]});
    }
    return entries;
}

int ordered_match_score(const std::string& text, const std::string& query)
{
    if (text.empty() || query.empty()) {
        return 0;
    }
    std::vector<std::size_t> positions;
    std::size_t start = 0;
    for (const char character : query) {
        const std::size_t position = text.find(character, start);
        if (position == std::string::npos) {
            return 0;
        }
        positions.push_back(position);
        start = position + 1;
    }
    const int span = static_cast<int>(positions.back() - positions.front());
    const int gaps = span - (static_cast<int>(query.size()) - 1);
    return 60 + std::max(0, 20 - static_cast<int>(positions.front())) +
        std::max(0, 20 - gaps);
}

}

ApplicationMenuModule::ApplicationMenuModule(
        IBackend& backend, std::map<std::string, std::string> config, bool button_visible,
        bool vertical, std::function<void()> request_panel_redraw)
        : backend_(backend), config_(std::move(config)),
            request_panel_redraw_(std::move(request_panel_redraw)),
            button_visible_(button_visible), vertical_(vertical)
{
    application_load_state_ = std::make_shared<ApplicationLoadState>();
    if (button_visible_) {
        load_applications();
    }
}

ApplicationMenuModule::ApplicationLoadState::~ApplicationLoadState()
{
    for (auto& application : applications) {
        if (application.info != nullptr) {
            g_object_unref(application.info);
        }
    }
}

ApplicationMenuModule::~ApplicationMenuModule()
{
    alive_->store(false);
    if (load_poll_source_id_ != 0) {
        g_source_remove(load_poll_source_id_);
        load_poll_source_id_ = 0;
    }
    for (auto& application : applications_) {
        if (application.info != nullptr) {
            g_object_unref(application.info);
        }
    }
}

bool ApplicationMenuModule::update()
{
    if (confirming_power_ && std::chrono::steady_clock::now() >= next_confirmation_tick_) {
        next_confirmation_tick_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        --confirmation_seconds_;
        if (confirmation_seconds_ <= 0) {
            const std::string action = pending_power_action_;
            clear_power_confirmation();
            if (execute_power_action(action)) {
                backend_.close_application_menu();
            } else {
                search({});
            }
            return true;
        }
        if (!visible_rows_.empty() && visible_rows_[0].kind == RowKind::Confirm) {
            visible_rows_[0].label = "Confirm " + pending_power_label_ + " (" +
                std::to_string(confirmation_seconds_) + ")";
            refresh_display(0, true);
        }
        return true;
    }
    if (!icon_refresh_notified_ && theme_icon_index_ready()) {
        icon_refresh_notified_ = true;
        if (applications_loaded_) {
            resolve_application_icons();
            search(search_text_);
        }
        return true;
    }
    if (applications_loaded_) {
        return false;
    }
    {
        std::lock_guard lock(application_load_state_->mutex);
        if (!application_load_state_->complete) {
            return false;
        }
        applications_.swap(application_load_state_->applications);
    }
    applications_loaded_ = true;
    if (theme_icon_index_ready()) {
        resolve_application_icons();
        icon_refresh_notified_ = true;
    }
    update_visible_categories();
    backend_.set_application_menu_categories(visible_categories_);
    search(search_text_);
    return true;
}

int ApplicationMenuModule::measure_width(cairo_t* cr, int panel_height)
{
    if (!button_visible_) {
        return 0;
    }
    const std::string text = setting("menu_text", "Menu");
    const int font_size = menu_button_font_size(setting("menu_font_size", "8"));
    int text_width = 0;
    if (!text.empty()) {
        PangoLayout* layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, text.c_str(), -1);
        const std::string font_name = "Sans " + std::to_string(font_size);
        PangoFontDescription* font = pango_font_description_from_string(font_name.c_str());
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        pango_layout_get_pixel_size(layout, &text_width, nullptr);
        g_object_unref(layout);
    }
    const int icon_size = std::min(22, std::max(12, panel_height - 6));
    if (vertical_) return panel_height;
    return icon_size + (text.empty() ? 0 : text_width + 6) + 12;
}

void ApplicationMenuModule::render(cairo_t* cr, const Geometry& geometry)
{
    geometry_ = geometry;
    if (!button_visible_) {
        return;
    }
    const std::string configured_icon = setting(
        "menu_icon", "/usr/share/icons/hicolor/scalable/apps/MDEON.svg");
    const std::string resolved_icon = resolve_theme_icon_name(configured_icon);
    const std::string icon = resolved_icon.empty()
        ? "view-app-grid-symbolic" : resolved_icon;
    const std::string text = setting("menu_text", "Menu");
    const int font_size = menu_button_font_size(setting("menu_font_size", "8"));
    const int icon_size = std::min(22, std::max(12,
        vertical_ ? geometry.width - 6 : geometry.height - 6));
    PangoLayout* layout = nullptr;
    int text_width = 0;
    int text_height = 0;
    if (!text.empty()) {
        layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, text.c_str(), -1);
        const std::string font_name = "Sans " + std::to_string(font_size);
        PangoFontDescription* font = pango_font_description_from_string(font_name.c_str());
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        pango_layout_get_pixel_size(layout, &text_width, &text_height);
    }
    const bool show_text = !vertical_ && !text.empty();
    const int content_width = icon_size + (show_text ? text_width + 6 : 0);
    double cursor = geometry.x + (geometry.width - content_width) / 2.0;
    const double center_y = geometry.y + geometry.height / 2.0;
    paint_theme_icon(cr, icon, cursor + icon_size / 2.0, center_y, icon_size);
    cursor += icon_size + (show_text ? 6 : 0);
    if (layout != nullptr) {
        if (show_text) {
            cairo_set_source_rgb(cr, PanelTheme::text.red, PanelTheme::text.green,
                                 PanelTheme::text.blue);
            cairo_move_to(cr, cursor, center_y - text_height / 2.0);
            pango_cairo_show_layout(cr, layout);
        }
        g_object_unref(layout);
    }
}

void ApplicationMenuModule::on_click(int button, int, int)
{
    if (button == 3) {
        if (backend_.application_menu_visible()) {
            backend_.close_application_menu();
        }
        backend_.toggle_application_power_menu(vertical_ ? geometry_.y : geometry_.x,
            vertical_ ? geometry_.height : geometry_.width,
            [this](const std::string& action, const std::string& label) {
                request_power_action(action, label);
            });
        return;
    }
    if (button != 1) {
        return;
    }
    if (backend_.application_menu_visible()) {
        backend_.close_application_menu();
        return;
    }
    active_category_ = "All";
    search_text_.clear();
    clear_power_confirmation();
    if (!application_load_started_) {
        load_applications();
    }
    search({});
    open_menu();
}

void ApplicationMenuModule::open_menu()
{
    ApplicationMenuSettings settings;
    const bool has_column_widths = config_.count("menu_places_width") != 0 ||
        config_.count("menu_categories_width") != 0 ||
        config_.count("menu_applications_width") != 0;
    int legacy_width = 700;
    try { legacy_width = std::clamp(std::stoi(setting("menu_width", "700")), 300, 2400); }
    catch (...) {}
    const auto column_width = [this, has_column_widths, legacy_width](
        const std::string& key, int fallback, double legacy_fraction,
        int minimum, int maximum) {
        const auto configured = config_.find(key);
        if (configured != config_.end()) {
            try { return std::clamp(std::stoi(configured->second), minimum, maximum); }
            catch (...) {}
        }
        if (!has_column_widths) {
            return std::clamp(static_cast<int>(std::lround(legacy_width * legacy_fraction)),
                              minimum, maximum);
        }
        return fallback;
    };
    settings.places_width = column_width("menu_places_width", 138, 0.197, 100, 400);
    settings.categories_width = column_width("menu_categories_width", 128, 0.183, 100, 400);
    settings.applications_width = column_width("menu_applications_width", 434, 0.620, 200, 800);
    try {
        settings.height = std::clamp(std::stoi(setting("menu_height", "500")), 200, 1200);
    } catch (...) {
        settings.height = 500;
    }
    const auto enabled = [this](const std::string& key) {
        const std::string value = lower(setting(key, "1"));
        return value == "1" || value == "true" || value == "yes" || value == "on";
    };
    settings.show_categories = enabled("menu_show_categories");
    settings.show_places = enabled("menu_show_places");
    settings.width = settings.applications_width +
        (settings.show_categories ? settings.categories_width : 0) +
        (settings.show_places ? settings.places_width : 0);
    place_targets_.clear();
    system_commands_.clear();
    for (auto& entry : configured_menu_entries(setting("menu_places",
        "Computer|drive-harddisk-symbolic|file:///;Home Folder|user-home-symbolic|home;"
        "Network|network-workgroup-symbolic|network:///;Desktop|user-desktop-symbolic|desktop;"
        "Trash|user-trash-symbolic|trash:///"))) {
        settings.places.push_back(std::move(entry.display));
        place_targets_.push_back(std::move(entry.target));
    }
    for (auto& entry : configured_menu_entries(setting("menu_system",
        "Control Center|preferences-system-symbolic|mate-control-center,cinnamon-settings,"
        "gnome-control-center,xfce4-settings-manager,lxqt-config;Terminal|utilities-terminal-symbolic|"
        "kgx,mate-terminal,gnome-terminal,xfce4-terminal,konsole,kitty,alacritty,xterm"))) {
        settings.system.push_back(std::move(entry.display));
        system_commands_.push_back(std::move(entry.target));
    }
    backend_.open_application_menu(
        vertical_ ? geometry_.y : geometry_.x,
        vertical_ ? geometry_.height : geometry_.width,
        std::move(settings), visible_categories_, display_items(),
        [this](const std::string& category) { select_category(category); },
        [this](const std::string& query) { search(query); },
        [this](std::size_t index) { activate(index); },
        [this](bool system, std::size_t index) { activate_shortcut(system, index); },
        [this](const std::string& action, const std::string& label) {
            request_power_action(action, label);
        },
        [this] { clear_power_confirmation(); });
}

void ApplicationMenuModule::activate_shortcut(bool system, std::size_t index)
{
    if (system) {
        if (index >= system_commands_.size()) return;
        std::istringstream candidates(system_commands_[index]);
        std::string candidate;
        while (std::getline(candidates, candidate, ',')) {
            candidate = trim(std::move(candidate));
            if (candidate.empty()) continue;
            if (candidate.ends_with(".desktop") &&
                g_file_test(candidate.c_str(), G_FILE_TEST_IS_REGULAR)) {
                const bool launched = SystemCommands::launch_first(
                    {{"gio", "launch", candidate}});
                if (launched) {
                    backend_.close_application_menu();
                    return;
                }
                continue;
            }
            const bool launched = SystemCommands::launch_first({{candidate}});
            if (launched) {
                backend_.close_application_menu();
                return;
            }
        }
        return;
    }
    if (index >= place_targets_.size()) return;
    std::string target = place_targets_[index];
    if (target == "home") {
        gchar* uri = g_filename_to_uri(g_get_home_dir(), nullptr, nullptr);
        target = uri != nullptr ? uri : "";
        g_free(uri);
    } else if (target == "desktop") {
        const char* desktop = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
        gchar* uri = g_filename_to_uri(desktop != nullptr ? desktop : g_get_home_dir(),
                                       nullptr, nullptr);
        target = uri != nullptr ? uri : "";
        g_free(uri);
    } else if (!target.empty() && target.front() == '/') {
        gchar* uri = g_filename_to_uri(target.c_str(), nullptr, nullptr);
        if (uri != nullptr) {
            target = uri;
            g_free(uri);
        }
    }
    if (!target.empty() && SystemCommands::launch_first({{"xdg-open", target}})) {
        backend_.close_application_menu();
    }
}

void ApplicationMenuModule::open_menu_from_command()
{
    if (!application_load_started_) {
        load_applications();
    }
    active_category_ = "All";
    search_text_.clear();
    clear_power_confirmation();
    search({});
    open_menu();
}

std::string ApplicationMenuModule::tooltip_text() const
{
    return setting("menu_tooltip", "Application Menu");
}

void ApplicationMenuModule::load_applications()
{
    application_load_started_ = true;
    const auto state = application_load_state_;
    struct PollRequest {
        ApplicationMenuModule* module;
        std::weak_ptr<std::atomic<bool>> alive;
    };
    auto* request = new PollRequest{this, alive_};
    load_poll_source_id_ = g_timeout_add_full(G_PRIORITY_DEFAULT, 40,
        [](gpointer data) -> gboolean {
            auto* request = static_cast<PollRequest*>(data);
            const auto alive = request->alive.lock();
            if (alive == nullptr || !alive->load()) {
                return G_SOURCE_REMOVE;
            }
            const bool changed = request->module->update();
            if (changed && request->module->icon_refresh_notified_ &&
                request->module->request_panel_redraw_) {
                request->module->request_panel_redraw_();
            }
            if (request->module->applications_loaded_ &&
                request->module->icon_refresh_notified_) {
                request->module->load_poll_source_id_ = 0;
                return G_SOURCE_REMOVE;
            }
            return G_SOURCE_CONTINUE;
        }, request, [](gpointer data) { delete static_cast<PollRequest*>(data); });
    std::thread([state] {
        auto applications = discover_applications();
        {
            std::lock_guard lock(state->mutex);
            state->applications = std::move(applications);
            state->complete = true;
        }
        warm_theme_icon_index();
    }).detach();
}

std::vector<ApplicationMenuModule::Application>
ApplicationMenuModule::discover_applications()
{
    std::vector<Application> applications;
    std::set<std::string> seen;
    GList* apps = g_app_info_get_all();
    for (GList* node = apps; node != nullptr; node = node->next) {
        auto* app = G_APP_INFO(node->data);
        if (!g_app_info_should_show(app)) {
            continue;
        }
        const char* name = g_app_info_get_display_name(app);
        if (name == nullptr || *name == '\0') {
            continue;
        }
        const char* id = g_app_info_get_id(app);
        const std::string identity = id != nullptr ? id : name;
        if (!seen.insert(identity).second) {
            continue;
        }

        Application application;
        application.info = G_APP_INFO(g_object_ref(app));
        application.name = name;
        const char* description = g_app_info_get_description(app);
        if (description != nullptr) {
            application.description = lower(description);
        }
        const char* executable = g_app_info_get_executable(app);
        if (executable != nullptr) {
            application.executable = lower(executable);
        }
        if (G_IS_DESKTOP_APP_INFO(app)) {
            const char* categories = g_desktop_app_info_get_categories(G_DESKTOP_APP_INFO(app));
            if (categories != nullptr) {
                std::istringstream category_stream(categories);
                std::string category;
                while (std::getline(category_stream, category, ';')) {
                    if (!category.empty()) {
                        application.categories.insert(lower(category));
                    }
                }
            }
        }
        GIcon* icon = g_app_info_get_icon(app);
        if (icon != nullptr && G_IS_THEMED_ICON(icon)) {
            const gchar* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
            for (const gchar* const* name = names; name != nullptr && *name != nullptr; ++name) {
                application.icon = *name;
                break;
            }
        } else if (icon != nullptr && G_IS_FILE_ICON(icon)) {
            GFile* file = g_file_icon_get_file(G_FILE_ICON(icon));
            gchar* path = g_file_get_path(file);
            if (path != nullptr) {
                application.icon = path;
                g_free(path);
            }
        }
        if (application.icon.empty()) {
            application.icon = "application-x-executable-symbolic";
        }
        applications.push_back(std::move(application));
    }
    g_list_free_full(apps, g_object_unref);
    std::sort(applications.begin(), applications.end(), [](const Application& left,
                                                            const Application& right) {
        return lower(left.name) < lower(right.name);
    });
    return applications;
}

void ApplicationMenuModule::resolve_application_icons()
{
    for (auto& application : applications_) {
        const std::string resolved = resolve_theme_icon_name(application.icon);
        if (!resolved.empty()) {
            application.icon = resolved;
        }
    }
}

void ApplicationMenuModule::update_visible_categories()
{
    visible_categories_.clear();
    for (const auto& [category, tags] : category_map()) {
        if (category == "All" ||
            std::any_of(applications_.begin(), applications_.end(), [this, &category](const auto& app) {
                return matches_category(app, category);
            })) {
            visible_categories_.push_back(category);
        }
    }
}

bool ApplicationMenuModule::matches_category(const Application& application,
                                             const std::string& category) const
{
    if (category == "All") {
        return true;
    }
    if (category == "Other") {
        return std::none_of(category_map().begin() + 1, category_map().end() - 1,
            [this, &application](const auto& entry) {
                return matches_category(application, entry.first);
            });
    }
    const auto found = std::find_if(category_map().begin(), category_map().end(),
        [&category](const auto& entry) { return entry.first == category; });
    if (found == category_map().end()) {
        return false;
    }
    return std::any_of(found->second.begin(), found->second.end(), [&application](const auto& tag) {
        return application.categories.count(tag) != 0;
    });
}

void ApplicationMenuModule::select_category(const std::string& category)
{
    active_category_ = category;
    search_text_.clear();
    clear_power_confirmation();
    search(search_text_);
}

void ApplicationMenuModule::search(const std::string& query)
{
    search_text_ = query;
    if (!trim(query).empty() && active_category_ != "All") {
        active_category_ = "All";
        backend_.set_application_menu_category("All");
    }
    if (!trim(query).empty()) {
        clear_power_confirmation();
    }
    visible_rows_.clear();
    if (!applications_loaded_ && !confirming_power_) {
        visible_rows_.push_back({"Loading applications...", "content-loading-symbolic",
            RowKind::Status, 0, {}});
    } else if (confirming_power_) {
        visible_rows_.push_back({"Confirm " + pending_power_label_ + " (" +
            std::to_string(confirmation_seconds_) + ")",
            "dialog-warning-symbolic", RowKind::Confirm, 0, pending_power_action_});
        visible_rows_.push_back({"Cancel", "process-stop-symbolic", RowKind::Cancel, 0, {}});
    } else if (trim(query).empty()) {
        for (std::size_t index = 0; index < applications_.size(); ++index) {
            if (matches_category(applications_[index], active_category_)) {
                visible_rows_.push_back({applications_[index].name, applications_[index].icon,
                    RowKind::Application, index, {}});
            }
        }
    } else {
        struct ScoredRow {
            int score;
            std::string label;
            Row row;
        };
        std::vector<ScoredRow> scored;
        const auto query_terms = terms(query);
        for (std::size_t index = 0; index < applications_.size(); ++index) {
            const auto& app = applications_[index];
            if (!matches_category(app, active_category_)) {
                continue;
            }
            const std::string name = lower(app.name);
            std::string category_text;
            for (const auto& category : app.categories) {
                category_text += category + ";";
            }
            const std::string searchable = name + " " + app.description + " " +
                app.executable + " " + category_text;
            int score = 0;
            for (const auto& term : query_terms) {
                if (name == term) score += 120;
                else if (name.rfind(term, 0) == 0) score += 80;
                else if (name.find(term) != std::string::npos) score += 50;
                else score += ordered_match_score(name, term);
                if (app.description.find(term) != std::string::npos) score += 12;
                if (app.executable.find(term) != std::string::npos) score += 10;
                if (category_text.find(term) != std::string::npos) score += 8;
                if (searchable.find(term) != std::string::npos) score += 5;
            }
            if (score > 0) {
                scored.push_back({score, name, {app.name, app.icon, RowKind::Application, index, {}}});
            }
        }
        for (const auto& [label, action, aliases, warning] : power_actions()) {
            const std::string lowered_label = lower(label);
            const std::string searchable = "power " + lowered_label + " " + action + " " +
                aliases + " " + warning;
            if (!std::all_of(query_terms.begin(), query_terms.end(), [&searchable](const auto& term) {
                    return searchable.find(term) != std::string::npos;
                })) {
                continue;
            }
            int score = 0;
            for (const auto& term : query_terms) {
                if (searchable.find(term) == std::string::npos) {
                    continue;
                }
                if (lowered_label == term) score += 120;
                else if (lowered_label.rfind(term, 0) == 0) score += 80;
                else score += 50;
                if (searchable.find(term) != std::string::npos) score += 5;
            }
            if (score > 0) {
                scored.push_back({score, lowered_label,
                    {label, "system-shutdown-symbolic", RowKind::Power, 0, action}});
            }
        }
        std::sort(scored.begin(), scored.end(), [](const ScoredRow& left, const ScoredRow& right) {
            if (left.score != right.score) {
                return left.score > right.score;
            }
            return left.label < right.label;
        });
        for (auto& result : scored) {
            visible_rows_.push_back(std::move(result.row));
        }
    }
    refresh_display();
}

void ApplicationMenuModule::refresh_display(std::size_t selected, bool preserve_selection)
{
    backend_.update_application_menu(search_text_, display_items(), selected, preserve_selection);
}

void ApplicationMenuModule::activate(std::size_t index)
{
    if (index >= visible_rows_.size()) {
        return;
    }
    const Row& row = visible_rows_[index];
    if (row.kind == RowKind::Application) {
        if (row.application_index < applications_.size()) {
            GAppInfo* app = applications_[row.application_index].info;
            const char* desktop_file = G_IS_DESKTOP_APP_INFO(app)
                ? g_desktop_app_info_get_filename(G_DESKTOP_APP_INFO(app)) : nullptr;
            const bool launched = desktop_file != nullptr &&
                SystemCommands::launch_first({{"gio", "launch", desktop_file}});
            if (launched) {
                backend_.close_application_menu();
            }
        }
        return;
    }
    if (row.kind == RowKind::Power) {
        request_power_action(row.action, row.label);
        return;
    }
    if (row.kind == RowKind::Cancel) {
        clear_power_confirmation();
        search({});
        return;
    }
    if (row.kind == RowKind::Confirm) {
        const std::string action = row.action;
        clear_power_confirmation();
        if (execute_power_action(action)) {
            backend_.close_application_menu();
        } else {
            search({});
        }
    }
}

void ApplicationMenuModule::request_power_action(const std::string& action,
                                                const std::string& label)
{
    const bool open_confirmation_menu = !backend_.application_menu_visible();
    confirming_power_ = true;
    backend_.set_application_menu_countdown_active(true);
    pending_power_action_ = action;
    pending_power_label_ = label;
    confirmation_seconds_ = 5;
    next_confirmation_tick_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    search_text_.clear();
    active_category_ = "All";
    search({});
    if (open_confirmation_menu) {
        open_menu();
    }
    backend_.focus_application_menu_items();
}

void ApplicationMenuModule::clear_power_confirmation()
{
    confirming_power_ = false;
    pending_power_action_.clear();
    pending_power_label_.clear();
    confirmation_seconds_ = 0;
    backend_.set_application_menu_countdown_active(false);
}

bool ApplicationMenuModule::execute_power_action(const std::string& action) const
{
    return PowerActions::execute(action);
}

std::vector<ApplicationMenuDisplayItem> ApplicationMenuModule::display_items() const
{
    std::vector<ApplicationMenuDisplayItem> result;
    result.reserve(visible_rows_.size());
    for (const auto& row : visible_rows_) {
        result.push_back({row.label, row.icon});
    }
    return result;
}

std::string ApplicationMenuModule::setting(const std::string& key,
                                           const std::string& fallback) const
{
    const auto found = config_.find(key);
    return found == config_.end() ? fallback : found->second;
}
#include "panel/modules/desktop_hub.hpp"

#include "panel/icon_theme.hpp"
#include "panel/modules/power_actions.hpp"
#include "panel/modules/system_commands.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <glib.h>
#include <sys/stat.h>

#include <gio/gdesktopappinfo.h>

namespace {

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return text;
}

std::vector<std::string> split(const std::string& value, char delimiter)
{
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(delimiter, start);
        result.push_back(value.substr(start,
            end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

std::string expand_home(const std::string& value)
{
    if (value == "~") {
        return g_get_home_dir();
    }
    if (value.rfind("~/", 0) == 0) {
        return std::string(g_get_home_dir()) + value.substr(1);
    }
    return value;
}

std::string trim(std::string value)
{
    const std::size_t first = value.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(" \t\n\r") - first + 1);
}

bool is_real_block_device(const std::string& path)
{
    std::ifstream mounts("/proc/mounts");
    struct stat target_status{};
    if (!mounts || stat(path.c_str(), &target_status) != 0) {
        return false;
    }

    std::string line;
    while (std::getline(mounts, line)) {
        const auto fields = split(line, ' ');
        if (fields.size() < 3) {
            continue;
        }
        struct stat mount_status{};
        if (stat(fields[1].c_str(), &mount_status) != 0 ||
            mount_status.st_dev != target_status.st_dev ||
            mount_status.st_ino != target_status.st_ino) {
            continue;
        }
        static const std::set<std::string> pseudo_filesystems = {
            "nfs", "nfs4", "cifs", "fuse.sshfs", "davfs", "gvfsd-fuse", "fusectl"
        };
        if (pseudo_filesystems.count(fields[2]) != 0) {
            return false;
        }
        const std::string& device = fields[0];
        return device.rfind("/dev/sd", 0) == 0 || device.rfind("/dev/nvme", 0) == 0 ||
            device.rfind("/dev/mmcblk", 0) == 0 || device.rfind("/dev/hd", 0) == 0 ||
            device.rfind("/dev/sr", 0) == 0;
    }
    return false;
}

std::string find_default_terminal()
{
    static const std::vector<std::string> terminals = {
        "x-terminal-emulator", "alacritty", "kitty", "wezterm", "xfce4-terminal",
        "mate-terminal", "lxterminal", "konsole", "xterm", "st", "urxvt"
    };
    for (const auto& terminal : terminals) {
        const std::string executable = SystemCommands::find_executable(terminal);
        if (!executable.empty()) {
            return executable;
        }
    }
    return {};
}

}

DesktopHubModule::DesktopHubModule(IBackend& backend,
                                   std::map<std::string, std::string> config)
    : backend_(backend), config_(std::move(config)),
      icon_name_(setting("desktop_hub_icon", "folder-activities"))
{
    if (icon_name_.empty()) {
        icon_name_ = "folder-activities";
    }
    load_applications();
    load_places();
    visible_items_ = places_;
}

DesktopHubModule::~DesktopHubModule()
{
    alive_->store(false);
    search_generation_->fetch_add(1);
    if (file_search_source_id_ != 0) {
        g_source_remove(file_search_source_id_);
        file_search_source_id_ = 0;
    }
}

bool DesktopHubModule::update()
{
    if (!confirming_power_ || std::chrono::steady_clock::now() < next_confirmation_tick_) {
        return false;
    }
    next_confirmation_tick_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    --confirmation_seconds_;
    if (confirmation_seconds_ <= 0) {
        const std::string action = pending_power_action_;
        clear_power_confirmation();
        if (execute_power_action(action)) {
            backend_.close_desktop_hub();
        } else {
            search(current_query_);
        }
        return true;
    }
    if (!visible_items_.empty()) {
        visible_items_[0].label = "Confirm " + pending_power_label_ + " (" +
            std::to_string(confirmation_seconds_) + ")";
        refresh_display(0, true);
    }
    return true;
}

int DesktopHubModule::measure_width(cairo_t*, int)
{
    return 30;
}

void DesktopHubModule::render(cairo_t* cr, const Geometry& geometry)
{
    geometry_ = geometry;
    paint_theme_icon(cr, icon_name_, geometry.x + geometry.width / 2.0,
                     geometry.y + geometry.height / 2.0, 28);
}

void DesktopHubModule::on_click(int button, int, int)
{
    if (button != 1) {
        return;
    }
    if (backend_.desktop_hub_visible()) {
        backend_.close_desktop_hub();
        return;
    }
    load_places();
    current_query_.clear();
    clear_power_confirmation();
    base_results_.clear();
    file_results_.clear();
    visible_items_ = places_;
    backend_.open_desktop_hub(
        geometry_.vertical ? geometry_.y : geometry_.x,
        geometry_.vertical ? geometry_.height : geometry_.width, icon_name_, display_items(),
        [this](const std::string& query) { search(query); },
        [this](std::size_t index) { activate(index); },
        [this] { clear_power_confirmation(); });
}

std::string DesktopHubModule::tooltip_text() const
{
    return setting("desktop_hub_tooltip", "Desktop Hub");
}

void DesktopHubModule::load_applications()
{
    std::set<std::string> seen;
    GList* apps = g_app_info_get_all();
    for (GList* node = apps; node != nullptr; node = node->next) {
        auto* app = G_APP_INFO(node->data);
        if (!g_app_info_should_show(app)) {
            continue;
        }
        const char* id_value = g_app_info_get_id(app);
        const char* name_value = g_app_info_get_display_name(app);
        if (name_value == nullptr || *name_value == '\0') {
            continue;
        }
        const std::string id = id_value != nullptr ? id_value : name_value;
        if (!seen.insert(id).second) {
            continue;
        }
        std::string target = id_value != nullptr ? id_value : "";
        if (G_IS_DESKTOP_APP_INFO(app)) {
            const char* filename = g_desktop_app_info_get_filename(G_DESKTOP_APP_INFO(app));
            if (filename != nullptr) {
                target = filename;
            }
        }
        if (target.empty()) {
            continue;
        }
        const char* executable = g_app_info_get_executable(app);
        std::string searchable = name_value;
        std::string categories;
        if (executable != nullptr) {
            application_commands_.push_back(lower(executable));
        }
        const char* description = g_app_info_get_description(app);
        if (description != nullptr) {
            searchable += " " + std::string(description);
        }
        if (G_IS_DESKTOP_APP_INFO(app)) {
            const char* raw_categories = g_desktop_app_info_get_categories(
                G_DESKTOP_APP_INFO(app));
            if (raw_categories != nullptr) {
                categories = raw_categories;
                searchable += " " + categories;
            }
        }
        std::string icon;
        GIcon* app_icon = g_app_info_get_icon(app);
        if (app_icon != nullptr && G_IS_THEMED_ICON(app_icon)) {
            const gchar* const* names = g_themed_icon_get_names(G_THEMED_ICON(app_icon));
            if (names != nullptr && names[0] != nullptr) {
                icon = names[0];
            }
        }
        Item item{name_value, target,
            icon.empty() ? "application-x-executable" : icon, Item::Kind::Application};
        item.categories = lower(categories);
        item.searchable = lower(searchable);
        applications_.push_back(std::move(item));
    }
    g_list_free_full(apps, g_object_unref);
    std::sort(applications_.begin(), applications_.end(), [](const Item& left, const Item& right) {
        return lower(left.label) < lower(right.label);
    });
}

void DesktopHubModule::load_places()
{
    places_.clear();
    std::vector<std::pair<std::string, std::string>> folders;
    const std::string configured = setting("desktop_hub_folders");
    if (!configured.empty()) {
        for (const auto& entry : split(configured, ';')) {
            const std::size_t separator = entry.find('|');
            if (separator == std::string::npos) {
                continue;
            }
            const std::string label = trim(entry.substr(0, separator));
            const std::string target = trim(entry.substr(separator + 1));
            if (!label.empty() && !target.empty()) {
                folders.emplace_back(label, target);
            }
        }
    }
    if (folders.empty()) {
        folders = {
            {"Home Folder", "~"}, {"Desktop", "~/Desktop"},
            {"Documents", "~/Documents"}, {"Downloads", "~/Downloads"},
            {"Music", "~/Music"}, {"Pictures", "~/Pictures"},
            {"Videos", "~/Videos"}, {"Network", "network:///"},
            {"Trash", "trash:///"}
        };
    }

    std::set<std::string> seen;
    for (const auto& [label, raw_target] : folders) {
        const std::string target = expand_home(raw_target);
        const bool uri = target.find("://") != std::string::npos;
        std::error_code error;
        if (!uri && !std::filesystem::is_directory(target, error)) {
            continue;
        }
        std::string identity = target;
        if (!uri) {
            const std::filesystem::path canonical = std::filesystem::canonical(target, error);
            if (!error) {
                identity = canonical.string();
            }
            error.clear();
        }
        if (!seen.insert(identity).second) {
            continue;
        }
        const std::string kind = target.rfind("network://", 0) == 0 ? "Network" :
            (target.rfind("trash://", 0) == 0 ? "Trash" : "Folder");
        Item item{"[" + kind + "] " + label + " — " + raw_target, target, "folder-symbolic",
              uri ? Item::Kind::Uri : Item::Kind::Place};
        item.searchable = lower(item.label + " " + target);
        places_.push_back(std::move(item));
    }

    for (const auto* base_directory : {"/media", "/mnt"}) {
        std::error_code error;
        for (std::filesystem::directory_iterator user_directory(base_directory, error), end;
             user_directory != end; user_directory.increment(error)) {
            if (error) {
                error.clear();
                continue;
            }
            const std::string user_name = user_directory->path().filename().string();
            if (user_name.empty() || user_name.front() == '.') {
                continue;
            }
            const std::string user_path = user_directory->path().string();
            std::vector<std::filesystem::path> drives;
            if (is_real_block_device(user_path)) {
                drives.push_back(user_directory->path());
            } else {
                std::error_code child_error;
                for (std::filesystem::directory_iterator child(user_path, child_error), child_end;
                     child != child_end; child.increment(child_error)) {
                    if (child_error) {
                        child_error.clear();
                        continue;
                    }
                    const std::string child_name = child->path().filename().string();
                    if (!child_name.empty() && child_name.front() != '.') {
                        drives.push_back(child->path());
                    }
                }
            }
            for (const auto& drive : drives) {
                std::error_code drive_error;
                if (!std::filesystem::is_directory(drive, drive_error) ||
                    !is_real_block_device(drive.string())) {
                    continue;
                }
                const auto canonical = std::filesystem::canonical(drive, drive_error);
                const std::string identity = drive_error ? drive.string() : canonical.string();
                if (!seen.insert(identity).second) {
                    continue;
                }
                const std::string name = drive.filename().string();
                Item item{"[Drive] " + name + " — " + drive.string(), drive.string(),
                    "drive-harddisk-symbolic", Item::Kind::Place};
                item.searchable = lower("drive " + name);
                places_.push_back(std::move(item));
            }
        }
    }
}

void DesktopHubModule::search(const std::string& raw_query)
{
    current_query_ = raw_query;
    const std::uint64_t generation = search_generation_->fetch_add(1) + 1;
    clear_power_confirmation();
    const std::string query = lower(trim(raw_query));
    base_results_.clear();
    file_results_.clear();
    if (query.size() < 2) {
        base_results_ = places_;
    } else {
        const std::string local_path = [&query]() {
            std::error_code error;
            const std::filesystem::path path(expand_home(query));
            if (!std::filesystem::exists(path, error)) {
                return std::string{};
            }
            const std::filesystem::path canonical = std::filesystem::canonical(path, error);
            return error ? path.string() : canonical.string();
        }();
        if (!local_path.empty()) {
            std::error_code error;
            const bool directory = std::filesystem::is_directory(local_path, error);
            base_results_.push_back({"> Open " + std::string(directory ? "Folder" : "File") +
                ": " + local_path, local_path,
                directory ? "folder-symbolic" : "text-x-generic-symbolic", Item::Kind::Path});
        } else {
            std::istringstream command_stream(query);
            std::string command_name;
            command_stream >> command_name;
            if (!command_name.empty() &&
                std::find(application_commands_.begin(), application_commands_.end(), command_name) ==
                    application_commands_.end() &&
                !SystemCommands::find_executable(command_name).empty()) {
                base_results_.push_back({"> Run in Terminal: " + query, query,
                    "utilities-terminal-symbolic", Item::Kind::Terminal});
            }
        }

        static const std::vector<std::tuple<std::string, std::string, std::string>> power_actions = {
            {"[Power] Shutdown", "shutdown", "power shutdown off turn off"},
            {"[Power] Reboot", "reboot", "power restart reboot reset"},
            {"[Power] Lock Session", "lock", "power lock screen"},
            {"[Power] Logout", "logout", "power logout log out exit sign out"},
            {"[Power] Suspend", "suspend", "power suspend sleep"},
            {"[Power] Hibernation", "hibernation", "power hibernate"}
        };
        for (const auto& [label, action, aliases] : power_actions) {
            if (lower(label).find(query) != std::string::npos ||
                lower(aliases).find(query) != std::string::npos) {
                base_results_.push_back({label, action, "system-shutdown-symbolic", Item::Kind::Power});
            }
        }

        static const std::map<std::string, std::string> category_aliases = {
            {"web", "webbrowser"}, {"browser", "webbrowser"}, {"internet", "network"},
            {"net", "network"}, {"settings", "settings"}, {"development", "development"},
            {"game", "game"}, {"graphics", "graphics"}, {"office", "office"},
            {"sound", "audio"}, {"audio", "audio"}, {"video", "video"},
            {"utility", "utility"}, {"system", "system"}
        };
        const auto category = category_aliases.find(query);
        const std::string target_category = category == category_aliases.end() ? "" : category->second;
        struct ScoredApp {
            int tier;
            std::size_t index;
            std::size_t length;
            const Item* item;
        };
        std::vector<ScoredApp> scored;
        for (const auto& item : applications_) {
            const std::string name = lower(item.label);
            const std::string categories = lower(item.categories);
            const std::string searchable = lower(item.searchable);
            if (!target_category.empty()) {
                if (categories.find(target_category) != std::string::npos ||
                    (target_category == "webbrowser" && searchable.find("webbrowser") != std::string::npos)) {
                    scored.push_back({0, 0, name.size(), &item});
                }
                continue;
            }
            int tier = -1;
            std::size_t position = 0;
            if (name == query) tier = 0;
            else if (name.rfind(query, 0) == 0) tier = 1;
            else if ((position = name.find(query)) != std::string::npos) tier = 2;
            else if (searchable.find(query) != std::string::npos) tier = 3;
            if (tier >= 0) scored.push_back({tier, position, name.size(), &item});
        }
        std::sort(scored.begin(), scored.end(), [](const ScoredApp& left, const ScoredApp& right) {
            if (left.tier != right.tier) return left.tier < right.tier;
            if (left.index != right.index) return left.index < right.index;
            return left.length < right.length;
        });
        for (const auto& result : scored) base_results_.push_back(*result.item);
    }
    refresh_display();
    if (query.size() >= 2) {
        schedule_file_search(raw_query, generation);
    }
}

void DesktopHubModule::activate(std::size_t index)
{
    if (index >= visible_items_.size()) {
        return;
    }
    const Item& item = visible_items_[index];
    if (confirming_power_) {
        if (index == 1) {
            clear_power_confirmation();
            search(current_query_);
        } else if (index == 0) {
            const std::string action = pending_power_action_;
            clear_power_confirmation();
            if (execute_power_action(action)) {
                backend_.close_desktop_hub();
            } else {
                search(current_query_);
            }
        }
        return;
    }
    if (item.kind == Item::Kind::Power) {
        confirming_power_ = true;
        backend_.set_desktop_hub_countdown_active(true);
        pending_power_action_ = item.target;
        const std::size_t label_start = item.label.find("] ");
        pending_power_label_ = label_start == std::string::npos
            ? item.label : item.label.substr(label_start + 2);
        confirmation_seconds_ = 5;
        next_confirmation_tick_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        base_results_ = {
            {"Confirm " + pending_power_label_ + " (5)", "confirm",
             "dialog-warning-symbolic", Item::Kind::Power},
            {"Cancel", "cancel", "process-stop-symbolic", Item::Kind::Place}
        };
        file_results_.clear();
        refresh_display(0);
        return;
    }
    bool launched = false;
    if (item.kind == Item::Kind::Application) {
        launched = SystemCommands::launch_first({{"gio", "launch", item.target}});
    } else if (item.kind == Item::Kind::Uri) {
        launched = SystemCommands::launch_first({{"xdg-open", item.target}});
    } else if (item.kind == Item::Kind::Terminal) {
        const std::string terminal = find_default_terminal();
        if (!terminal.empty()) {
            gchar* quoted_command = g_shell_quote(item.target.c_str());
            const std::string command = "printf '$ %s\\n' " +
                std::string(quoted_command) + "\n" + item.target +
                "\nread -p 'Press Enter to close...'\n";
            g_free(quoted_command);
            launched = SystemCommands::launch_first({{terminal, "-e", "bash", "-lc", command}});
        }
    } else {
        launched = SystemCommands::launch_first({{"xdg-open", item.target}});
    }
    if (launched) {
        backend_.close_desktop_hub();
    }
}

void DesktopHubModule::clear_power_confirmation()
{
    confirming_power_ = false;
    pending_power_action_.clear();
    pending_power_label_.clear();
    confirmation_seconds_ = 0;
    backend_.set_desktop_hub_countdown_active(false);
}

void DesktopHubModule::refresh_display(std::size_t selected, bool preserve_selection)
{
    visible_items_ = base_results_;
    visible_items_.insert(visible_items_.end(), file_results_.begin(), file_results_.end());
    backend_.update_desktop_hub(current_query_, display_items(), selected,
                                preserve_selection);
}

void DesktopHubModule::schedule_file_search(const std::string& query,
                                           std::uint64_t generation)
{
    if (file_search_source_id_ != 0) {
        g_source_remove(file_search_source_id_);
        file_search_source_id_ = 0;
    }
    struct Request {
        DesktopHubModule* self;
        std::weak_ptr<std::atomic<bool>> alive;
        std::string query;
        std::uint64_t generation;
    };
    auto* request = new Request{this, alive_, query, generation};
    file_search_source_id_ = g_timeout_add_full(
        G_PRIORITY_DEFAULT, 180,
        [](gpointer data) -> gboolean {
            auto* request = static_cast<Request*>(data);
            request->self->file_search_source_id_ = 0;
            const auto alive = request->alive.lock();
            if (alive != nullptr && alive->load() &&
                request->self->search_generation_->load() == request->generation) {
                request->self->start_file_search(request->query, request->generation);
            }
            return G_SOURCE_REMOVE;
        }, request, [](gpointer data) { delete static_cast<Request*>(data); });
}

void DesktopHubModule::start_file_search(const std::string& raw_query,
                                         std::uint64_t generation)
{
    const std::string query = lower(trim(raw_query));
    const std::string home = g_get_home_dir();
    const std::weak_ptr<std::atomic<bool>> lifetime = alive_;
    const auto generations = search_generation_;
    auto* self = this;
    std::thread([query, home, generation, lifetime, generations, self] {
        struct Match {
            int rank;
            int directory_priority;
            std::size_t name_length;
            Item item;
        };
        std::vector<Match> matches;
        std::error_code error;
        std::filesystem::recursive_directory_iterator iterator(
            home, std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        std::string clean_query = query;
        if (clean_query.rfind("*.", 0) == 0) clean_query = clean_query.substr(2);
        else if (clean_query.rfind('.', 0) == 0 && clean_query.size() > 1) clean_query = clean_query.substr(1);
        const bool extension_query = query.rfind("*.", 0) == 0 ||
            (query.rfind('.', 0) == 0 && query.size() > 1) ||
            (query.find('/') == std::string::npos && clean_query.size() <= 5 &&
             query.find(' ') == std::string::npos);
        std::string extension = query.rfind("*.", 0) == 0 ? query.substr(2) :
            (query[0] == '.' ? query.substr(1) : "");
        if (query.find("*.") != 0 && query[0] != '.') extension = query;
        while (iterator != end && generations->load() == generation) {
            if (error) {
                error.clear();
                iterator.increment(error);
                continue;
            }
            const std::string name = iterator->path().filename().string();
            if (iterator->is_directory(error)) {
                if (!name.empty() && name.front() == '.') {
                    iterator.disable_recursion_pending();
                }
            }
            if (!name.empty() && name.front() != '.') {
                const std::string lowered = lower(name);
                int rank = -1;
                if (extension_query && lower(iterator->path().extension().string()) == "." + extension) {
                    rank = 0;
                }
                if (rank < 0 && lowered == query) {
                    rank = 0;
                } else if (rank < 0 && lowered.rfind(query, 0) == 0) {
                    rank = 1;
                } else if (rank < 0 && lowered.find(query) != std::string::npos) {
                    rank = 2;
                }
                if (rank >= 0) {
                    int priority = 2;
                    const std::string relative = iterator->path().lexically_relative(home).string();
                    const std::size_t separator = relative.find('/');
                    const std::string top = lower(relative.substr(0, separator));
                    if (top == "desktop" || top == "documents" || top == "downloads" ||
                        top == "music" || top == "pictures" || top == "videos") {
                        priority = 0;
                    } else if (separator == std::string::npos) {
                        priority = 1;
                    }
                    matches.push_back({rank, priority, lowered.size(), {
                        "[" + std::string(iterator->is_directory(error) ? "Folder" : "File") + "] " +
                            name + " — " + iterator->path().parent_path().string(),
                        iterator->path().string(),
                        iterator->is_directory(error) ? "folder-symbolic" : "text-x-generic-symbolic",
                        Item::Kind::Path
                    }});
                }
            }
            iterator.increment(error);
        }
        std::stable_sort(matches.begin(), matches.end(), [](const Match& left, const Match& right) {
            if (left.rank != right.rank) return left.rank < right.rank;
            if (left.directory_priority != right.directory_priority) {
                return left.directory_priority < right.directory_priority;
            }
            return left.name_length < right.name_length;
        });
        std::vector<Item> results;
        const std::size_t count = std::min<std::size_t>(matches.size(), 30);
        results.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            results.push_back(std::move(matches[index].item));
        }
        struct Delivery {
            DesktopHubModule* self;
            std::weak_ptr<std::atomic<bool>> lifetime;
            std::uint64_t generation;
            std::vector<Item> items;
        };
        auto* delivery = new Delivery{self, lifetime, generation, std::move(results)};
        g_idle_add([](gpointer data) -> gboolean {
            std::unique_ptr<Delivery> result(static_cast<Delivery*>(data));
            const auto alive = result->lifetime.lock();
            if (alive == nullptr || !alive->load() ||
                result->self->search_generation_->load() != result->generation ||
                !result->self->backend_.desktop_hub_visible()) {
                return G_SOURCE_REMOVE;
            }
            result->self->file_results_ = std::move(result->items);
            result->self->refresh_display(0, true);
            return G_SOURCE_REMOVE;
        }, delivery);
    }).detach();
}

bool DesktopHubModule::execute_power_action(const std::string& action)
{
    return PowerActions::execute(action);
}

std::vector<HubDisplayItem> DesktopHubModule::display_items() const
{
    std::vector<HubDisplayItem> rows;
    rows.reserve(visible_items_.size());
    for (const auto& item : visible_items_) {
        rows.push_back({item.label, item.icon_name});
    }
    return rows;
}

std::string DesktopHubModule::setting(const std::string& key,
                                      const std::string& fallback) const
{
    const auto found = config_.find(key);
    return found == config_.end() ? fallback : found->second;
}
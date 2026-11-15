#include "panel/icon_theme.hpp"
#include "panel/logging.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>

namespace {

struct CairoSurfaceDeleter {
    void operator()(cairo_surface_t* surface) const
    {
        if (surface != nullptr) {
            cairo_surface_destroy(surface);
        }
    }
};

struct CachedIcon {
    std::unique_ptr<cairo_surface_t, CairoSurfaceDeleter> surface;
    int width{0};
    int height{0};
};

struct ThemeIconIndex {
    std::mutex mutex;
    std::condition_variable ready_condition;
    bool ready{false};
    bool cache_available{false};
    std::uint64_t generation{0};
    std::unordered_map<std::string, std::filesystem::path> files;
    std::once_flag build_once;
};

struct IconDecodeRequest {
    std::string key;
    std::string name;
    int size;
};

struct ThemeIconCache {
    std::mutex mutex;
    std::condition_variable request_condition;
    std::unordered_map<std::string, CachedIcon> icons;
    std::unordered_set<std::string> pending;
    std::deque<IconDecodeRequest> requests;
    std::function<void()> redraw_callback;
    std::thread worker;
    bool redraw_pending{false};
    bool stopping{false};

    ~ThemeIconCache();
    void process_requests();
};

ThemeIconIndex& theme_icon_index()
{
    static ThemeIconIndex state;
    return state;
}

ThemeIconCache& theme_icon_cache()
{
    static ThemeIconCache cache;
    return cache;
}

void notify_icon_redraw();

std::uint64_t icon_index_generation()
{
    auto& state = theme_icon_index();
    std::lock_guard lock(state.mutex);
    return state.generation;
}

std::string configured_theme()
{
    if (const char* environment_theme = std::getenv("GTK_ICON_THEME_NAME")) {
        if (*environment_theme != '\0') {
            return environment_theme;
        }
    }

    const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
    if (desktop != nullptr) {
        std::string desktop_name = desktop;
        std::transform(desktop_name.begin(), desktop_name.end(), desktop_name.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        if (desktop_name.find("kde") != std::string::npos) {
            GKeyFile* key_file = g_key_file_new();
            const std::filesystem::path kde_config =
                std::filesystem::path(g_get_user_config_dir()) / "kdeglobals";
            if (g_key_file_load_from_file(key_file, kde_config.c_str(),
                                          G_KEY_FILE_NONE, nullptr)) {
                gchar* theme = g_key_file_get_string(key_file, "Icons", "Theme", nullptr);
                if (theme != nullptr && *theme != '\0') {
                    const std::string result = theme;
                    g_free(theme);
                    g_key_file_unref(key_file);
                    return result;
                }
                g_free(theme);
            }
            g_key_file_unref(key_file);
        }
    }

    GSettingsSchemaSource* source = g_settings_schema_source_get_default();
    if (source == nullptr) {
        return "Adwaita";
    }
    GSettingsSchema* schema = g_settings_schema_source_lookup(
        source, "org.gnome.desktop.interface", TRUE);
    if (schema == nullptr) {
        return "Adwaita";
    }
    GSettings* settings = g_settings_new_full(schema, nullptr, nullptr);
    gchar* theme = g_settings_get_string(settings, "icon-theme");
    const std::string result = theme != nullptr && *theme != '\0' ? theme : "Adwaita";
    g_free(theme);
    g_object_unref(settings);
    g_settings_schema_unref(schema);
    return result;
}

std::vector<std::filesystem::path> icon_roots()
{
    std::vector<std::filesystem::path> roots;
    if (const char* home = std::getenv("HOME")) {
        roots.emplace_back(std::filesystem::path(home) / ".icons");
    }
    const char* data_home = std::getenv("XDG_DATA_HOME");
    roots.emplace_back(data_home != nullptr && *data_home != '\0'
        ? std::filesystem::path(data_home) / "icons"
        : std::filesystem::path(std::getenv("HOME") != nullptr ? std::getenv("HOME") : ".") /
              ".local/share/icons");

    const char* data_dirs = std::getenv("XDG_DATA_DIRS");
    const std::string search_dirs = data_dirs != nullptr && *data_dirs != '\0'
        ? data_dirs : "/usr/local/share:/usr/share";
    std::size_t start = 0;
    while (start <= search_dirs.size()) {
        const std::size_t separator = search_dirs.find(':', start);
        const std::string directory = search_dirs.substr(
            start, separator == std::string::npos ? std::string::npos : separator - start);
        if (!directory.empty()) {
            roots.emplace_back(std::filesystem::path(directory) / "icons");
        }
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1;
    }
    roots.emplace_back("/usr/share/pixmaps");
    return roots;
}

std::vector<std::string> inherited_themes(const std::filesystem::path& theme_dir)
{
    std::vector<std::string> inherited;
    GKeyFile* key_file = g_key_file_new();
    const std::filesystem::path index = theme_dir / "index.theme";
    if (g_key_file_load_from_file(key_file, index.c_str(), G_KEY_FILE_NONE, nullptr)) {
        gchar* value = g_key_file_get_string(key_file, "Icon Theme", "Inherits", nullptr);
        gchar** names = value != nullptr ? g_strsplit_set(value, ",;", -1) : nullptr;
        for (gsize index = 0; names != nullptr && names[index] != nullptr; ++index) {
            gchar* name = g_strstrip(names[index]);
            if (name != nullptr && *name != '\0') {
                inherited.emplace_back(name);
            }
        }
        g_strfreev(names);
        g_free(value);
    }
    g_key_file_unref(key_file);
    return inherited;
}

std::vector<std::filesystem::path> theme_directories()
{
    const std::vector<std::filesystem::path> roots = icon_roots();
    std::vector<std::filesystem::path> directories;
    const std::string selected_theme = configured_theme();
    std::vector<std::string> themes{selected_theme};
    for (std::size_t index = 0; index < themes.size(); ++index) {
            for (const auto& root : roots) {
            const auto directory = root / themes[index];
            std::error_code error;
            if (!std::filesystem::is_directory(directory, error)) {
                continue;
            }
            directories.push_back(directory);
            for (const auto& inherited : inherited_themes(directory)) {
                if (std::find(themes.begin(), themes.end(), inherited) == themes.end()) {
                    themes.push_back(inherited);
                }
            }
        }
    }
    for (const char* fallback : {"Adwaita", "hicolor"}) {
        if (std::find(themes.begin(), themes.end(), fallback) != themes.end()) {
            continue;
        }
        for (const auto& root : roots) {
            const auto directory = root / fallback;
            std::error_code error;
            if (std::filesystem::is_directory(directory, error)) {
                directories.push_back(directory);
                break;
            }
        }
    }
    for (const auto& root : roots) {
        if (root.filename() == "pixmaps") {
            std::error_code error;
            if (std::filesystem::is_directory(root, error)) {
                directories.push_back(root);
            }
        }
    }
    panel_debug("icon theme selected='", selected_theme, "' search_directories=", directories.size());
    for (const auto& directory : directories) {
        panel_debug("icon theme search path: ", directory.string());
    }
    return directories;
}

std::string encode_hex(const std::string& value)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2);
    for (const unsigned char character : value) {
        encoded.push_back(digits[character >> 4]);
        encoded.push_back(digits[character & 0x0f]);
    }
    return encoded;
}

bool decode_hex(const std::string& encoded, std::string& value)
{
    if (encoded.size() % 2 != 0) {
        return false;
    }
    const auto digit_value = [](char digit) {
        if (digit >= '0' && digit <= '9') return digit - '0';
        if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
        if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
        return -1;
    };
    value.clear();
    value.reserve(encoded.size() / 2);
    for (std::size_t index = 0; index < encoded.size(); index += 2) {
        const int high = digit_value(encoded[index]);
        const int low = digit_value(encoded[index + 1]);
        if (high < 0 || low < 0) {
            value.clear();
            return false;
        }
        value.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

std::string icon_index_signature(
    const std::vector<std::filesystem::path>& directories)
{
    std::string signature = encode_hex(configured_theme());
    for (const auto& directory : directories) {
        signature.push_back('\n');
        signature += encode_hex(directory.string());
    }
    return signature;
}

std::filesystem::path icon_index_cache_path()
{
    return std::filesystem::path(g_get_user_config_dir()) /
        "mdeon-panel" / "icon-index-v2.cache";
}

using IconDirectorySnapshot = std::unordered_map<std::string, std::int64_t>;

bool load_icon_index_cache(
    const std::string& signature,
    std::unordered_map<std::string, std::filesystem::path>& files,
    IconDirectorySnapshot& directory_snapshot)
{
    std::ifstream input(icon_index_cache_path(), std::ios::binary);
    std::string line;
    if (!std::getline(input, line) || line != "MDEON_ICON_INDEX_V2" ||
        !std::getline(input, line)) {
        return false;
    }
    std::string cached_signature;
    if (!decode_hex(line, cached_signature) || cached_signature != signature) {
        return false;
    }

    std::unordered_map<std::string, std::filesystem::path> cached_files;
    IconDirectorySnapshot cached_directories;
    while (std::getline(input, line)) {
        const std::size_t first_separator = line.find('\t');
        const std::size_t second_separator = line.find('\t', first_separator + 1);
        if (first_separator != 1 || second_separator == std::string::npos) {
            return false;
        }
        std::string path;
        if (line[0] == 'D') {
            std::int64_t modified_ticks = 0;
            try {
                std::size_t parsed = 0;
                modified_ticks = std::stoll(line.substr(second_separator + 1), &parsed);
                if (parsed != line.size() - second_separator - 1) {
                    return false;
                }
            } catch (...) {
                return false;
            }
            if (!decode_hex(line.substr(first_separator + 1,
                                        second_separator - first_separator - 1), path) ||
                path.empty()) {
                return false;
            }
            cached_directories.insert_or_assign(std::move(path), modified_ticks);
        } else if (line[0] == 'F') {
            std::string name;
            if (!decode_hex(line.substr(first_separator + 1,
                                        second_separator - first_separator - 1), name) ||
                !decode_hex(line.substr(second_separator + 1), path) ||
                name.empty() || path.empty()) {
                return false;
            }
            cached_files.insert_or_assign(std::move(name),
                                          std::filesystem::path(std::move(path)));
        } else {
            return false;
        }
    }
    if (!input.eof()) {
        return false;
    }
    files = std::move(cached_files);
    directory_snapshot = std::move(cached_directories);
    return true;
}

bool directory_snapshot_is_current(const IconDirectorySnapshot& snapshot)
{
    if (snapshot.empty()) {
        return false;
    }
    for (const auto& [directory, modified_ticks] : snapshot) {
        std::error_code error;
        const auto modified = std::filesystem::last_write_time(directory, error);
        if (error || modified.time_since_epoch().count() != modified_ticks) {
            return false;
        }
    }
    return true;
}

void save_icon_index_cache(
    const std::string& signature,
    const std::unordered_map<std::string, std::filesystem::path>& files,
    const IconDirectorySnapshot& directory_snapshot)
{
    const std::filesystem::path cache_path = icon_index_cache_path();
    std::error_code error;
    std::filesystem::create_directories(cache_path.parent_path(), error);
    if (error) {
        panel_debug("icon index cache directory creation failed: ", error.message());
        return;
    }

    std::filesystem::path temporary_path = cache_path;
    temporary_path += "." + std::to_string(getpid()) + ".tmp";
    std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        panel_debug("icon index cache could not be opened for writing");
        return;
    }
    output << "MDEON_ICON_INDEX_V2\n" << encode_hex(signature) << '\n';
    std::vector<std::string> directories;
    directories.reserve(directory_snapshot.size());
    for (const auto& [directory, modified_ticks] : directory_snapshot) {
        static_cast<void>(modified_ticks);
        directories.push_back(directory);
    }
    std::sort(directories.begin(), directories.end());
    for (const auto& directory : directories) {
        output << "D\t" << encode_hex(directory) << '\t'
               << directory_snapshot.at(directory) << '\n';
    }
    std::vector<std::string> names;
    names.reserve(files.size());
    for (const auto& [name, path] : files) {
        static_cast<void>(path);
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        output << "F\t" << encode_hex(name) << '\t'
               << encode_hex(files.at(name).string()) << '\n';
    }
    output.flush();
    if (!output) {
        output.close();
        std::filesystem::remove(temporary_path, error);
        panel_debug("icon index cache write failed");
        return;
    }
    output.close();
    std::filesystem::rename(temporary_path, cache_path, error);
    if (error) {
        std::filesystem::remove(temporary_path, error);
        panel_debug("icon index cache replacement failed: ", error.message());
    }
}

void start_icon_index_build()
{
    auto& state = theme_icon_index();
    std::call_once(state.build_once, [&state] {
        const auto directories = theme_directories();
        const std::string signature = icon_index_signature(directories);
        std::unordered_map<std::string, std::filesystem::path> cached_files;
        IconDirectorySnapshot cached_directories;
        const bool cache_loaded = load_icon_index_cache(
            signature, cached_files, cached_directories);
        const bool cache_current = cache_loaded &&
            directory_snapshot_is_current(cached_directories);
        if (cache_loaded) {
            std::lock_guard lock(state.mutex);
            state.files = std::move(cached_files);
            state.cache_available = true;
            state.ready = cache_current;
            panel_debug("icon index cache loaded: entries=", state.files.size());
            state.ready_condition.notify_all();
        }

        if (cache_current) {
            panel_debug("icon index cache is current; skipping recursive scan");
            return;
        }

        std::thread([&state, directories, signature, cache_loaded,
                     cached_directories = std::move(cached_directories)] {
            const auto started = std::chrono::steady_clock::now();
            std::unordered_map<std::string, std::filesystem::path> files;
            std::unordered_map<std::string, int> priorities;
            std::vector<std::filesystem::path> scanned_directories = directories;
            for (std::size_t directory_index = 0; directory_index < directories.size();
                 ++directory_index) {
                const auto& directory = directories[directory_index];
                std::error_code scan_error;
                for (std::filesystem::recursive_directory_iterator iterator(
                         directory, std::filesystem::directory_options::skip_permission_denied,
                         scan_error), end;
                     iterator != end; iterator.increment(scan_error)) {
                    if (scan_error) {
                        scan_error.clear();
                        continue;
                    }
                    if (iterator->is_directory(scan_error)) {
                        scanned_directories.push_back(iterator->path());
                        continue;
                    }
                    if (scan_error) {
                        scan_error.clear();
                        continue;
                    }
                    if (!iterator->is_regular_file(scan_error)) {
                        scan_error.clear();
                        continue;
                    }
                    const auto& path = iterator->path();
                    const std::string extension = path.extension().string();
                    const int extension_priority = extension == ".svg" ? 0 :
                        extension == ".png" ? 1 : extension == ".xpm" ? 2 : 3;
                    if (extension_priority == 3) {
                        continue;
                    }
                    const std::string stem = path.stem().string();
                    const int priority = static_cast<int>(directory_index) * 3 +
                        extension_priority;
                    const auto found = priorities.find(stem);
                    if (found == priorities.end() || priority < found->second) {
                        priorities.insert_or_assign(stem, priority);
                        files.insert_or_assign(stem, path);
                    }
                }
            }
            IconDirectorySnapshot directory_snapshot;
            for (const auto& directory : scanned_directories) {
                std::error_code error;
                const auto modified = std::filesystem::last_write_time(directory, error);
                if (!error) {
                    directory_snapshot.insert_or_assign(directory.string(),
                        modified.time_since_epoch().count());
                }
            }
            bool index_changed = false;
            {
                std::lock_guard lock(state.mutex);
                index_changed = state.files != files;
                if (index_changed) {
                    state.files = files;
                    ++state.generation;
                }
                state.cache_available = true;
                state.ready = true;
                panel_debug("icon index ready: entries=", state.files.size(), " elapsed_ms=",
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started).count());
            }
            state.ready_condition.notify_all();
            if (index_changed) {
                notify_icon_redraw();
            }
            if (!cache_loaded || index_changed || cached_directories != directory_snapshot) {
                save_icon_index_cache(signature, files, directory_snapshot);
            }
        }).detach();
    });
}

bool icon_index_ready()
{
    auto& state = theme_icon_index();
    std::lock_guard lock(state.mutex);
    return state.ready;
}

std::filesystem::path find_icon_file(const std::string& name)
{
    std::error_code error;
    const std::filesystem::path direct(name);
    if (direct.is_absolute() && std::filesystem::is_regular_file(direct, error)) {
        return direct;
    }
    start_icon_index_build();
    auto& state = theme_icon_index();
    std::lock_guard lock(state.mutex);
    if (!state.cache_available && !state.ready) {
        return {};
    }
    auto found = state.files.find(name);
    if (found == state.files.end()) {
        found = state.files.find(direct.stem().string());
    }
    return found == state.files.end() ? std::filesystem::path{} : found->second;
}

std::string resolve_icon_alias(const std::string& name)
{
    if (!find_icon_file(name).empty()) {
        return name;
    }
    static const std::unordered_map<std::string, std::string> aliases = {
        {"vscode", "visual-studio-code"},
        {"code", "visual-studio-code"},
        {"com.microsoft.vscode", "visual-studio-code"}
    };
    const auto found = aliases.find(name);
    return found != aliases.end() && !find_icon_file(found->second).empty()
        ? found->second : std::string{};
}

CachedIcon load_icon(const std::string& name, int size)
{
    CachedIcon result;
    const std::filesystem::path file = find_icon_file(name);
    if (file.empty()) {
        panel_debug("icon not found: name='", name, "' size=", size);
        return result;
    }

    panel_debug("loading icon: name='", name, "' path='", file.string(), "' size=", size);
    GError* error = nullptr;
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file_at_scale(
        file.c_str(), size, size, TRUE, &error);
    if (pixbuf == nullptr) {
        panel_debug("icon decode failed: path='", file.string(), "' reason='",
            error != nullptr ? error->message : "unknown error", "'");
        g_clear_error(&error);
        return result;
    }

    result.width = gdk_pixbuf_get_width(pixbuf);
    result.height = gdk_pixbuf_get_height(pixbuf);
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                          result.width, result.height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        panel_debug("icon surface creation failed: path='", file.string(), "' status=",
            cairo_status_to_string(cairo_surface_status(surface)));
        cairo_surface_destroy(surface);
        g_object_unref(pixbuf);
        result.width = 0;
        result.height = 0;
        return result;
    }

    cairo_surface_flush(surface);
    unsigned char* destination = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const guchar* pixels = gdk_pixbuf_read_pixels(pixbuf);
    const int source_stride = gdk_pixbuf_get_rowstride(pixbuf);
    const int channels = gdk_pixbuf_get_n_channels(pixbuf);
    const bool has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
    const bool symbolic = name.find("-symbolic") != std::string::npos;
    for (int row = 0; row < result.height; ++row) {
        const guchar* source = pixels + row * source_stride;
        auto* target = reinterpret_cast<uint32_t*>(destination + row * stride);
        for (int column = 0; column < result.width; ++column) {
            const int offset = column * channels;
            const uint32_t alpha = has_alpha ? source[offset + 3] : 255;
            const uint32_t red = (symbolic ? 34 : source[offset]) * alpha / 255;
            const uint32_t green = (symbolic ? 34 : source[offset + 1]) * alpha / 255;
            const uint32_t blue = (symbolic ? 34 : source[offset + 2]) * alpha / 255;
            target[column] = (alpha << 24) | (red << 16) | (green << 8) | blue;
        }
    }
    cairo_surface_mark_dirty(surface);
    g_object_unref(pixbuf);
    result.surface.reset(surface);
    panel_debug("icon loaded: name='", name, "' dimensions=", result.width, "x", result.height);
    return result;
}

gboolean redraw_after_icon_decode(gpointer)
{
    auto& cache = theme_icon_cache();
    std::function<void()> callback;
    {
        std::lock_guard lock(cache.mutex);
        cache.redraw_pending = false;
        callback = cache.redraw_callback;
    }
    if (callback) {
        callback();
    }
    return G_SOURCE_REMOVE;
}

void notify_icon_redraw()
{
    auto& cache = theme_icon_cache();
    bool notify = false;
    {
        std::lock_guard lock(cache.mutex);
        if (cache.redraw_callback && !cache.redraw_pending) {
            cache.redraw_pending = true;
            notify = true;
        }
    }
    if (notify) {
        g_main_context_invoke(nullptr, redraw_after_icon_decode, nullptr);
    }
}

ThemeIconCache::~ThemeIconCache()
{
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    request_condition.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
}

void ThemeIconCache::process_requests()
{
    while (true) {
        IconDecodeRequest request;
        {
            std::unique_lock lock(mutex);
            request_condition.wait(lock, [this] {
                return stopping || !requests.empty();
            });
            if (stopping) {
                return;
            }
            request = std::move(requests.front());
            requests.pop_front();
        }

        warm_theme_icon_index();
        CachedIcon icon = load_icon(request.name, request.size);
        bool notify_redraw = false;
        {
            std::lock_guard lock(mutex);
            const bool decoded = icon.surface != nullptr;
            icons.insert_or_assign(request.key, std::move(icon));
            pending.erase(request.key);
            if (decoded && redraw_callback && !redraw_pending) {
                redraw_pending = true;
                notify_redraw = true;
            }
        }
        if (notify_redraw) {
            g_main_context_invoke(nullptr, redraw_after_icon_decode, nullptr);
        }
    }
}

void request_icon_decode(const std::string& key, const std::string& name, int size)
{
    auto& cache = theme_icon_cache();
    {
        std::lock_guard lock(cache.mutex);
        if (cache.icons.contains(key) || !cache.pending.insert(key).second) {
            return;
        }
        cache.requests.push_back({key, name, size});
        if (!cache.worker.joinable()) {
            cache.worker = std::thread([&cache] { cache.process_requests(); });
        }
    }
    cache.request_condition.notify_one();
}

}

void warm_theme_icon_index()
{
    panel_debug("waiting for icon index build");
    start_icon_index_build();
    auto& state = theme_icon_index();
    std::unique_lock lock(state.mutex);
    state.ready_condition.wait(lock, [&state] {
        return state.cache_available || state.ready;
    });
    panel_debug("icon index wait complete");
}

void set_theme_icon_redraw_callback(std::function<void()> callback)
{
    auto& cache = theme_icon_cache();
    std::lock_guard lock(cache.mutex);
    cache.redraw_callback = std::move(callback);
}

bool theme_icon_index_ready()
{
    return icon_index_ready();
}

std::string resolve_theme_icon_name(const std::string& icon_name)
{
    return resolve_icon_alias(icon_name);
}

std::string resolve_application_icon(const std::string& app_id)
{
    if (app_id.empty()) {
        return {};
    }

    std::vector<std::string> desktop_ids{app_id};
    if (app_id.ends_with(".desktop")) {
        desktop_ids.push_back(app_id.substr(0, app_id.size() - 8));
    } else {
        desktop_ids.push_back(app_id + ".desktop");
    }
    for (const auto& desktop_id : desktop_ids) {
        GDesktopAppInfo* app_info = g_desktop_app_info_new(desktop_id.c_str());
        if (app_info == nullptr) {
            continue;
        }
        GIcon* icon = g_app_info_get_icon(G_APP_INFO(app_info));
        if (icon != nullptr && G_IS_THEMED_ICON(icon)) {
            const gchar* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
            for (const gchar* const* name = names; name != nullptr && *name != nullptr; ++name) {
                const std::string resolved = resolve_icon_alias(*name);
                if (!resolved.empty()) {
                    panel_debug("application icon resolved: app_id='", app_id,
                        "' desktop_id='", desktop_id, "' icon='", resolved, "'");
                    g_object_unref(app_info);
                    return resolved;
                }
            }
        } else if (icon != nullptr && G_IS_FILE_ICON(icon)) {
            GFile* file = g_file_icon_get_file(G_FILE_ICON(icon));
            gchar* path = g_file_get_path(file);
            if (path != nullptr) {
                const std::string resolved = path;
                panel_debug("application file icon resolved: app_id='", app_id,
                    "' path='", resolved, "'");
                g_free(path);
                g_object_unref(app_info);
                return resolved;
            }
        }
        g_object_unref(app_info);
    }
    const std::string resolved = resolve_icon_alias(app_id);
    panel_debug("application icon fallback: app_id='", app_id, "' icon='", resolved, "'");
    return resolved;
}

void paint_theme_icon(cairo_t* cr, const std::string& icon_name,
                      double center_x, double center_y, int size)
{
    const std::string key = icon_name + "@" + std::to_string(size) + "#" +
        std::to_string(icon_index_generation());
    cairo_surface_t* surface = nullptr;
    int width = 0;
    int height = 0;
    bool cached = false;
    auto& cache = theme_icon_cache();
    {
        std::lock_guard lock(cache.mutex);
        const auto found = cache.icons.find(key);
        if (found != cache.icons.end()) {
            cached = true;
            surface = found->second.surface.get();
            width = found->second.width;
            height = found->second.height;
        }
    }
    if (!cached) {
        start_icon_index_build();
        request_icon_decode(key, icon_name, size);
        return;
    }
    if (surface == nullptr) {
        return;
    }

    cairo_set_source_surface(cr, surface,
                             center_x - width / 2.0,
                             center_y - height / 2.0);
    cairo_paint(cr);
}
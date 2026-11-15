#include "panel/config_editor_model.hpp"

#include <cairo-xlib.h>
#include <pango/pangocairo.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr int initial_width = 920;
constexpr int initial_height = 660;
constexpr int header_height = 58;
constexpr int footer_height = 30;
constexpr int sidebar_width = 210;
constexpr int row_height = 42;
constexpr int item_row_height = 36;
constexpr int item_row_pitch = 40;
constexpr int scrollbar_width = 8;

enum class FieldKind { Text, Toggle, Number, Choice, Color };

struct Field {
    std::string key;
    std::string label;
    FieldKind kind{FieldKind::Text};
    std::string initial;
    int minimum{0};
    int maximum{100};
    std::vector<std::pair<std::string, std::string>> options;
};

struct Page {
    std::string id;
    std::string title;
    char code{0};
    std::vector<Field> fields;
};

Field text_field(std::string key, std::string label, std::string value = {})
{
    return {std::move(key), std::move(label), FieldKind::Text, std::move(value), 0, 100, {}};
}

Field color_field(std::string key, std::string label, std::string value = "#ffffff 100")
{
    return {std::move(key), std::move(label), FieldKind::Color, std::move(value), 0, 100, {}};
}

Field toggle_field(std::string key, std::string label, bool value)
{
    return {std::move(key), std::move(label), FieldKind::Toggle, value ? "1" : "0", 0, 1, {}};
}

Field number_field(std::string key, std::string label, int value, int minimum, int maximum)
{
    return {std::move(key), std::move(label), FieldKind::Number, std::to_string(value), minimum, maximum, {}};
}

Field choice_field(std::string key, std::string label, std::string initial,
                   std::vector<std::pair<std::string, std::string>> options)
{
    return {std::move(key), std::move(label), FieldKind::Choice,
            std::move(initial), 0, 0, std::move(options)};
}

using DesktopHubFolder = std::pair<std::string, std::string>;
struct MenuEntry {
    std::string label;
    std::string icon;
    std::string target;
};

constexpr const char* default_desktop_hub_folders =
    "Home|~;Desktop|~/Desktop;Documents|~/Documents;Downloads|~/Downloads;"
    "Music|~/Music;Pictures|~/Pictures;Videos|~/Videos;Trash|trash:///;Network|network:///";
constexpr const char* default_menu_places =
    "Computer|drive-harddisk-symbolic|file:///;Home Folder|user-home-symbolic|home;"
    "Network|network-workgroup-symbolic|network:///;Desktop|user-desktop-symbolic|desktop;"
    "Trash|user-trash-symbolic|trash:///";
constexpr const char* default_menu_system =
    "Control Center|preferences-system-symbolic|mate-control-center,cinnamon-settings,"
    "gnome-control-center,xfce4-settings-manager,lxqt-config;Terminal|utilities-terminal-symbolic|"
    "kgx,mate-terminal,gnome-terminal,xfce4-terminal,konsole,kitty,alacritty,xterm";

std::vector<MenuEntry> parse_menu_entries(const std::string& value)
{
    std::vector<MenuEntry> entries;
    std::istringstream input(value);
    std::string record;
    while (std::getline(input, record, ';')) {
        const std::size_t first = record.find('|');
        const std::size_t second = first == std::string::npos
            ? std::string::npos : record.find('|', first + 1);
        if (first == std::string::npos || second == std::string::npos) continue;
        MenuEntry entry{
            ConfigEditorModel::trim(record.substr(0, first)),
            ConfigEditorModel::trim(record.substr(first + 1, second - first - 1)),
            ConfigEditorModel::trim(record.substr(second + 1))};
        if (!entry.label.empty() && !entry.target.empty()) {
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

std::string serialize_menu_entries(const std::vector<MenuEntry>& entries)
{
    std::string value;
    for (const auto& entry : entries) {
        if (entry.label.empty() || entry.target.empty()) continue;
        if (!value.empty()) value += ';';
        value += entry.label + '|' + entry.icon + '|' + entry.target;
    }
    return value;
}

std::vector<DesktopHubFolder> parse_desktop_hub_folders(const std::string& value)
{
    std::vector<DesktopHubFolder> folders;
    std::istringstream input(value);
    std::string entry;
    while (std::getline(input, entry, ';')) {
        const std::size_t separator = entry.find('|');
        if (separator == std::string::npos) continue;
        std::string label = ConfigEditorModel::trim(entry.substr(0, separator));
        std::string path = ConfigEditorModel::trim(entry.substr(separator + 1));
        if (!label.empty() && !path.empty()) folders.emplace_back(std::move(label), std::move(path));
    }
    return folders;
}

std::string serialize_desktop_hub_folders(const std::vector<DesktopHubFolder>& folders)
{
    std::string value;
    for (const auto& [label, path] : folders) {
        if (label.empty() || path.empty()) continue;
        if (!value.empty()) value += ';';
        value += label + '|' + path;
    }
    return value;
}

std::vector<Page> make_pages()
{
    using O = std::pair<std::string, std::string>;
    return {
        {"items", "Panel items", 0, {}},
        {"panel", "Panel", 0, {
            text_field("panel_size", "Size (W H)", "100% 32"),
            number_field("panel_thickness", "Panel size", 32, 12, 128),
            color_field("panel_color", "Panel color", "#1e1e1e"),
            number_field("panel_opacity", "Transparency", 90, 0, 100),
            toggle_field("panel_show_in_task_manager", "Show in task managers", false),
            choice_field("panel_position", "Position", "bottom center horizontal", {
                O{"bottom center horizontal", "Bottom"}, O{"top center horizontal", "Top"},
                O{"left center vertical", "Left"}, O{"right center vertical", "Right"}}),
            text_field("panel_monitor", "Monitor (all, primary, or output name)", "all"),
            text_field("panel_margin", "Margin (x y)", "0 0"),
            text_field("panel_padding", "Padding (L V R)", "2 1 2"),
            number_field("panel_spacing", "Item spacing", 2, 0, 64),
            number_field("panel_background_id", "Background id", 1, 1, 20),
            number_field("module_icon_size", "Module icon size (0=auto)", 0, 0, 128)}},
        {"spacer", "Spacer", 'F', {
            number_field("spacer_length", "Length (0=automatic)", 0, 0, 2000),
            toggle_field("spacer_bar", "Show separator bar", false),
            toggle_field("spacer_bar_start", "Bar at beginning", true),
            toggle_field("spacer_bar_middle", "Bar in middle", false),
            toggle_field("spacer_bar_end", "Bar at end", false)}},
        {"tasks", "Task list", 'T', {
            toggle_field("task_icon", "Show icons", true),
            toggle_field("task_text", "Show text horizontally", true),
            toggle_field("task_monitor_filter", "Only show windows on this monitor", true),
            toggle_field("task_centered", "Center content", true),
            toggle_field("task_borderless", "Borderless tasks", false),
            number_field("task_icon_size", "Icon size (0=auto)", 0, 0, 128),
            text_field("task_maximum_size", "Max size (W H)", "160 32"),
            text_field("task_padding", "Padding", "2 0 2"),
            text_field("task_font", "Font", "sans 9"),
            color_field("task_font_color", "Font color"),
            color_field("task_active_font_color", "Active font color"),
            color_field("task_iconified_font_color", "Iconified font color", "#d0d0d0 80"),
            number_field("task_background_id", "Normal background id", 2, 0, 20),
            number_field("task_active_background_id", "Active background id", 3, 0, 20),
            number_field("task_iconified_background_id", "Iconified background id", 0, 0, 20),
            choice_field("mouse_left", "Left click", "toggle_iconify", {
                O{"toggle_iconify", "Toggle minimize"}, O{"activate", "Activate"}, O{"menu", "Window menu"},
                O{"close", "Close"}, O{"none", "None"}}),
            choice_field("mouse_middle", "Middle click", "close", {
                O{"close", "Close"}, O{"menu", "Window menu"}, O{"none", "None"},
                O{"toggle_iconify", "Toggle"}, O{"activate", "Activate"}}),
            choice_field("mouse_right", "Right click", "menu", {
                O{"menu", "Window menu"}, O{"close", "Close"}, O{"none", "None"},
                O{"toggle_iconify", "Toggle"}, O{"activate", "Activate"}})}},
        {"launcher", "Launcher", 'L', {
            number_field("launcher_icon_size", "Icon size (0=auto)", 0, 0, 128),
            number_field("launcher_max_icons", "Maximum visible icons", 5, 1, 32),
            text_field("launcher_padding", "Padding", "2 1 2"),
            number_field("launcher_background_id", "Background id", 0, 0, 20),
            toggle_field("launcher_tooltip", "Tooltips", true)}},
        {"desktop_hub", "Desktop Hub", 'H', {
            text_field("desktop_hub_icon", "Icon name", "folder-activities"),
            text_field("desktop_hub_text", "Panel text", "Desktop Hub"),
            text_field("desktop_hub_tooltip", "Tooltip", "Desktop Hub")}},
        {"menu", "Application Menu", 'M', {
            text_field("menu_text", "Button text", "Menu"),
            number_field("menu_font_size", "Button text size", 8, 4, 32),
            text_field("menu_icon", "Button icon",
                       "/usr/share/icons/hicolor/scalable/apps/MDEON.svg"),
            text_field("menu_tooltip", "Tooltip", "Open applications menu"),
            number_field("menu_places_width", "Places / System width", 138, 100, 400),
            number_field("menu_categories_width", "Categories width", 128, 100, 400),
            number_field("menu_applications_width", "Applications width", 434, 200, 800),
            number_field("menu_height", "Menu height", 500, 200, 1200),
            toggle_field("menu_show_categories", "Show category sidebar", true),
            toggle_field("menu_show_places", "Show Places and System column", true)}},
        {"virtual_desktop", "Virtual Desktop", 'W', {
            number_field("virtual_desktop_workspaces", "Number of desktops", 2, 1, 32),
            choice_field("virtual_desktop_display", "Display", "icon_number", {
                O{"icon", "Icon"}, O{"number", "Desktop number"}, O{"icon_number", "Icon and number"}})}},
        {"clock", "Clock", 'C', {
            text_field("time1_format", "Line 1 format", "%H:%M"),
            text_field("time2_format", "Line 2 format", "%a %d"),
            text_field("time1_font", "Line 1 font", "sans 8"),
            text_field("time2_font", "Line 2 font", "sans 7"),
            color_field("clock_font_color", "Font color"),
            text_field("clock_padding", "Padding", "4 1"),
            number_field("clock_background_id", "Background id", 0, 0, 20),
            text_field("clock_tooltip", "Tooltip"),
            text_field("clock_lclick_command", "Left-click command"),
            text_field("clock_rclick_command", "Right-click command")}},
        {"battery", "Battery", 'B', {
            toggle_field("battery_hide_if_absent", "Hide if no battery", true),
            text_field("bat1_format", "Format", "%p%"), text_field("bat2_format", "Line 2 format"),
            text_field("battery_tooltip", "Tooltip"), text_field("battery_font", "Font", "sans 8"),
            color_field("battery_font_color", "Font color"), text_field("battery_padding", "Padding", "2 0"),
            number_field("battery_background_id", "Background id", 0, 0, 20)}},
        {"volume", "Volume", 'V', {
            number_field("volume_step", "Scroll step %", 5, 1, 25),
            toggle_field("volume_show_percent", "Show percent", false),
            text_field("volume_mixer_command", "Mixer command", "pavucontrol")}},
        {"network", "Network / Wi-Fi", 'N', {
            toggle_field("network_show_label", "Show SSID label", false),
            text_field("network_settings_command", "Settings command", "nm-connection-editor")}},
        {"bluetooth", "Bluetooth", 'K', {
            toggle_field("bluetooth_hide_if_absent", "Hide if bluetoothctl missing", true),
            text_field("bluetooth_settings_command", "Settings command", "blueman-manager")}},
        {"system_tray", "System tray", 'S', {
            number_field("systray_icon_size", "Icon size (0=auto)", 0, 0, 128),
            color_field("systray_font_color", "Font/icon color")}},
        {"notifications", "Notifications", 'U', {}},
        {"buttons", "Buttons (P)", 'P', {}},
        {"executer", "Executor", 'E', {
            text_field("executer_text", "Button text"), text_field("executer_tooltip", "Tooltip", "Executor"),
            text_field("executer_command", "Script or program")}},
        {"backgrounds", "Backgrounds", 0, {}},
        {"raw", "Raw file", 0, {}},
    };
}

struct Rect { int x; int y; int width; int height; };

struct Hit {
    Rect rect;
    std::string action;
    std::string key;
    int index{-1};
};

void rounded_rect(cairo_t* cr, const Rect& rect, double radius)
{
    const double x = rect.x;
    const double y = rect.y;
    const double w = std::max(0, rect.width);
    const double h = std::max(0, rect.height);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - radius, y + radius, radius, -M_PI_2, 0);
    cairo_arc(cr, x + w - radius, y + h - radius, radius, 0, M_PI_2);
    cairo_arc(cr, x + radius, y + h - radius, radius, M_PI_2, M_PI);
    cairo_arc(cr, x + radius, y + radius, radius, M_PI, 3 * M_PI_2);
    cairo_close_path(cr);
}

int text_width(cairo_t* cr, const std::string& text, const char* font = "Sans 10")
{
    PangoLayout* layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text.c_str(), static_cast<int>(text.size()));
    PangoFontDescription* description = pango_font_description_from_string(font);
    pango_layout_set_font_description(layout, description);
    int width = 0;
    pango_layout_get_pixel_size(layout, &width, nullptr);
    pango_font_description_free(description);
    g_object_unref(layout);
    return width;
}

void draw_text(cairo_t* cr, const std::string& text, int x, int y, int max_width,
               double red, double green, double blue, const char* font = "Sans 10")
{
    PangoLayout* layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text.c_str(), static_cast<int>(text.size()));
    pango_layout_set_width(layout, std::max(1, max_width) * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    PangoFontDescription* description = pango_font_description_from_string(font);
    pango_layout_set_font_description(layout, description);
    pango_font_description_free(description);
    cairo_set_source_rgb(cr, red, green, blue);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

class ConfigEditor {
public:
    ConfigEditor(Display* display, Window window, cairo_t* cr, int width, int height)
        : display_(display), window_(window), cr_(cr), width_(width), height_(height), pages_(make_pages())
    {
        model_.load();
        status_ = "Loaded " + model_.path.string();
        selected_background_ = model_.backgrounds.empty() ? 0 : model_.backgrounds.begin()->first;
        selected_executer_ = model_.executers.empty() ? -1 : 0;
        sync_desktop_hub_folder_draft();
        sync_button_draft();
    }

    void resize(int width, int height) { width_ = width; height_ = height; }
    void pointer_motion(int y)
    {
        if (dragging_scrollbar_.empty()) return;
        const std::string key = dragging_scrollbar_;
        set_scrollbar_position(key, y);
        if (key == "sidebar") render_sidebar_column();
        else if (key == "raw") render_raw_view();
        else if (key == "page") render_page_column();
        else if (key == "launcher") render_page_column();
        else if (key == "desktop_hub_folders") render_page_column();
        else if (key == "menu_places") render_menu_entry_list(0);
        else if (key == "menu_system") render_menu_entry_list(1);
        else if (key == "items") render_item_list_column(false);
        else if (key == "plugins") render_item_list_column(true);
        else render();
    }
    void pointer_release() { dragging_scrollbar_.clear(); }
    void render()
    {
        hits_.clear();
        cairo_set_source_rgb(cr_, 0.955, 0.963, 0.973);
        cairo_paint(cr_);
        draw_header();
        draw_sidebar();
        draw_content();
        draw_footer();
        if (confirm_close_) draw_close_dialog();
        if (color_picker_open_) draw_color_picker();
        cairo_surface_flush(cairo_get_target(cr_));
    }

    void remove_hits(const std::string& action)
    {
        hits_.erase(std::remove_if(hits_.begin(), hits_.end(), [&action](const Hit& hit) {
            return hit.action == action;
        }), hits_.end());
    }

    void render_menu_entry_list(int list_index)
    {
        const std::size_t slot = static_cast<std::size_t>(list_index);
        const std::string key = list_index == 0 ? "menu_places" : "menu_system";
        hits_.erase(std::remove_if(hits_.begin(), hits_.end(), [&key](const Hit& hit) {
            return (hit.action == "menu-entry-select" ||
                    hit.action == "menu-entry-scrollbar") && hit.key == key;
        }), hits_.end());

        const Rect view = menu_entry_list_views_[slot];
        const Rect track = menu_entry_scrollbar_tracks_[slot];
        cairo_save(cr_);
        cairo_rectangle(cr_, view.x, view.y, track.x + track.width - view.x, view.height);
        cairo_clip(cr_);
        cairo_set_source_rgb(cr_, 0.955, 0.963, 0.973);
        cairo_paint(cr_);

        const auto entries = menu_entries(list_index);
        const int extent = menu_entry_content_extent(list_index);
        const int maximum = scrollbar_max_scroll(key);
        menu_entry_scroll_[slot] = std::clamp(menu_entry_scroll_[slot], 0, maximum);
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const int row_y = view.y + static_cast<int>(index) * 34 - menu_entry_scroll_[slot];
            if (row_y + 30 <= view.y || row_y >= view.y + view.height) continue;
            const Rect row{view.x, row_y, view.width, 30};
            draw_list_row(row, entries[index].label + "    " + entries[index].target,
                selected_menu_entry_[slot] == static_cast<int>(index));
            add_hit({row.x, std::max(row.y, view.y), row.width,
                     std::min(row.y + row.height, view.y + view.height) -
                         std::max(row.y, view.y)},
                "menu-entry-select", key, static_cast<int>(index));
        }
        if (maximum > 0) {
            add_hit(track, "menu-entry-scrollbar", key);
        }
        draw_scrollbar(track, extent, menu_entry_scroll_[slot], maximum);
        cairo_restore(cr_);
        cairo_surface_flush(cairo_get_target(cr_));
    }

    void render_sidebar_column()
    {
        remove_hits("page");
        remove_hits("sidebar-scrollbar");
        cairo_save(cr_);
        cairo_rectangle(cr_, 0, header_height, sidebar_width, height_ - header_height - footer_height);
        cairo_clip(cr_);
        draw_sidebar();
        cairo_restore(cr_);
        cairo_surface_flush(cairo_get_target(cr_));
    }

    void render_raw_view()
    {
        remove_hits("raw-view");
        remove_hits("raw-scrollbar");
        cairo_save(cr_);
        cairo_rectangle(cr_, raw_view_.x, raw_view_.y, raw_view_.width, raw_view_.height);
        cairo_clip(cr_);
        draw_raw_view_contents(raw_view_);
        cairo_restore(cr_);
        cairo_surface_flush(cairo_get_target(cr_));
    }

    void render_page_column()
    {
        hits_.erase(std::remove_if(hits_.begin(), hits_.end(), [this](const Hit& hit) {
            return hit.rect.x >= sidebar_width && hit.rect.y >= header_height &&
                hit.rect.y < height_ - footer_height;
        }), hits_.end());
        cairo_save(cr_);
        cairo_rectangle(cr_, sidebar_width + 1, header_height,
                        width_ - sidebar_width - 1, height_ - header_height - footer_height);
        cairo_clip(cr_);
        cairo_set_source_rgb(cr_, 0.955, 0.963, 0.973);
        cairo_paint(cr_);
        draw_content();
        cairo_restore(cr_);
        cairo_surface_flush(cairo_get_target(cr_));
    }

    void render_item_list_column(bool plugins)
    {
        const std::string row_action = plugins ? "plugin-select" : "item-select";
        const std::string scrollbar_action = plugins ? "plugin-scrollbar" : "item-scrollbar";
        remove_hits(row_action);
        remove_hits(scrollbar_action);

        const int content_x = sidebar_width + 1;
        const int content_width = width_ - content_x;
        const int col_width = (content_width - 64 - 16) / 2;
        const int left_x = content_x + 24;
        const int column_x = plugins ? left_x + col_width + 16 : left_x;
        const int list_top = content_top() - content_scroll_ + 78;
        const int list_height = std::max(160, content_height() - 150);
        const int rows_top = list_top + 20;
        const int rows_height = std::max(1, list_height - 20);
        const std::size_t retained_hits = hits_.size();

        cairo_save(cr_);
        cairo_rectangle(cr_, column_x, rows_top, col_width, rows_height);
        cairo_clip(cr_);
        cairo_set_source_rgb(cr_, 0.955, 0.963, 0.973);
        cairo_paint(cr_);
        draw_items(content_x, content_top() - content_scroll_, content_width);
        cairo_restore(cr_);

        std::vector<Hit> redraw_hits(hits_.begin() + static_cast<std::ptrdiff_t>(retained_hits), hits_.end());
        hits_.resize(retained_hits);
        for (auto& hit : redraw_hits) {
            if (hit.action == row_action || hit.action == scrollbar_action) {
                hits_.push_back(std::move(hit));
            }
        }
        cairo_surface_flush(cairo_get_target(cr_));
    }

    void click(int x, int y, unsigned int button)
    {
        if (color_picker_open_) {
            if (button == Button4 || button == Button5) return;
            for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
                const Rect& rect = it->rect;
                if (x >= rect.x && y >= rect.y && x < rect.x + rect.width &&
                    y < rect.y + rect.height &&
                    (it->action == "color-palette" || it->action == "color-cancel" ||
                     it->action == "color-select")) {
                    activate(*it);
                    render();
                    return;
                }
            }
            color_picker_open_ = false;
            render();
            return;
        }
        if (button == Button4 || button == Button5) {
            const int direction = button == Button4 ? -1 : 1;
            if (x < sidebar_width) {
                sidebar_scroll_ = std::clamp(sidebar_scroll_ + direction * row_height * 3,
                                             0, scrollbar_max_scroll("sidebar"));
                render_sidebar_column();
                return;
            }
            else if (current_page_ == "raw") {
                raw_scroll_ = std::clamp(raw_scroll_ + direction * 17 * 3,
                                         0, scrollbar_max_scroll("raw"));
                render_raw_view();
                return;
            }
            else if (current_page_ == "launcher" &&
                                         ((x >= launcher_list_view_.x &&
                                             x < launcher_list_view_.x + launcher_list_view_.width) ||
                                            (x >= launcher_scrollbar_track_.x &&
                                             x < launcher_scrollbar_track_.x + launcher_scrollbar_track_.width)) &&
                     y >= launcher_list_view_.y &&
                     y < launcher_list_view_.y + launcher_list_view_.height) {
                launcher_scroll_ = std::clamp(launcher_scroll_ + direction * row_height * 2,
                                              0, scrollbar_max_scroll("launcher"));
                render_page_column();
                return;
            }
            else if (current_page_ == "desktop_hub" &&
                     ((x >= desktop_hub_folder_list_view_.x &&
                       x < desktop_hub_folder_list_view_.x + desktop_hub_folder_list_view_.width) ||
                      (x >= desktop_hub_folder_scrollbar_track_.x &&
                       x < desktop_hub_folder_scrollbar_track_.x + desktop_hub_folder_scrollbar_track_.width)) &&
                     y >= desktop_hub_folder_list_view_.y &&
                     y < desktop_hub_folder_list_view_.y + desktop_hub_folder_list_view_.height) {
                desktop_hub_folder_scroll_ = std::clamp(
                    desktop_hub_folder_scroll_ + direction * row_height * 2,
                    0, scrollbar_max_scroll("desktop_hub_folders"));
                render_page_column();
                return;
            }
            else if (current_page_ == "menu" &&
                     y >= menu_entry_list_views_[0].y &&
                     y < menu_entry_list_views_[0].y + menu_entry_list_views_[0].height &&
                     (x >= menu_entry_list_views_[0].x &&
                      x < menu_entry_scrollbar_tracks_[1].x + menu_entry_scrollbar_tracks_[1].width)) {
                const int list_index = x < menu_entry_list_views_[1].x ? 0 : 1;
                const std::string key = list_index == 0 ? "menu_places" : "menu_system";
                menu_entry_scroll_[static_cast<std::size_t>(list_index)] = std::clamp(
                    menu_entry_scroll_[static_cast<std::size_t>(list_index)] +
                        direction * row_height * 2,
                    0, scrollbar_max_scroll(key));
                render_menu_entry_list(list_index);
                return;
            }
            else if (has_page_scrollbar()) {
                content_scroll_ = std::clamp(content_scroll_ + direction * row_height * 3,
                                             0, scrollbar_max_scroll("page"));
                render_page_column();
                return;
            }
            else if (current_page_ == "items") {
                const int list_top = content_top() - content_scroll_ + 78;
                const int list_height = std::max(160, content_height() - 150);
                const int content_x = sidebar_width + 1;
                const int content_width = width_ - content_x;
                const int col_width = (content_width - 64 - 16) / 2;
                const int left_x = content_x + 24;
                const int right_x = left_x + col_width + 16;
                const int rows_top = list_top + 20;
                if (y >= rows_top && y < list_top + list_height) {
                    if (x >= left_x && x < left_x + col_width) {
                        const auto items = model_.panel_items();
                        const int extent = items.empty() ? 0 :
                            static_cast<int>(items.size() - 1) * item_row_pitch + item_row_height;
                        const int maximum = std::max(0, extent - (list_height - 20));
                        item_scroll_ = std::clamp(item_scroll_ + direction * row_height * 2, 0, maximum);
                        render_item_list_column(false);
                        return;
                    } else if (x >= right_x && x < right_x + col_width) {
                        const int count = static_cast<int>(ConfigEditorModel::item_catalog().size());
                        const int extent = count == 0 ? 0 : (count - 1) * 42 + 38;
                        const int maximum = std::max(0, extent - (list_height - 20));
                        plugin_scroll_ = std::clamp(plugin_scroll_ + direction * row_height * 2, 0, maximum);
                        render_item_list_column(true);
                        return;
                    }
                }
            } else content_scroll_ = std::max(0, content_scroll_ + direction * row_height * 3);
            render();
            return;
        }
        finish_number_edit();
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
            const Rect& r = it->rect;
            if (x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height) {
                if (button == Button1 &&
                    (it->action == "sidebar-scrollbar" || it->action == "item-scrollbar" ||
                     it->action == "plugin-scrollbar" || it->action == "raw-scrollbar" ||
                     it->action == "page-scrollbar" || it->action == "launcher-scrollbar" ||
                     it->action == "desktop-hub-folder-scrollbar" ||
                     it->action == "menu-entry-scrollbar")) {
                    dragging_scrollbar_ = it->key;
                    const Rect& track = scrollbar_track(dragging_scrollbar_);
                    const int thumb_height = scrollbar_thumb_height(
                        track, scrollbar_content_extent(dragging_scrollbar_));
                    const int thumb_y = scrollbar_thumb_y(
                        track, thumb_height, scrollbar_max_scroll(dragging_scrollbar_));
                    scrollbar_drag_offset_ = y >= thumb_y && y < thumb_y + thumb_height
                        ? y - thumb_y : thumb_height / 2;
                    set_scrollbar_position(dragging_scrollbar_, y);
                    if (dragging_scrollbar_ == "sidebar") render_sidebar_column();
                    else if (dragging_scrollbar_ == "raw") render_raw_view();
                    else if (dragging_scrollbar_ == "page") render_page_column();
                    else if (dragging_scrollbar_ == "launcher") render_page_column();
                    else if (dragging_scrollbar_ == "desktop_hub_folders") render_page_column();
                    else if (dragging_scrollbar_ == "menu_places" ||
                             dragging_scrollbar_ == "menu_system") {
                        render_menu_entry_list(dragging_scrollbar_ == "menu_places" ? 0 : 1);
                    }
                    else if (dragging_scrollbar_ == "items") render_item_list_column(false);
                    else if (dragging_scrollbar_ == "plugins") render_item_list_column(true);
                    else render();
                    return;
                }
                activate(*it);
                render();
                return;
            }
        }
        edit_target_.clear();
        render();
    }

    void key(XKeyEvent& event)
    {
        KeySym symbol = NoSymbol;
        std::array<char, 32> bytes{};
        const int length = XLookupString(&event, bytes.data(), bytes.size(), &symbol, nullptr);
        if (color_picker_open_) {
            if (symbol == XK_Escape) color_picker_open_ = false;
            else if (symbol == XK_Return || symbol == XK_KP_Enter) {
                set_field_value(color_picker_target_, color_picker_key_, color_picker_index_,
                                color_picker_value_);
                color_picker_open_ = false;
            }
            render();
            return;
        }
        if (confirm_close_) {
            if (symbol == XK_Escape) confirm_close_ = false;
            render();
            return;
        }
        if (symbol == XK_Escape) {
            if (editing_number_) {
                number_edit_buffer_.clear();
                editing_number_ = false;
                number_edit_replace_on_next_input_ = false;
            }
            edit_target_.clear();
        } else if (symbol == XK_Return || symbol == XK_KP_Enter || symbol == XK_Tab) {
            finish_number_edit();
            edit_target_.clear();
        } else if (!edit_target_.empty()) {
            if (editing_number_) {
                if (symbol == XK_BackSpace) {
                    if (number_edit_replace_on_next_input_) {
                        number_edit_buffer_.clear();
                    } else if (!number_edit_buffer_.empty()) {
                        number_edit_buffer_.pop_back();
                    }
                    number_edit_replace_on_next_input_ = false;
                } else if (symbol == XK_Delete ||
                           ((event.state & ControlMask) && symbol == XK_a)) {
                    number_edit_buffer_.clear();
                    number_edit_replace_on_next_input_ = false;
                } else if (length > 0 && !(event.state & ControlMask)) {
                    if (number_edit_replace_on_next_input_) {
                        number_edit_buffer_.clear();
                    }
                    for (int index = 0; index < length; ++index) {
                        if (bytes[static_cast<std::size_t>(index)] >= '0' &&
                            bytes[static_cast<std::size_t>(index)] <= '9') {
                            number_edit_buffer_.push_back(bytes[static_cast<std::size_t>(index)]);
                        }
                    }
                    number_edit_replace_on_next_input_ = false;
                }
            } else if (symbol == XK_BackSpace) {
                erase_edit_text();
            } else if (symbol == XK_Delete || ((event.state & ControlMask) && symbol == XK_a)) {
                set_edit_text({});
            } else if (length > 0 && !(event.state & ControlMask) && bytes[0] >= 0x20) {
                append_edit_text(std::string(bytes.data(), static_cast<std::size_t>(length)));
            }
        }
        render();
    }

    void request_close()
    {
        if (model_.dirty) confirm_close_ = true;
        else close_window();
        render();
    }

    [[nodiscard]] bool should_close() const { return close_requested_; }

private:
    Display* display_;
    Window window_;
    cairo_t* cr_;
    int width_;
    int height_;
    ConfigEditorModel model_;
    std::vector<Page> pages_;
    std::vector<Hit> hits_;
    std::string current_page_{"items"};
    std::string status_{"Ready"};
    std::string edit_target_;
    std::string edit_key_;
    int edit_index_{-1};
    std::string number_edit_buffer_;
    bool editing_number_{false};
    bool number_edit_replace_on_next_input_{false};
    int selected_item_{-1};
    int selected_plugin_{-1};
    int selected_button_{-1};
    int selected_executer_{-1};
    int selected_background_{0};
    int sidebar_scroll_{0};
    int content_scroll_{0};
    int item_scroll_{0};
    int plugin_scroll_{0};
    int launcher_scroll_{0};
    int desktop_hub_folder_scroll_{0};
    int selected_desktop_hub_folder_{-1};
    std::array<int, 2> menu_entry_scroll_{};
    std::array<int, 2> selected_menu_entry_{{-1, -1}};
    std::array<std::array<std::string, 3>, 2> menu_entry_draft_{};
    Rect sidebar_scrollbar_track_{0, 0, 0, 0};
    Rect raw_view_{0, 0, 0, 0};
    Rect raw_scrollbar_track_{0, 0, 0, 0};
    Rect page_scrollbar_track_{0, 0, 0, 0};
    Rect launcher_list_view_{0, 0, 0, 0};
    Rect launcher_scrollbar_track_{0, 0, 0, 0};
    Rect desktop_hub_folder_list_view_{0, 0, 0, 0};
    Rect desktop_hub_folder_scrollbar_track_{0, 0, 0, 0};
    std::array<Rect, 2> menu_entry_list_views_{};
    std::array<Rect, 2> menu_entry_scrollbar_tracks_{};
    Rect item_scrollbar_track_{0, 0, 0, 0};
    Rect plugin_scrollbar_track_{0, 0, 0, 0};
    std::string dragging_scrollbar_;
    int scrollbar_drag_offset_{0};
    bool confirm_close_{false};
    bool color_picker_open_{false};
    bool close_after_save_{false};
    bool close_requested_{false};
    bool raw_open_{false};
    std::string color_picker_target_;
    std::string color_picker_key_;
    std::string color_picker_value_;
    int color_picker_index_{-1};
    ConfigEditorModel::Values button_draft_;

    int content_top() const { return header_height + 12; }
    int content_bottom() const { return height_ - footer_height - 12; }
    int content_height() const { return content_bottom() - content_top(); }
    const Page* page(const std::string& id) const
    {
        const auto found = std::find_if(pages_.begin(), pages_.end(), [&id](const Page& value) {
            return value.id == id;
        });
        return found == pages_.end() ? nullptr : &*found;
    }

    void add_hit(Rect rect, std::string action, std::string key = {}, int index = -1)
    {
        hits_.push_back({rect, std::move(action), std::move(key), index});
    }

    const Rect& scrollbar_track(const std::string& key) const
    {
        if (key == "sidebar") return sidebar_scrollbar_track_;
        if (key == "raw") return raw_scrollbar_track_;
        if (key == "page") return page_scrollbar_track_;
        if (key == "launcher") return launcher_scrollbar_track_;
        if (key == "desktop_hub_folders") return desktop_hub_folder_scrollbar_track_;
        if (key == "menu_places") return menu_entry_scrollbar_tracks_[0];
        if (key == "menu_system") return menu_entry_scrollbar_tracks_[1];
        return key == "items" ? item_scrollbar_track_ : plugin_scrollbar_track_;
    }

    std::vector<DesktopHubFolder> desktop_hub_folders() const
    {
        return parse_desktop_hub_folders(
            model_.get("desktop_hub_folders", default_desktop_hub_folders));
    }

    std::vector<MenuEntry> menu_entries(int list_index) const
    {
        const char* key = list_index == 0 ? "menu_places" : "menu_system";
        const char* fallback = list_index == 0 ? default_menu_places : default_menu_system;
        return parse_menu_entries(model_.get(key, fallback));
    }

    int menu_entry_content_extent(int list_index) const
    {
        const auto entries = menu_entries(list_index);
        return entries.empty() ? 0 : static_cast<int>(entries.size()) * 34;
    }

    int menu_entry_list_view_height() const
    {
        return std::clamp(content_height() - 478, 72, 112);
    }

    int desktop_hub_folder_list_view_height() const
    {
        return std::clamp(content_height() - 334, 64, 220);
    }

    int desktop_hub_folder_content_extent() const
    {
        const auto folders = desktop_hub_folders();
        return folders.empty() ? 0 : static_cast<int>(folders.size() - 1) * 36 + 32;
    }

    int launcher_list_content_extent() const
    {
        return model_.launcher_apps.empty() ? 0 :
            static_cast<int>(model_.launcher_apps.size() - 1) * 34 + 30;
    }

    int launcher_list_view_height() const
    {
        const Page* launcher = page("launcher");
        int fields_height = 52;
        if (launcher != nullptr) {
            for (const auto& field : launcher->fields) {
                fields_height += field.kind == FieldKind::Text && field.key.find("menu_") == 0 ? 52 : 44;
            }
        }
        const int available = std::max(34, content_height() - fields_height - 100);
        const int content = std::max(34, launcher_list_content_extent());
        return std::min(available, content);
    }

    bool has_page_scrollbar() const
    {
        static const std::array<std::string, 11> pages = {
            "panel", "tasks", "launcher", "desktop_hub", "menu", "clock", "battery",
            "notifications", "buttons", "executer", "backgrounds"};
        return std::find(pages.begin(), pages.end(), current_page_) != pages.end();
    }

    int page_content_extent() const
    {
        if (current_page_ == "buttons") {
            return std::max(62 + 14 * 39, 96 + static_cast<int>(model_.buttons.size()) * 36);
        }
        if (current_page_ == "executer") {
            return std::max(62 + 3 * 39, 96 + static_cast<int>(model_.executers.size()) * 36);
        }
        if (current_page_ == "backgrounds") {
            return std::max(62 + 7 * 42 + 34,
                            96 + static_cast<int>(model_.backgrounds.size()) * 36);
        }
        if (current_page_ == "desktop_hub") {
            return 334 + desktop_hub_folder_list_view_height();
        }
        const Page* current = page(current_page_);
        if (current == nullptr) return 0;
        int extent = 52;
        for (const auto& field : current->fields) {
            extent += field.kind == FieldKind::Text && field.key.find("menu_") == 0 ? 52 : 44;
        }
        if (current_page_ == "launcher") {
            extent += 100 + launcher_list_view_height();
        } else if (current_page_ == "menu") {
            extent += 354;
        } else if (current_page_ == "notifications") {
            extent += 26;
        }
        return extent;
    }

    int scrollbar_content_extent(const std::string& key) const
    {
        if (key == "sidebar") {
            return pages_.empty() ? 0 : static_cast<int>(pages_.size() - 1) * 32 + 28;
        }
        if (key == "raw") {
            const std::string text = model_.dump();
            return static_cast<int>(std::count(text.begin(), text.end(), '\n')) * 17;
        }
        if (key == "page") return page_content_extent();
        if (key == "launcher") return launcher_list_content_extent();
        if (key == "desktop_hub_folders") return desktop_hub_folder_content_extent();
        if (key == "menu_places") return menu_entry_content_extent(0);
        if (key == "menu_system") return menu_entry_content_extent(1);
        if (key == "items") {
            const auto items = model_.panel_items();
            return items.empty() ? 0 :
                static_cast<int>(items.size() - 1) * item_row_pitch + item_row_height;
        }
        const int count = static_cast<int>(ConfigEditorModel::item_catalog().size());
        return count == 0 ? 0 : (count - 1) * 42 + 38;
    }

    int scrollbar_max_scroll(const std::string& key) const
    {
        const Rect& track = scrollbar_track(key);
        return std::max(0, scrollbar_content_extent(key) - track.height);
    }

    int scrollbar_thumb_height(const Rect& track, int content_extent) const
    {
        if (content_extent <= track.height || track.height <= 0) return track.height;
        return std::min(track.height, std::max(24, track.height * track.height / content_extent));
    }

    int scrollbar_thumb_y(const Rect& track, int thumb_height, int maximum) const
    {
        const int travel = track.height - thumb_height;
        const int scroll = dragging_scrollbar_ == "sidebar" ? sidebar_scroll_ :
            dragging_scrollbar_ == "raw" ? raw_scroll_ :
            dragging_scrollbar_ == "page" ? content_scroll_ :
            dragging_scrollbar_ == "launcher" ? launcher_scroll_ :
            dragging_scrollbar_ == "desktop_hub_folders" ? desktop_hub_folder_scroll_ :
            dragging_scrollbar_ == "menu_places" ? menu_entry_scroll_[0] :
            dragging_scrollbar_ == "menu_system" ? menu_entry_scroll_[1] :
            dragging_scrollbar_ == "items" ? item_scroll_ : plugin_scroll_;
        const int offset = maximum > 0 ? travel * scroll / maximum : 0;
        return track.y + offset;
    }

    void set_scrollbar_position(const std::string& key, int pointer_y)
    {
        const Rect& track = scrollbar_track(key);
        const int maximum = scrollbar_max_scroll(key);
        const int thumb_height = scrollbar_thumb_height(track, scrollbar_content_extent(key));
        const int travel = track.height - thumb_height;
        const int offset = std::clamp(pointer_y - track.y - scrollbar_drag_offset_, 0, travel);
        const int scroll = travel > 0 ? maximum * offset / travel : 0;
        if (key == "sidebar") sidebar_scroll_ = scroll;
        else if (key == "raw") raw_scroll_ = scroll;
        else if (key == "page") content_scroll_ = scroll;
        else if (key == "launcher") launcher_scroll_ = scroll;
        else if (key == "desktop_hub_folders") desktop_hub_folder_scroll_ = scroll;
        else if (key == "menu_places") menu_entry_scroll_[0] = scroll;
        else if (key == "menu_system") menu_entry_scroll_[1] = scroll;
        else if (key == "items") item_scroll_ = scroll;
        else plugin_scroll_ = scroll;
    }

    void draw_scrollbar(const Rect& track, int content_extent, int scroll, int maximum)
    {
        if (maximum <= 0 || track.height <= 0) return;
        rounded_rect(cr_, track, scrollbar_width / 2.0);
        cairo_set_source_rgb(cr_, 0.90, 0.91, 0.93);
        cairo_fill(cr_);
        const int thumb_height = scrollbar_thumb_height(track, content_extent);
        const int travel = track.height - thumb_height;
        const int thumb_y = track.y + (maximum > 0 ? travel * scroll / maximum : 0);
        const Rect thumb{track.x, thumb_y, track.width, thumb_height};
        rounded_rect(cr_, thumb, scrollbar_width / 2.0);
        cairo_set_source_rgb(cr_, 0.56, 0.59, 0.63);
        cairo_fill(cr_);
    }

    void draw_button(const std::string& label, int x, int y, int w, int h,
                     const std::string& action, bool primary = false,
                     const std::string& key = {}, int index = -1)
    {
        const Rect rect{x, y, w, h};
        rounded_rect(cr_, rect, 6);
        if (primary) cairo_set_source_rgb(cr_, 0.16, 0.42, 0.76);
        else cairo_set_source_rgb(cr_, 0.93, 0.94, 0.95);
        cairo_fill_preserve(cr_);
        cairo_set_source_rgb(cr_, primary ? 0.16 : 0.82, primary ? 0.42 : 0.83,
                             primary ? 0.76 : 0.85);
        cairo_set_line_width(cr_, 1);
        cairo_stroke(cr_);
        const int label_width = text_width(cr_, label, "Sans 9");
        draw_text(cr_, label, x + (w - label_width) / 2, y + (h - 14) / 2,
                  w - 12, primary ? 1.0 : 0.13, primary ? 1.0 : 0.15,
                  primary ? 1.0 : 0.17, "Sans 9");
        add_hit(rect, action, key, index);
    }

    void draw_header()
    {
        cairo_set_source_rgb(cr_, 1.0, 1.0, 1.0);
        cairo_rectangle(cr_, 0, 0, width_, header_height);
        cairo_fill(cr_);
        cairo_set_source_rgb(cr_, 0.86, 0.88, 0.90);
        cairo_move_to(cr_, 0, header_height - 0.5);
        cairo_line_to(cr_, width_, header_height - 0.5);
        cairo_stroke(cr_);
        draw_text(cr_, "mdeon-panel settings", 20, 18, 260, 0.12, 0.14, 0.16, "Sans Bold 14");
        draw_text(cr_, model_.path.string(), 20, 38, 300, 0.38, 0.40, 0.43, "Sans 8");
        int right = width_ - 14;
        draw_button("Close", right - 70, 13, 70, 32, "close"); right -= 78;
        draw_button("Apply", right - 70, 13, 70, 32, "save", true); right -= 78;
        draw_button("Open file", right - 78, 13, 78, 32, "open"); right -= 86;
        draw_button("Reload", right - 70, 13, 70, 32, "reload");
    }

    void draw_sidebar()
    {
        cairo_set_source_rgb(cr_, 0.97, 0.975, 0.98);
        cairo_rectangle(cr_, 0, header_height, sidebar_width, height_ - header_height - footer_height);
        cairo_fill(cr_);
        cairo_set_source_rgb(cr_, 0.87, 0.89, 0.91);
        cairo_move_to(cr_, sidebar_width - 0.5, header_height);
        cairo_line_to(cr_, sidebar_width - 0.5, height_ - footer_height);
        cairo_stroke(cr_);
        cairo_save(cr_);
        cairo_rectangle(cr_, 0, header_height, sidebar_width, height_ - header_height - footer_height);
        cairo_clip(cr_);
        sidebar_scrollbar_track_ = {
            sidebar_width - scrollbar_width - 4,
            header_height + 8,
            scrollbar_width,
            std::max(1, height_ - footer_height - header_height - 8)};
        const int sidebar_extent = scrollbar_content_extent("sidebar");
        const int sidebar_max_scroll = scrollbar_max_scroll("sidebar");
        sidebar_scroll_ = std::clamp(sidebar_scroll_, 0, sidebar_max_scroll);
        for (std::size_t i = 0; i < pages_.size(); ++i) {
            const int y = header_height + 8 + static_cast<int>(i) * 32 - sidebar_scroll_;
            if (y + 28 <= header_height + 8 || y >= height_ - footer_height) continue;
            const bool selected = pages_[i].id == current_page_;
            const Rect rect{8, y, sidebar_scrollbar_track_.x - 12, 28};
            if (selected) {
                rounded_rect(cr_, rect, 5);
                cairo_set_source_rgb(cr_, 0.84, 0.90, 0.98);
                cairo_fill(cr_);
            }
            draw_text(cr_, pages_[i].title, 18, y + 7, sidebar_width - 36,
                      selected ? 0.10 : 0.19, selected ? 0.28 : 0.21,
                      selected ? 0.52 : 0.24, selected ? "Sans Bold 9" : "Sans 9");
            add_hit({rect.x, std::max(rect.y, header_height + 8), rect.width,
                     std::min(rect.y + rect.height, height_ - footer_height) -
                         std::max(rect.y, header_height + 8)},
                    "page", pages_[i].id);
        }
        if (sidebar_max_scroll > 0) {
            add_hit(sidebar_scrollbar_track_, "sidebar-scrollbar", "sidebar");
        }
        cairo_restore(cr_);
        draw_scrollbar(sidebar_scrollbar_track_, sidebar_extent, sidebar_scroll_, sidebar_max_scroll);
    }

    void draw_footer()
    {
        cairo_set_source_rgb(cr_, 1, 1, 1);
        cairo_rectangle(cr_, 0, height_ - footer_height, width_, footer_height);
        cairo_fill(cr_);
        cairo_set_source_rgb(cr_, 0.86, 0.88, 0.90);
        cairo_move_to(cr_, 0, height_ - footer_height + 0.5);
        cairo_line_to(cr_, width_, height_ - footer_height + 0.5);
        cairo_stroke(cr_);
        draw_text(cr_, status_, 16, height_ - 21, width_ - 32, 0.34, 0.36, 0.39, "Sans 9");
    }

    void section_title(const std::string& title, int x, int y)
    {
        draw_text(cr_, title, x, y, width_ - x - 24, 0.12, 0.14, 0.16, "Sans Bold 14");
    }

    void draw_content()
    {
        const int x = sidebar_width + 1;
        const int full_content_width = width_ - x;
        const bool page_scrollable = has_page_scrollbar();
        page_scrollbar_track_ = {
            width_ - scrollbar_width - 8,
            content_top(),
            scrollbar_width,
            std::max(1, content_height())};
        const int page_max_scroll = page_scrollable ? scrollbar_max_scroll("page") : 0;
        content_scroll_ = std::clamp(content_scroll_, 0, page_max_scroll);
        const int content_width = full_content_width - (page_scrollable ? scrollbar_width + 16 : 0);
        cairo_save(cr_);
        cairo_rectangle(cr_, x, header_height, full_content_width, height_ - header_height - footer_height);
        cairo_clip(cr_);
        const int y = content_top() - content_scroll_;
        if (current_page_ == "items") draw_items(x, y, content_width);
        else if (current_page_ == "buttons") draw_buttons(x, y, content_width);
        else if (current_page_ == "executer") draw_executers(x, y, content_width);
        else if (current_page_ == "backgrounds") draw_backgrounds(x, y, content_width);
        else if (current_page_ == "raw") draw_raw(x, y, content_width);
        else draw_fields(x, y, content_width);
        cairo_restore(cr_);
        if (page_scrollable && page_max_scroll > 0) {
            add_hit(page_scrollbar_track_, "page-scrollbar", "page");
            draw_scrollbar(page_scrollbar_track_, page_content_extent(), content_scroll_, page_max_scroll);
        }
    }

    void draw_items(int x, int y, int content_width)
    {
        section_title("Panel items (left to right)", x + 24, y + 8);
        draw_text(cr_, "Select modules, then reorder the current list.", x + 24, y + 34,
                  content_width - 48, 0.40, 0.42, 0.45, "Sans 9");
        const int gap = 16;
        const int col_width = (content_width - 64 - gap) / 2;
        const int left_x = x + 24;
        const int right_x = left_x + col_width + gap;
        const int list_top = y + 78;
        const int list_height = std::max(160, content_height() - 150);
        const int rows_top = list_top + 20;
        const int rows_height = std::max(1, list_height - 20);
        const int rows_bottom = rows_top + rows_height;
        const int list_width = std::max(1, col_width - scrollbar_width - 6);
        item_scrollbar_track_ = {left_x + list_width + 4, rows_top, scrollbar_width, rows_height};
        plugin_scrollbar_track_ = {right_x + list_width + 4, rows_top, scrollbar_width, rows_height};
        draw_text(cr_, "Current items", left_x, list_top, col_width, 0.18, 0.20, 0.23, "Sans Bold 10");
        draw_text(cr_, "Add module", right_x, list_top, col_width, 0.18, 0.20, 0.23, "Sans Bold 10");
        const auto items = model_.panel_items();
        const int item_extent = items.empty() ? 0 :
            static_cast<int>(items.size() - 1) * item_row_pitch + item_row_height;
        const int item_max_scroll = std::max(0, item_extent - rows_height);
        item_scroll_ = std::clamp(item_scroll_, 0, item_max_scroll);
        const int plugin_extent = ConfigEditorModel::item_catalog().empty() ? 0 :
            static_cast<int>(ConfigEditorModel::item_catalog().size() - 1) * 42 + 38;
        const int plugin_max_scroll = std::max(0, plugin_extent - rows_height);
        plugin_scroll_ = std::clamp(plugin_scroll_, 0, plugin_max_scroll);
        cairo_save(cr_);
        cairo_rectangle(cr_, left_x, rows_top, list_width, rows_height);
        cairo_rectangle(cr_, right_x, rows_top, list_width, rows_height);
        cairo_clip(cr_);
        for (std::size_t i = 0; i < items.size(); ++i) {
            const int row_y = rows_top + static_cast<int>(i) * item_row_pitch - item_scroll_;
            if (row_y + item_row_height <= rows_top || row_y >= rows_bottom) continue;
            const auto found = ConfigEditorModel::item_catalog().find(items[i]);
            const std::string title = found == ConfigEditorModel::item_catalog().end()
                ? std::string(1, items[i]) : found->second.first;
            std::string label = std::string(1, items[i]) + "   " + title;
            if (items[i] == 'P') {
                const int number = static_cast<int>(std::count(items.begin(), items.begin() + static_cast<long>(i + 1), 'P'));
                label += "  (button #" + std::to_string(number) + ")";
            }
            const Rect row{left_x, row_y, list_width, item_row_height};
            draw_list_row(row, label, selected_item_ == static_cast<int>(i));
            add_hit({row.x, std::max(row.y, rows_top), row.width,
                     std::min(row.y + row.height, rows_bottom) - std::max(row.y, rows_top)},
                    "item-select", {}, static_cast<int>(i));
        }
        std::size_t plugin_index = 0;
        for (const auto& [code, data] : ConfigEditorModel::item_catalog()) {
            const int row_y = rows_top + static_cast<int>(plugin_index) * 42 - plugin_scroll_;
            if (row_y + 38 <= rows_top || row_y >= rows_bottom) { ++plugin_index; continue; }
            const Rect row{right_x, row_y, list_width, 38};
            draw_list_row(row, std::string(1, code) + "  " + data.first,
                          selected_plugin_ == static_cast<int>(plugin_index));
            draw_text(cr_, data.second, right_x + 10, row_y + 20, list_width - 20,
                      0.42, 0.44, 0.47, "Sans 8");
            add_hit({row.x, std::max(row.y, rows_top), row.width,
                     std::min(row.y + row.height, rows_bottom) - std::max(row.y, rows_top)},
                    "plugin-select", {}, static_cast<int>(plugin_index));
            ++plugin_index;
        }
                if (item_max_scroll > 0) add_hit(item_scrollbar_track_, "item-scrollbar", "items");
                if (plugin_max_scroll > 0) add_hit(plugin_scrollbar_track_, "plugin-scrollbar", "plugins");
        cairo_restore(cr_);
                draw_scrollbar(item_scrollbar_track_, item_extent, item_scroll_, item_max_scroll);
                draw_scrollbar(plugin_scrollbar_track_, plugin_extent, plugin_scroll_, plugin_max_scroll);
        int controls_y = list_top + list_height + 8;
        const int button_gap = 4;
        const int button_width = std::max(1, (col_width - button_gap * 3) / 4);
        draw_button("Up", left_x, controls_y, button_width, 30, "item-up");
        draw_button("Down", left_x + button_width + button_gap, controls_y, button_width, 30, "item-down");
        draw_button(button_width < 60 ? "Del" : "Remove",
                left_x + (button_width + button_gap) * 2, controls_y,
                    button_width, 30, "item-remove");
        draw_button(button_width < 72 ? "Config" : "Configure",
                left_x + (button_width + button_gap) * 3, controls_y,
                    col_width - (button_width + button_gap) * 3, 30, "item-configure");
        draw_button("Add selected", right_x, controls_y, col_width, 30, "item-add", true);
        draw_text(cr_, "panel_items = " + items_to_string(items), left_x, controls_y + 44,
                  content_width - 48, 0.32, 0.34, 0.37, "Monospace 9");
    }

    static std::string items_to_string(const std::vector<char>& items)
    {
        std::string result;
        for (const char item : items) result.push_back(item);
        return result;
    }

    void draw_list_row(const Rect& row, const std::string& label, bool selected)
    {
        rounded_rect(cr_, row, 5);
        cairo_set_source_rgb(cr_, selected ? 0.85 : 1.0, selected ? 0.91 : 1.0, selected ? 0.98 : 1.0);
        cairo_fill_preserve(cr_);
        cairo_set_source_rgb(cr_, 0.88, 0.89, 0.91);
        cairo_stroke(cr_);
        draw_text(cr_, label, row.x + 10, row.y + 7, row.width - 20,
                  0.16, 0.18, 0.21, selected ? "Sans Bold 9" : "Sans 9");
    }

    const Field* field_for(const Page& page_data, const std::string& key) const
    {
        const auto found = std::find_if(page_data.fields.begin(), page_data.fields.end(), [&key](const Field& field) {
            return field.key == key;
        });
        return found == page_data.fields.end() ? nullptr : &*found;
    }

    void draw_fields(int x, int y, int content_width)
    {
        const Page* current = page(current_page_);
        if (current == nullptr) return;
        section_title(current->title, x + 24, y + 8);
        int row_y = y + 52;
        const int label_width = std::min(250, content_width / 3);
        for (const auto& field : current->fields) {
            const int row_height_current = field.kind == FieldKind::Text && field.key.find("menu_") == 0 ? 52 : 44;
            if (row_y + row_height_current >= header_height && row_y <= height_ - footer_height) {
                draw_text(cr_, field.label, x + 28, row_y + 13, label_width - 16,
                          0.20, 0.22, 0.25, "Sans 9");
                draw_field_control(field, x + label_width + 28, row_y + 2,
                                   content_width - label_width - 56, row_height_current - 8,
                                   "value", -1);
            }
            row_y += row_height_current;
        }
        if (current_page_ == "menu") {
            const auto width_value = [this](const std::string& key, int fallback) {
                try { return std::stoi(model_.get(key, std::to_string(fallback))); }
                catch (...) { return fallback; }
            };
            const auto enabled_value = [this](const std::string& key) {
                const std::string value = model_.get(key, "1");
                return value == "1" || value == "true" || value == "yes" || value == "on";
            };
            int total_width = width_value("menu_applications_width", 434);
            if (enabled_value("menu_show_categories")) {
                total_width += width_value("menu_categories_width", 128);
            }
            if (enabled_value("menu_show_places")) {
                total_width += width_value("menu_places_width", 138);
            }
            draw_text(cr_, "Calculated popup width: " + std::to_string(total_width) + " px",
                x + 28, row_y + 2, content_width - 56, 0.30, 0.32, 0.35, "Sans 9");
            row_y += 24;
        }
        if (current_page_ == "launcher") draw_launcher_list(x, row_y, content_width);
        if (current_page_ == "desktop_hub") draw_desktop_hub_folders(x, row_y, content_width);
        if (current_page_ == "menu") draw_menu_entries(x, row_y, content_width);
        if (current_page_ == "notifications") {
            draw_text(cr_, "Notification history is available when this panel owns the desktop notification service.",
                x + 28, row_y + 16, content_width - 56, 0.40, 0.42, 0.45, "Sans 9");
        }
    }

    void draw_desktop_hub_folders(int x, int y, int content_width)
    {
        draw_text(cr_, "Folders shown on launch", x + 28, y + 14, content_width - 56,
                  0.18, 0.20, 0.23, "Sans Bold 10");
        const int list_width = std::max(1, content_width - 56 - scrollbar_width - 6);
        const int list_height = desktop_hub_folder_list_view_height();
        const int list_top = y + 28;
        desktop_hub_folder_list_view_ = {x + 28, list_top, list_width, list_height};
        desktop_hub_folder_scrollbar_track_ = {x + 28 + list_width + 4, list_top,
            scrollbar_width, list_height};
        const auto folders = desktop_hub_folders();
        const int extent = desktop_hub_folder_content_extent();
        const int maximum = scrollbar_max_scroll("desktop_hub_folders");
        desktop_hub_folder_scroll_ = std::clamp(desktop_hub_folder_scroll_, 0, maximum);
        cairo_save(cr_);
        cairo_rectangle(cr_, desktop_hub_folder_list_view_.x, list_top, list_width, list_height);
        cairo_clip(cr_);
        for (std::size_t i = 0; i < folders.size(); ++i) {
            const int row_y = list_top + static_cast<int>(i) * 36 - desktop_hub_folder_scroll_;
            if (row_y + 32 <= list_top || row_y >= list_top + list_height) continue;
            const Rect row{x + 28, row_y, list_width, 32};
            draw_list_row(row, folders[i].first + "    " + folders[i].second,
                          selected_desktop_hub_folder_ == static_cast<int>(i));
            add_hit({row.x, std::max(row.y, list_top), row.width,
                     std::min(row.y + row.height, list_top + list_height) - std::max(row.y, list_top)},
                    "desktop-hub-folder-select", {}, static_cast<int>(i));
        }
        cairo_restore(cr_);
        if (maximum > 0) {
            add_hit(desktop_hub_folder_scrollbar_track_, "desktop-hub-folder-scrollbar",
                    "desktop_hub_folders");
        }
        draw_scrollbar(desktop_hub_folder_scrollbar_track_, extent,
                       desktop_hub_folder_scroll_, maximum);

        const int label_width = std::min(110, content_width / 3);
        int field_y = list_top + list_height + 8;
        draw_text(cr_, "Label", x + 28, field_y + 10, label_width - 10,
                  0.20, 0.22, 0.25, "Sans 9");
        draw_field_control(text_field("desktop_hub_folder_label", "", desktop_hub_folder_label_draft_),
            x + 28 + label_width, field_y, content_width - 56 - label_width, 32,
            "desktop_hub_folder", -1);
        field_y += 38;
        draw_text(cr_, "Path or URI", x + 28, field_y + 10, label_width - 10,
                  0.20, 0.22, 0.25, "Sans 9");
        draw_field_control(text_field("desktop_hub_folder_path", "", desktop_hub_folder_path_draft_),
            x + 28 + label_width, field_y, content_width - 56 - label_width, 32,
            "desktop_hub_folder", -1);
        field_y += 40;
        draw_button("Add", x + 28, field_y, 58, 28, "desktop-hub-folder-add", true);
        draw_button("Update", x + 92, field_y, 70, 28, "desktop-hub-folder-update");
        draw_button("Remove", x + 168, field_y, 72, 28, "desktop-hub-folder-remove");
        draw_button("Up", x + 246, field_y, 46, 28, "desktop-hub-folder-up");
        draw_button("Down", x + 298, field_y, 56, 28, "desktop-hub-folder-down");
    }

    void draw_menu_entries(int x, int y, int content_width)
    {
        const int panel_gap = 14;
        const int panel_width = std::max(1, (content_width - 56 - panel_gap) / 2);
        const int list_height = menu_entry_list_view_height();
        const int panel_x[] = {x + 28, x + 28 + panel_width + panel_gap};
        const char* keys[] = {"menu_places", "menu_system"};
        const char* titles[] = {"Places", "System"};
        const std::string list_names[] = {"menu_places", "menu_system"};

        for (int list_index = 0; list_index < 2; ++list_index) {
            const std::size_t slot = static_cast<std::size_t>(list_index);
            const int left = panel_x[list_index];
            draw_text(cr_, titles[list_index], left, y + 8, panel_width - 72,
                      0.18, 0.20, 0.23, "Sans Bold 10");
            draw_button("Add", left + panel_width - 62, y + 2, 58, 26,
                        "menu-entry-add", true, keys[list_index]);

            const int list_top = y + 34;
            const int list_width = std::max(1, panel_width - scrollbar_width - 6);
            menu_entry_list_views_[slot] = {left, list_top, list_width, list_height};
            menu_entry_scrollbar_tracks_[slot] = {left + list_width + 4, list_top,
                scrollbar_width, list_height};
            const auto entries = menu_entries(list_index);
            const int extent = menu_entry_content_extent(list_index);
            const int maximum = scrollbar_max_scroll(list_names[slot]);
            menu_entry_scroll_[slot] = std::clamp(menu_entry_scroll_[slot], 0, maximum);
            cairo_save(cr_);
            cairo_rectangle(cr_, left, list_top, list_width, list_height);
            cairo_clip(cr_);
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const int row_y = list_top + static_cast<int>(i) * 34 - menu_entry_scroll_[slot];
                if (row_y + 30 <= list_top || row_y >= list_top + list_height) continue;
                const Rect row{left, row_y, list_width, 30};
                draw_list_row(row, entries[i].label + "    " + entries[i].target,
                    selected_menu_entry_[slot] == static_cast<int>(i));
                add_hit({row.x, std::max(row.y, list_top), row.width,
                         std::min(row.y + row.height, list_top + list_height) -
                             std::max(row.y, list_top)},
                        "menu-entry-select", list_names[slot], static_cast<int>(i));
            }
            cairo_restore(cr_);
            if (maximum > 0) {
                add_hit(menu_entry_scrollbar_tracks_[slot], "menu-entry-scrollbar",
                        list_names[slot]);
            }
            draw_scrollbar(menu_entry_scrollbar_tracks_[slot], extent,
                           menu_entry_scroll_[slot], maximum);

            const int label_width = std::min(48, panel_width / 4);
            int field_y = list_top + list_height + 5;
            draw_text(cr_, "Label", left, field_y + 10, label_width - 4,
                      0.20, 0.22, 0.25, "Sans 8");
            draw_field_control(text_field("menu_entry_label", "",
                menu_entry_draft_[slot][0]), left + label_width, field_y,
                panel_width - label_width, 30, keys[list_index], -1);
            field_y += 34;
            draw_text(cr_, "Icon", left, field_y + 10, label_width - 4,
                      0.20, 0.22, 0.25, "Sans 8");
            draw_field_control(text_field("menu_entry_icon", "",
                menu_entry_draft_[slot][1]), left + label_width, field_y,
                panel_width - label_width, 30, keys[list_index], -1);
            field_y += 34;
            draw_text(cr_, list_index == 0 ? "Folder" : "App", left,
                      field_y + 10, label_width - 4, 0.20, 0.22, 0.25, "Sans 8");
            const int browse_width = 58;
            draw_field_control(text_field("menu_entry_target", "",
                menu_entry_draft_[slot][2]), left + label_width, field_y,
                panel_width - label_width - browse_width - 4, 30,
                keys[list_index], -1);
            draw_button("Browse", left + panel_width - browse_width, field_y,
                browse_width, 30, "menu-entry-browse", false, keys[list_index]);
            field_y += 34;
            const int button_width = (panel_width - 16) / 5;
            const std::string actions[] = {"menu-entry-update", "menu-entry-remove",
                "menu-entry-up", "menu-entry-down"};
            const std::string labels[] = {"Update", "Remove", "Up", "Down"};
            for (int button = 0; button < 4; ++button) {
                draw_button(labels[button], left + button * (button_width + 4), field_y,
                    button_width, 27, actions[button], false, keys[list_index]);
            }
        }
    }

    void draw_field_control(const Field& field, int x, int y, int width, int height,
                            const std::string& target, int index)
    {
        std::string value = field_value(field.key, target, index, field.initial);
        if (field.kind == FieldKind::Toggle) {
            const bool active = value == "1" || value == "true" || value == "yes" || value == "on";
            const Rect switch_rect{x + width - 48, y + 2, 42, height - 4};
            rounded_rect(cr_, switch_rect, (height - 4) / 2.0);
            cairo_set_source_rgb(cr_, active ? 0.18 : 0.73, active ? 0.55 : 0.75, active ? 0.36 : 0.77);
            cairo_fill(cr_);
            cairo_arc(cr_, active ? switch_rect.x + switch_rect.width - 10 : switch_rect.x + 10,
                      switch_rect.y + switch_rect.height / 2.0, 7, 0, 2 * M_PI);
            cairo_set_source_rgb(cr_, 1, 1, 1);
            cairo_fill(cr_);
            add_hit({x, y, width, height}, "toggle", target + "|" + field.key, index);
            return;
        }
        const Rect rect{x, y, width, height};
        rounded_rect(cr_, rect, 5);
        cairo_set_source_rgb(cr_, edit_target_ == target + ":" + field.key ? 1.0 : 1.0,
                             edit_target_ == target + ":" + field.key ? 1.0 : 1.0, 1.0);
        cairo_fill_preserve(cr_);
        cairo_set_source_rgb(cr_, edit_target_ == target + ":" + field.key ? 0.25 : 0.80,
                             edit_target_ == target + ":" + field.key ? 0.48 : 0.82,
                             edit_target_ == target + ":" + field.key ? 0.78 : 0.84);
        cairo_stroke(cr_);
        if (field.kind == FieldKind::Number) {
            const bool editing = editing_number_ && edit_target_ == target + ":" + field.key;
            const std::string shown = editing ? number_edit_buffer_ : value;
            draw_text(cr_, "-", x + 10, y + 4, 20, 0.22, 0.24, 0.27, "Sans Bold 12");
            draw_text(cr_, "+", x + width - 25, y + 5, 20, 0.22, 0.24, 0.27, "Sans Bold 12");
            draw_text(cr_, shown, x + 32, y + 7, width - 64, 0.17, 0.19, 0.22,
                      editing ? "Monospace 9" : "Sans 9");
            if (editing) {
                const int cursor_x = std::min(x + width - 30,
                    x + 32 + text_width(cr_, shown, "Monospace 9"));
                cairo_set_source_rgb(cr_, 0.12, 0.32, 0.59);
                cairo_set_line_width(cr_, 1);
                cairo_move_to(cr_, cursor_x + 1, y + 6);
                cairo_line_to(cr_, cursor_x + 1, y + height - 6);
                cairo_stroke(cr_);
            }
            add_hit({x, y, 28, height}, "step", field.key, -1);
            add_hit({x + width - 28, y, 28, height}, "step", field.key, 1);
            add_hit({x + 28, y, width - 56, height}, "number-edit",
                    target + "|" + field.key, index);
        } else if (field.kind == FieldKind::Choice) {
            auto option = std::find_if(field.options.begin(), field.options.end(), [&value](const auto& item) {
                return item.first == value;
            });
            const std::string shown = option == field.options.end() ? value : option->second;
            draw_text(cr_, shown + "  v", x + 10, y + 7, width - 20, 0.17, 0.19, 0.22, "Sans 9");
            add_hit(rect, "cycle", field.key);
        } else if (field.kind == FieldKind::Color) {
            draw_swatch(value, x + 7, y + 7, 18, std::max(12, height - 14));
            const int picker_width = 56;
            const int picker_x = x + width - picker_width;
            const int input_width = std::max(1, picker_x - x - 4);
            draw_text(cr_, value.empty() ? " " : value, x + 34, y + 7,
                      input_width - 40, 0.17, 0.19, 0.22,
                      edit_target_ == target + ":" + field.key ? "Monospace 9" : "Sans 9");
            if (edit_target_ == target + ":" + field.key) {
                const int cursor_x = std::min(x + input_width - 10,
                    x + 34 + text_width(cr_, value, "Monospace 9"));
                cairo_set_source_rgb(cr_, 0.12, 0.32, 0.59);
                cairo_set_line_width(cr_, 1);
                cairo_move_to(cr_, cursor_x + 1, y + 6);
                cairo_line_to(cr_, cursor_x + 1, y + height - 6);
                cairo_stroke(cr_);
            }
            add_hit({x, y, input_width, height}, "edit", target + "|" + field.key, index);
            draw_button("Pick", picker_x, y + (height - 26) / 2, picker_width, 26,
                        "color-picker", false, target + "|" + field.key, index);
        } else {
            draw_text(cr_, value.empty() ? " " : value, x + 10, y + 7, width - 20,
                      0.17, 0.19, 0.22, edit_target_ == target + ":" + field.key ? "Monospace 9" : "Sans 9");
            if (edit_target_ == target + ":" + field.key) {
                const int cursor_x = std::min(x + width - 10, x + 10 + text_width(cr_, value, "Monospace 9"));
                cairo_set_source_rgb(cr_, 0.12, 0.32, 0.59);
                cairo_set_line_width(cr_, 1);
                cairo_move_to(cr_, cursor_x + 1, y + 6);
                cairo_line_to(cr_, cursor_x + 1, y + height - 6);
                cairo_stroke(cr_);
            }
            add_hit(rect, "edit", target + "|" + field.key, index);
        }
    }

    void draw_swatch(const std::string& value, int x, int y, int w, int h)
    {
        unsigned int red = 255, green = 255, blue = 255;
        if (value.size() >= 7 && value[0] == '#') {
            std::sscanf(value.c_str() + 1, "%2x%2x%2x", &red, &green, &blue);
        }
        rounded_rect(cr_, {x, y, w, h}, 3);
        cairo_set_source_rgb(cr_, red / 255.0, green / 255.0, blue / 255.0);
        cairo_fill_preserve(cr_);
        cairo_set_source_rgb(cr_, 0.72, 0.73, 0.75);
        cairo_stroke(cr_);
    }

    void draw_launcher_list(int x, int y, int content_width)
    {
        draw_text(cr_, "Applications (.desktop)", x + 28, y + 14, content_width - 56,
                  0.18, 0.20, 0.23, "Sans Bold 10");
        const int list_width = std::max(1, content_width - 56 - scrollbar_width - 6);
        const int list_height = launcher_list_view_height();
        const int list_top = y + 28;
        launcher_list_view_ = {x + 28, list_top, list_width, list_height};
        launcher_scrollbar_track_ = {x + 28 + list_width + 4, list_top,
                                     scrollbar_width, list_height};
        const int content_extent = launcher_list_content_extent();
        const int maximum = scrollbar_max_scroll("launcher");
        launcher_scroll_ = std::clamp(launcher_scroll_, 0, maximum);
        cairo_save(cr_);
        cairo_rectangle(cr_, launcher_list_view_.x, launcher_list_view_.y,
                        launcher_list_view_.width, launcher_list_view_.height);
        cairo_clip(cr_);
        for (std::size_t i = 0; i < model_.launcher_apps.size(); ++i) {
            const int row_y = list_top + static_cast<int>(i) * 34 - launcher_scroll_;
            if (row_y + 30 <= list_top || row_y >= list_top + list_height) continue;
            const Rect row{x + 28, row_y, list_width, 30};
            draw_list_row(row, model_.launcher_apps[i],
                          selected_launcher_ == static_cast<int>(i));
            add_hit({row.x, std::max(row.y, list_top), row.width,
                     std::min(row.y + row.height, list_top + list_height) - std::max(row.y, list_top)},
                    "launcher-select", {}, static_cast<int>(i));
        }
        cairo_restore(cr_);
        if (maximum > 0) add_hit(launcher_scrollbar_track_, "launcher-scrollbar", "launcher");
        draw_scrollbar(launcher_scrollbar_track_, content_extent, launcher_scroll_, maximum);
        const int row_y = list_top + list_height;
        draw_field_control(text_field("launcher_item_app", "", launcher_draft_),
                           x + 28, row_y + 4, content_width - 56, 32, "launcher", -1);
        draw_button("Add", x + 28, row_y + 44, 58, 28, "launcher-add", true);
        draw_button("Remove", x + 92, row_y + 44, 76, 28, "launcher-remove");
        draw_button("Up", x + 174, row_y + 44, 48, 28, "launcher-up");
        draw_button("Down", x + 228, row_y + 44, 58, 28, "launcher-down");
    }

    void draw_buttons(int x, int y, int content_width)
    {
        section_title("Custom buttons (P)", x + 24, y + 8);
        draw_text(cr_, "Each P uses one definition in order.", x + 24, y + 32,
                  content_width - 48, 0.40, 0.42, 0.45, "Sans 9");
        const int left_width = 196;
        const int left_x = x + 24;
        int row_y = y + 62;
        for (std::size_t i = 0; i < model_.buttons.size(); ++i) {
            const auto text = model_.buttons[i].find("button_text");
            const std::string label = std::to_string(i + 1) + ". " +
                (text == model_.buttons[i].end() ? "Button" : text->second);
            draw_list_row({left_x, row_y, left_width, 32}, label, selected_button_ == static_cast<int>(i));
            add_hit({left_x, row_y, left_width, 32}, "button-select", {}, static_cast<int>(i));
            row_y += 36;
        }
        draw_button("Add", left_x, row_y + 4, 58, 30, "button-add");
        draw_button("Remove", left_x + 64, row_y + 4, 76, 30, "button-remove");
        const int right_x = left_x + left_width + 18;
        const int right_width = content_width - (right_x - x) - 24;
        int field_y = y + 62;
        const std::vector<Field> fields = {
            text_field("button_text", "Text"), text_field("button_tooltip", "Tooltip"),
            text_field("button_icon", "Icon file"), number_field("button_max_icon_size", "Max icon (0=auto)", 0, 0, 128),
            text_field("button_lclick_command", "Left click cmd"), text_field("button_rclick_command", "Right click cmd"),
            text_field("button_mclick_command", "Middle click cmd"), text_field("button_uwheel_command", "Scroll up cmd"),
            text_field("button_dwheel_command", "Scroll down cmd"), color_field("button_font_color", "Font color"),
            text_field("button_padding", "Padding"), number_field("button_background_id", "Background id", 0, 0, 20),
            toggle_field("button_centered", "Center content", false), text_field("button_font", "Font")};
        for (const auto& field : fields) {
            if (field_y + 38 < header_height || field_y > height_ - footer_height) { field_y += 39; continue; }
            draw_text(cr_, field.label, right_x, field_y + 10, std::min(160, right_width / 3),
                      0.20, 0.22, 0.25, "Sans 9");
            draw_field_control(field, right_x + std::min(165, right_width / 3 + 4), field_y,
                right_width - std::min(165, right_width / 3 + 4), 34, "button", selected_button_);
            field_y += 39;
        }
    }

    void draw_executers(int x, int y, int content_width)
    {
        section_title("Executor items (E)", x + 24, y + 8);
        const int left_width = 196;
        const int left_x = x + 24;
        int row_y = y + 62;
        for (std::size_t i = 0; i < model_.executers.size(); ++i) {
            const auto text = model_.executers[i].find("executer_text");
            const std::string label = std::to_string(i + 1) + ". " +
                (text == model_.executers[i].end() || text->second.empty() ? "Executor" : text->second);
            draw_list_row({left_x, row_y, left_width, 32}, label,
                          selected_executer_ == static_cast<int>(i));
            add_hit({left_x, row_y, left_width, 32}, "executer-select", {}, static_cast<int>(i));
            row_y += 36;
        }
        draw_button("Add", left_x, row_y + 4, 58, 30, "executer-add");
        draw_button("Remove", left_x + 64, row_y + 4, 76, 30, "executer-remove");
        const int right_x = left_x + left_width + 18;
        const int right_width = content_width - (right_x - x) - 24;
        int field_y = y + 62;
        const std::vector<Field> fields = {
            text_field("executer_text", "Button text"),
            text_field("executer_tooltip", "Tooltip", "Executor"),
            text_field("executer_command", "Script or program")};
        for (const auto& field : fields) {
            if (field_y + 38 < header_height || field_y > height_ - footer_height) { field_y += 39; continue; }
            draw_text(cr_, field.label, right_x, field_y + 10, std::min(160, right_width / 3),
                      0.20, 0.22, 0.25, "Sans 9");
            draw_field_control(field, right_x + std::min(165, right_width / 3 + 4), field_y,
                right_width - std::min(165, right_width / 3 + 4), 34, "executer", selected_executer_);
            field_y += 39;
        }
    }

    void draw_backgrounds(int x, int y, int content_width)
    {
        section_title("Backgrounds", x + 24, y + 8);
        draw_text(cr_, "Colors use #RRGGBB and optional opacity (0-100).", x + 24, y + 32,
                  content_width - 48, 0.40, 0.42, 0.45, "Sans 9");
        const int left_x = x + 24;
        const int left_width = 150;
        int row_y = y + 62;
        for (const auto& [number, props] : model_.backgrounds) {
            (void)props;
            draw_list_row({left_x, row_y, left_width, 32}, "Background " + std::to_string(number),
                          selected_background_ == number);
            add_hit({left_x, row_y, left_width, 32}, "background-select", {}, number);
            row_y += 36;
        }
        draw_button("Add", left_x, row_y + 4, 58, 30, "background-add");
        draw_button("Remove", left_x + 64, row_y + 4, 76, 30, "background-remove");
        const int right_x = left_x + left_width + 20;
        const int right_width = content_width - (right_x - x) - 24;
        int field_y = y + 62;
        const std::vector<Field> fields = {
            number_field("rounded", "Rounded", 0, 0, 50),
            number_field("border_width", "Border width", 0, 0, 20),
            color_field("background_color", "Background color"), color_field("border_color", "Border color"),
            color_field("background_color_hover", "Hover background"), color_field("border_color_hover", "Hover border"),
            color_field("background_color_pressed", "Pressed background"), color_field("border_color_pressed", "Pressed border")};
        for (const auto& field : fields) {
            draw_text(cr_, field.label, right_x, field_y + 10, 170, 0.20, 0.22, 0.25, "Sans 9");
            draw_field_control(field, right_x + 174, field_y, right_width - 174, 34,
                               "background", selected_background_);
            field_y += 42;
        }
    }

    void draw_raw(int x, int y, int content_width)
    {
        section_title("Raw configuration preview", x + 24, y + 8);
        draw_text(cr_, "Preview of the file that Apply writes.", x + 24, y + 32,
                  content_width - 48, 0.40, 0.42, 0.45, "Sans 9");
        raw_view_ = {x + 24, y + 54, content_width - 48, content_height() - 72};
        draw_raw_view_contents(raw_view_);
        draw_button("Refresh preview", x + 24, raw_view_.y + raw_view_.height + 8, 118, 30, "raw-refresh");
        draw_button("Open in text editor", x + 150, raw_view_.y + raw_view_.height + 8, 132, 30, "open");
    }

    void draw_raw_view_contents(const Rect& view)
    {
        rounded_rect(cr_, view, 5);
        cairo_set_source_rgb(cr_, 1, 1, 1);
        cairo_fill_preserve(cr_);
        cairo_set_source_rgb(cr_, 0.85, 0.86, 0.88);
        cairo_stroke(cr_);
        raw_scrollbar_track_ = {view.x + view.width - 16, view.y + 6, scrollbar_width,
                                std::max(1, view.height - 12)};
        const int raw_extent = scrollbar_content_extent("raw");
        const int raw_max_scroll = scrollbar_max_scroll("raw");
        raw_scroll_ = std::clamp(raw_scroll_, 0, raw_max_scroll);
        cairo_save(cr_);
        cairo_rectangle(cr_, view.x + 8, view.y + 6, view.width - 28, view.height - 12);
        cairo_clip(cr_);
        std::istringstream input(model_.dump());
        std::string line;
        int index = 0;
        while (std::getline(input, line)) {
            const int line_y = view.y + 8 + index * 17 - raw_scroll_;
            if (line_y >= view.y - 20 && line_y <= view.y + view.height) {
                draw_text(cr_, line, view.x + 8, line_y, view.width - 28,
                    line.rfind("#", 0) == 0 ? 0.42 : 0.12,
                    line.rfind("#", 0) == 0 ? 0.44 : 0.14,
                    line.rfind("#", 0) == 0 ? 0.47 : 0.16, "Monospace 8");
            }
            ++index;
        }
        cairo_restore(cr_);
        add_hit(view, "raw-view");
        if (raw_max_scroll > 0) add_hit(raw_scrollbar_track_, "raw-scrollbar", "raw");
        draw_scrollbar(raw_scrollbar_track_, raw_extent, raw_scroll_, raw_max_scroll);
    }

    std::string field_value(const std::string& key, const std::string& target,
                            int index, const std::string& fallback) const
    {
        if (target == "value") return model_.get(key, fallback);
        if (target == "button") {
            if (index < 0 || static_cast<std::size_t>(index) >= model_.buttons.size()) return fallback;
            const auto found = model_.buttons[static_cast<std::size_t>(index)].find(key);
            return found == model_.buttons[static_cast<std::size_t>(index)].end() ? fallback : found->second;
        }
        if (target == "executer") {
            if (index < 0 || static_cast<std::size_t>(index) >= model_.executers.size()) return fallback;
            const auto found = model_.executers[static_cast<std::size_t>(index)].find(key);
            return found == model_.executers[static_cast<std::size_t>(index)].end() ? fallback : found->second;
        }
        if (target == "background") {
            const auto background = model_.backgrounds.find(index);
            if (background == model_.backgrounds.end()) return fallback;
            const auto found = background->second.find(key);
            return found == background->second.end() ? fallback : found->second;
        }
        if (target == "launcher") return launcher_draft_;
        if (target == "desktop_hub_folder") {
            return key == "desktop_hub_folder_label"
                ? desktop_hub_folder_label_draft_ : desktop_hub_folder_path_draft_;
        }
        if (target == "menu_places" || target == "menu_system") {
            const std::size_t list_index = target == "menu_places" ? 0 : 1;
            if (key == "menu_entry_label") return menu_entry_draft_[list_index][0];
            if (key == "menu_entry_icon") return menu_entry_draft_[list_index][1];
            if (key == "menu_entry_target") return menu_entry_draft_[list_index][2];
        }
        return fallback;
    }

    void set_field_value(const std::string& target, const std::string& key,
                         int index, std::string value)
    {
        if (target == "value") model_.set(key, std::move(value));
        else if (target == "button" && index >= 0 && static_cast<std::size_t>(index) < model_.buttons.size()) {
            model_.buttons[static_cast<std::size_t>(index)][key] = std::move(value);
            model_.dirty = true;
        } else if (target == "executer" && index >= 0 && static_cast<std::size_t>(index) < model_.executers.size()) {
            model_.executers[static_cast<std::size_t>(index)][key] = std::move(value);
            model_.dirty = true;
        } else if (target == "background" && index > 0) {
            model_.backgrounds[index][key] = std::move(value);
            model_.dirty = true;
        } else if (target == "launcher") launcher_draft_ = std::move(value);
        else if (target == "desktop_hub_folder") {
            if (key == "desktop_hub_folder_label") desktop_hub_folder_label_draft_ = std::move(value);
            else if (key == "desktop_hub_folder_path") desktop_hub_folder_path_draft_ = std::move(value);
        } else if (target == "menu_places" || target == "menu_system") {
            const std::size_t list_index = target == "menu_places" ? 0 : 1;
            if (key == "menu_entry_label") menu_entry_draft_[list_index][0] = std::move(value);
            else if (key == "menu_entry_icon") menu_entry_draft_[list_index][1] = std::move(value);
            else if (key == "menu_entry_target") menu_entry_draft_[list_index][2] = std::move(value);
        }
    }

    void sync_menu_entry_draft(int list_index)
    {
        const auto entries = menu_entries(list_index);
        const int selected = selected_menu_entry_[static_cast<std::size_t>(list_index)];
        auto& draft = menu_entry_draft_[static_cast<std::size_t>(list_index)];
        if (selected < 0 || selected >= static_cast<int>(entries.size())) {
            draft = {};
            return;
        }
        const auto& entry = entries[static_cast<std::size_t>(selected)];
        draft = {entry.label, entry.icon, entry.target};
    }

    void add_menu_entry(int list_index)
    {
        auto& draft = menu_entry_draft_[static_cast<std::size_t>(list_index)];
        MenuEntry entry{ConfigEditorModel::trim(draft[0]), ConfigEditorModel::trim(draft[1]),
                        ConfigEditorModel::trim(draft[2])};
        if (entry.label.empty() || entry.target.empty()) return;
        if (entry.icon.empty()) entry.icon = "application-x-executable-symbolic";
        auto entries = menu_entries(list_index);
        entries.push_back(std::move(entry));
        const std::string key = list_index == 0 ? "menu_places" : "menu_system";
        model_.set(key, serialize_menu_entries(entries));
        selected_menu_entry_[static_cast<std::size_t>(list_index)] =
            static_cast<int>(entries.size()) - 1;
        sync_menu_entry_draft(list_index);
        menu_entry_scroll_[static_cast<std::size_t>(list_index)] =
            scrollbar_max_scroll(key);
    }

    void update_menu_entry(int list_index)
    {
        auto entries = menu_entries(list_index);
        const std::size_t slot = static_cast<std::size_t>(list_index);
        const int selected = selected_menu_entry_[slot];
        if (selected < 0 || selected >= static_cast<int>(entries.size()) ||
            menu_entry_draft_[slot][0].empty() || menu_entry_draft_[slot][2].empty()) return;
        entries[static_cast<std::size_t>(selected)] = {
                ConfigEditorModel::trim(menu_entry_draft_[slot][0]),
                ConfigEditorModel::trim(menu_entry_draft_[slot][1]),
                ConfigEditorModel::trim(menu_entry_draft_[slot][2])};
        model_.set(slot == 0 ? "menu_places" : "menu_system",
                   serialize_menu_entries(entries));
        sync_menu_entry_draft(list_index);
    }

    void remove_menu_entry(int list_index)
    {
        auto entries = menu_entries(list_index);
        const std::size_t slot = static_cast<std::size_t>(list_index);
        const int selected = selected_menu_entry_[slot];
        if (selected < 0 || selected >= static_cast<int>(entries.size())) return;
        entries.erase(entries.begin() + selected);
        model_.set(slot == 0 ? "menu_places" : "menu_system",
                   serialize_menu_entries(entries));
        selected_menu_entry_[slot] = std::min(selected, static_cast<int>(entries.size()) - 1);
        sync_menu_entry_draft(list_index);
    }

    void move_menu_entry(int list_index, int delta)
    {
        auto entries = menu_entries(list_index);
        const std::size_t slot = static_cast<std::size_t>(list_index);
        const int selected = selected_menu_entry_[slot];
        const int other = selected + delta;
        if (selected < 0 || other < 0 || other >= static_cast<int>(entries.size())) return;
        std::swap(entries[static_cast<std::size_t>(selected)],
                  entries[static_cast<std::size_t>(other)]);
        model_.set(slot == 0 ? "menu_places" : "menu_system",
                   serialize_menu_entries(entries));
        selected_menu_entry_[slot] = other;
        sync_menu_entry_draft(list_index);
    }

    void browse_menu_entry_target(int list_index)
    {
        const char* command = list_index == 0
            ? "zenity --file-selection --directory --title='Select folder' 2>/dev/null"
            : "zenity --file-selection --title='Select .desktop file' "
              "--file-filter='Desktop files | *.desktop' 2>/dev/null";
        FILE* chooser = popen(command, "r");
        if (chooser == nullptr) {
            status_ = "Could not open the file chooser.";
            return;
        }
        std::array<char, 4096> buffer{};
        std::string selected_path;
        while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), chooser) != nullptr) {
            selected_path += buffer.data();
        }
        const int result = pclose(chooser);
        selected_path = ConfigEditorModel::trim(std::move(selected_path));
        if (result == 0 && !selected_path.empty()) {
            auto& draft = menu_entry_draft_[static_cast<std::size_t>(list_index)];
            draft[2] = selected_path;
            if (list_index == 0) {
                const std::size_t slash = selected_path.find_last_of('/');
                if (draft[0].empty()) {
                    draft[0] = slash == std::string::npos
                        ? selected_path : selected_path.substr(slash + 1);
                }
                if (draft[1].empty()) draft[1] = "folder-symbolic";
            } else {
                std::ifstream desktop_file(selected_path);
                bool in_desktop_entry = false;
                std::string line;
                while (std::getline(desktop_file, line)) {
                    line = ConfigEditorModel::trim(std::move(line));
                    if (line.empty() || line.front() == '#') continue;
                    if (line.front() == '[') {
                        in_desktop_entry = line == "[Desktop Entry]";
                        continue;
                    }
                    if (!in_desktop_entry) continue;
                    if (draft[0].empty() && line.rfind("Name=", 0) == 0) {
                        draft[0] = line.substr(5);
                    } else if (draft[1].empty() && line.rfind("Icon=", 0) == 0) {
                        draft[1] = line.substr(5);
                    }
                }
            }
        }
    }

    std::string launcher_draft_;
    std::string desktop_hub_folder_label_draft_;
    std::string desktop_hub_folder_path_draft_;
    int selected_launcher_{-1};
    int raw_scroll_{0};

    void activate(const Hit& hit)
    {
        if (color_picker_open_) {
            if (hit.action == "color-palette" && hit.index >= 0 &&
                hit.index < static_cast<int>(color_palette().size())) {
                const auto separator = color_picker_value_.find_first_of(" \t");
                const std::string suffix = separator == std::string::npos
                    ? std::string{} : color_picker_value_.substr(separator);
                color_picker_value_ = color_palette()[static_cast<std::size_t>(hit.index)] + suffix;
            } else if (hit.action == "color-select") {
                set_field_value(color_picker_target_, color_picker_key_, color_picker_index_,
                                color_picker_value_);
                color_picker_open_ = false;
            } else if (hit.action == "color-cancel") {
                color_picker_open_ = false;
            }
            return;
        }
        if (confirm_close_) {
            if (hit.action == "close-save") { confirm_close_ = false; close_after_save_ = true; save(); }
            else if (hit.action == "close-discard") { confirm_close_ = false; close_window(); }
            else confirm_close_ = false;
            return;
        }
        if (hit.action == "page") { current_page_ = hit.key; content_scroll_ = 0; edit_target_.clear(); return; }
        if (hit.action == "edit") {
            const auto separator = hit.key.find('|');
            if (separator == std::string::npos) return;
            edit_target_ = hit.key.substr(0, separator) + ":" + hit.key.substr(separator + 1);
            edit_key_ = hit.key.substr(separator + 1);
            edit_index_ = hit.index;
            return;
        }
        if (hit.action == "number-edit") {
            const auto separator = hit.key.find('|');
            if (separator == std::string::npos) return;
            const std::string target = hit.key.substr(0, separator);
            edit_target_ = target + ":" + hit.key.substr(separator + 1);
            edit_key_ = hit.key.substr(separator + 1);
            edit_index_ = hit.index;
            number_edit_buffer_ = field_value(edit_key_, target, edit_index_, "0");
            editing_number_ = true;
            number_edit_replace_on_next_input_ = true;
            return;
        }
        if (hit.action == "color-picker") { open_color_picker(hit); return; }
        if (hit.action == "toggle") {
            const auto separator = hit.key.find('|');
            if (separator == std::string::npos) return;
            const std::string target = hit.key.substr(0, separator);
            const std::string key = hit.key.substr(separator + 1);
            const Field* field = target == "value" && page(current_page_) != nullptr
                ? field_for(*page(current_page_), key) : nullptr;
            const std::string value = field_value(key, target, hit.index,
                                                   field != nullptr ? field->initial : "0");
            set_field_value(target, key, hit.index,
                            (value == "1" || value == "true" || value == "yes" || value == "on") ? "0" : "1");
            return;
        }
        if (hit.action == "cycle") { cycle_choice(hit.key); return; }
        if (hit.action == "step") { step_number(hit.key, hit.index); return; }
        if (hit.action == "item-select") { selected_item_ = hit.index; edit_target_.clear(); return; }
        if (hit.action == "plugin-select") { selected_plugin_ = hit.index; return; }
        if (hit.action == "item-up") { move_item(-1); return; }
        if (hit.action == "item-down") { move_item(1); return; }
        if (hit.action == "item-remove") { remove_item(); return; }
        if (hit.action == "item-add") { add_item(); return; }
        if (hit.action == "item-configure") { configure_item(); return; }
        if (hit.action == "button-select") { selected_button_ = hit.index; sync_button_draft(); return; }
        if (hit.action == "button-add") { add_button(); return; }
        if (hit.action == "button-remove") { remove_button(); return; }
        if (hit.action == "executer-select") { selected_executer_ = hit.index; return; }
        if (hit.action == "executer-add") { add_executer(); return; }
        if (hit.action == "executer-remove") { remove_executer(); return; }
        if (hit.action == "background-select") { selected_background_ = hit.index; return; }
        if (hit.action == "background-add") { add_background(); return; }
        if (hit.action == "background-remove") { remove_background(); return; }
        if (hit.action == "launcher-select") { selected_launcher_ = hit.index; return; }
        if (hit.action == "launcher-add") { add_launcher(); return; }
        if (hit.action == "launcher-remove") { remove_launcher(); return; }
        if (hit.action == "launcher-up") { move_launcher(-1); return; }
        if (hit.action == "launcher-down") { move_launcher(1); return; }
        if (hit.action == "desktop-hub-folder-select") {
            selected_desktop_hub_folder_ = hit.index;
            sync_desktop_hub_folder_draft();
            edit_target_.clear();
            return;
        }
        if (hit.action == "desktop-hub-folder-add") { add_desktop_hub_folder(); return; }
        if (hit.action == "desktop-hub-folder-update") { update_desktop_hub_folder(); return; }
        if (hit.action == "desktop-hub-folder-remove") { remove_desktop_hub_folder(); return; }
        if (hit.action == "desktop-hub-folder-up") { move_desktop_hub_folder(-1); return; }
        if (hit.action == "desktop-hub-folder-down") { move_desktop_hub_folder(1); return; }
        if (hit.action == "menu-entry-select") {
            const int list_index = hit.key == "menu_places" ? 0 : 1;
            selected_menu_entry_[static_cast<std::size_t>(list_index)] = hit.index;
            sync_menu_entry_draft(list_index);
            edit_target_.clear();
            return;
        }
        if (hit.action == "menu-entry-add") {
            add_menu_entry(hit.key == "menu_places" ? 0 : 1);
            return;
        }
        if (hit.action == "menu-entry-update") {
            update_menu_entry(hit.key == "menu_places" ? 0 : 1);
            return;
        }
        if (hit.action == "menu-entry-remove") {
            remove_menu_entry(hit.key == "menu_places" ? 0 : 1);
            return;
        }
        if (hit.action == "menu-entry-up") {
            move_menu_entry(hit.key == "menu_places" ? 0 : 1, -1);
            return;
        }
        if (hit.action == "menu-entry-down") {
            move_menu_entry(hit.key == "menu_places" ? 0 : 1, 1);
            return;
        }
        if (hit.action == "menu-entry-browse") {
            browse_menu_entry_target(hit.key == "menu_places" ? 0 : 1);
            return;
        }
        if (hit.action == "save") { save(); return; }
        if (hit.action == "reload") { reload(); return; }
        if (hit.action == "open") { open_config(); return; }
        if (hit.action == "close") { request_close(); return; }
    }

    void cycle_choice(const std::string& key)
    {
        const Page* current = page(current_page_);
        const Field* field = current == nullptr ? nullptr : field_for(*current, key);
        if (field == nullptr || field->options.empty()) return;
        const std::string value = model_.get(key, field->initial);
        auto found = std::find_if(field->options.begin(), field->options.end(), [&value](const auto& item) {
            return item.first == value;
        });
        if (found == field->options.end() || ++found == field->options.end()) found = field->options.begin();
        model_.set(key, found->first);
    }

    const Field* number_field_for_key(const std::string& key) const
    {
        const Page* current = page(current_page_);
        const Field* field = current == nullptr ? nullptr : field_for(*current, key);
        if (field == nullptr && current_page_ == "buttons") {
            static const Field icon = number_field("button_max_icon_size", "", 0, 0, 128);
            static const Field bg = number_field("button_background_id", "", 0, 0, 20);
            field = key == icon.key ? &icon : &bg;
        }
        if (field == nullptr && current_page_ == "backgrounds") {
            static const Field rounded = number_field("rounded", "", 0, 0, 50);
            static const Field border = number_field("border_width", "", 0, 0, 20);
            field = key == rounded.key ? &rounded : &border;
        }
        return field;
    }

    void finish_number_edit()
    {
        if (!editing_number_) return;
        const auto separator = edit_target_.find(':');
        const Field* field = number_field_for_key(edit_key_);
        if (separator != std::string::npos && field != nullptr &&
            !number_edit_buffer_.empty()) {
            long long number = field->maximum;
            try { number = std::stoll(number_edit_buffer_); } catch (...) {}
            number = std::clamp(number, static_cast<long long>(field->minimum),
                                static_cast<long long>(field->maximum));
            set_field_value(edit_target_.substr(0, separator), edit_key_, edit_index_,
                            std::to_string(number));
        }
        editing_number_ = false;
        number_edit_replace_on_next_input_ = false;
        number_edit_buffer_.clear();
        edit_target_.clear();
    }

    void step_number(const std::string& key, int delta)
    {
        const Field* field = number_field_for_key(key);
        if (field == nullptr) return;
        const std::string target = current_page_ == "buttons" ? "button" :
            current_page_ == "backgrounds" ? "background" : "value";
        const int index = target == "button" ? selected_button_ :
            target == "background" ? selected_background_ : -1;
        int number = field->minimum;
        try { number = std::stoi(field_value(key, target, index, field->initial)); } catch (...) {}
        number = std::clamp(number + delta, field->minimum, field->maximum);
        set_field_value(target, key, index, std::to_string(number));
    }

    void move_item(int delta)
    {
        auto items = model_.panel_items();
        if (selected_item_ < 0 || selected_item_ >= static_cast<int>(items.size())) return;
        const int other = selected_item_ + delta;
        if (other < 0 || other >= static_cast<int>(items.size())) return;
        std::swap(items[static_cast<std::size_t>(selected_item_)], items[static_cast<std::size_t>(other)]);
        model_.set_panel_items(items);
        selected_item_ = other;
    }

    void remove_item()
    {
        auto items = model_.panel_items();
        if (selected_item_ < 0 || selected_item_ >= static_cast<int>(items.size()) || items.size() <= 1) return;
        const char removed = items[static_cast<std::size_t>(selected_item_)];
        if (removed == 'P') {
            const int button_index = static_cast<int>(std::count(items.begin(), items.begin() + selected_item_, 'P'));
            if (button_index >= 0 && button_index < static_cast<int>(model_.buttons.size())) {
                model_.buttons.erase(model_.buttons.begin() + button_index);
            }
        }
        if (removed == 'E') {
            const int executer_index = static_cast<int>(std::count(items.begin(), items.begin() + selected_item_, 'E'));
            if (executer_index >= 0 && executer_index < static_cast<int>(model_.executers.size())) {
                model_.executers.erase(model_.executers.begin() + executer_index);
            }
        }
        items.erase(items.begin() + selected_item_);
        model_.set_panel_items(items);
        selected_item_ = std::min(selected_item_, static_cast<int>(items.size()) - 1);
        status_ = "Removed panel item";
    }

    void add_item()
    {
        const auto& catalog = ConfigEditorModel::item_catalog();
        if (selected_plugin_ < 0 || selected_plugin_ >= static_cast<int>(catalog.size())) return;
        auto item = catalog.begin();
        std::advance(item, selected_plugin_);
        auto items = model_.panel_items();
        const char code = item->first;
        if (std::string("DHJMSTW").find(code) != std::string::npos &&
            std::find(items.begin(), items.end(), code) != items.end()) {
            status_ = item->second.first + " already present";
            return;
        }
        items.push_back(code);
        model_.set_panel_items(items);
        if (code == 'P' && model_.buttons.size() < static_cast<std::size_t>(std::count(items.begin(), items.end(), 'P'))) {
            model_.buttons.push_back({{"button_text", "Btn" + std::to_string(model_.buttons.size() + 1)},
                {"button_tooltip", "Custom button"}, {"button_lclick_command", ""}, {"button_max_icon_size", "0"}});
        }
        status_ = "Added " + std::string(1, code);
    }

    void configure_item()
    {
        const auto items = model_.panel_items();
        if (selected_item_ < 0 || selected_item_ >= static_cast<int>(items.size())) return;
        const char code = items[static_cast<std::size_t>(selected_item_)];
        if (code == 'P') { current_page_ = "buttons"; selected_button_ = 0; sync_button_draft(); return; }
        if (code == 'E') {
            current_page_ = "executer";
            selected_executer_ = static_cast<int>(std::count(items.begin(), items.begin() + selected_item_, 'E'));
            return;
        }
        const auto found = std::find_if(pages_.begin(), pages_.end(), [code](const Page& p) { return p.code == code; });
        if (found != pages_.end()) current_page_ = found->id;
    }

    void add_button()
    {
        const auto number = model_.buttons.size() + 1;
        model_.buttons.push_back({{"button_text", "Btn" + std::to_string(number)},
            {"button_tooltip", "Custom button"}, {"button_lclick_command", ""},
            {"button_max_icon_size", "0"}});
        auto items = model_.panel_items();
        items.push_back('P');
        model_.set_panel_items(items);
        selected_button_ = static_cast<int>(model_.buttons.size()) - 1;
        model_.dirty = true;
        sync_button_draft();
    }

    void add_executer()
    {
        model_.executers.push_back({{"executer_text", ">_"},
            {"executer_tooltip", "Executor"}, {"executer_command", ""}});
        auto items = model_.panel_items();
        items.push_back('E');
        model_.set_panel_items(items);
        selected_executer_ = static_cast<int>(model_.executers.size()) - 1;
        model_.dirty = true;
    }

    void remove_executer()
    {
        if (selected_executer_ < 0 || selected_executer_ >= static_cast<int>(model_.executers.size())) return;
        model_.executers.erase(model_.executers.begin() + selected_executer_);
        auto items = model_.panel_items();
        int seen = 0;
        for (auto it = items.begin(); it != items.end(); ++it) {
            if (*it == 'E' && seen++ == selected_executer_) { items.erase(it); break; }
        }
        model_.set_panel_items(items);
        selected_executer_ = std::min(selected_executer_, static_cast<int>(model_.executers.size()) - 1);
        model_.dirty = true;
    }

    void remove_button()
    {
        if (selected_button_ < 0 || selected_button_ >= static_cast<int>(model_.buttons.size())) return;
        model_.buttons.erase(model_.buttons.begin() + selected_button_);
        auto items = model_.panel_items();
        int seen = 0;
        for (auto it = items.begin(); it != items.end(); ++it) {
            if (*it == 'P' && seen++ == selected_button_) { items.erase(it); break; }
        }
        model_.set_panel_items(items);
        selected_button_ = std::min(selected_button_, static_cast<int>(model_.buttons.size()) - 1);
        model_.dirty = true;
        sync_button_draft();
    }

    void sync_button_draft()
    {
        button_draft_.clear();
        if (selected_button_ >= 0 && selected_button_ < static_cast<int>(model_.buttons.size()))
            button_draft_ = model_.buttons[static_cast<std::size_t>(selected_button_)];
    }

    void add_background()
    {
        selected_background_ = model_.backgrounds.empty() ? 1 : model_.backgrounds.rbegin()->first + 1;
        model_.backgrounds[selected_background_] = {{"rounded", "4"}, {"border_width", "0"},
            {"background_color", "#000000 0"}, {"border_color", "#000000 0"}};
        model_.dirty = true;
    }

    void remove_background()
    {
        if (model_.backgrounds.size() <= 1 || selected_background_ == 0) return;
        model_.backgrounds.erase(selected_background_);
        selected_background_ = model_.backgrounds.empty() ? 0 : model_.backgrounds.begin()->first;
        model_.dirty = true;
    }

    void add_launcher()
    {
        if (launcher_draft_.empty()) return;
        model_.launcher_apps.push_back(launcher_draft_);
        selected_launcher_ = static_cast<int>(model_.launcher_apps.size()) - 1;
        launcher_draft_.clear();
        model_.dirty = true;
    }

    void remove_launcher()
    {
        if (selected_launcher_ < 0 || selected_launcher_ >= static_cast<int>(model_.launcher_apps.size())) return;
        model_.launcher_apps.erase(model_.launcher_apps.begin() + selected_launcher_);
        selected_launcher_ = std::min(selected_launcher_, static_cast<int>(model_.launcher_apps.size()) - 1);
        model_.dirty = true;
    }

    void move_launcher(int delta)
    {
        const int other = selected_launcher_ + delta;
        if (selected_launcher_ < 0 || other < 0 || other >= static_cast<int>(model_.launcher_apps.size())) return;
        std::swap(model_.launcher_apps[static_cast<std::size_t>(selected_launcher_)],
                  model_.launcher_apps[static_cast<std::size_t>(other)]);
        selected_launcher_ = other;
        model_.dirty = true;
    }

    void sync_desktop_hub_folder_draft()
    {
        const auto folders = desktop_hub_folders();
        if (selected_desktop_hub_folder_ < 0 ||
            selected_desktop_hub_folder_ >= static_cast<int>(folders.size())) {
            desktop_hub_folder_label_draft_.clear();
            desktop_hub_folder_path_draft_.clear();
            return;
        }
        const auto& folder = folders[static_cast<std::size_t>(selected_desktop_hub_folder_)];
        desktop_hub_folder_label_draft_ = folder.first;
        desktop_hub_folder_path_draft_ = folder.second;
    }

    void add_desktop_hub_folder()
    {
        const std::string label = ConfigEditorModel::trim(desktop_hub_folder_label_draft_);
        const std::string path = ConfigEditorModel::trim(desktop_hub_folder_path_draft_);
        if (label.empty() || path.empty()) return;
        auto folders = desktop_hub_folders();
        folders.emplace_back(label, path);
        model_.set("desktop_hub_folders", serialize_desktop_hub_folders(folders));
        selected_desktop_hub_folder_ = static_cast<int>(folders.size()) - 1;
        desktop_hub_folder_label_draft_ = label;
        desktop_hub_folder_path_draft_ = path;
        desktop_hub_folder_scroll_ = scrollbar_max_scroll("desktop_hub_folders");
    }

    void update_desktop_hub_folder()
    {
        auto folders = desktop_hub_folders();
        if (selected_desktop_hub_folder_ < 0 ||
            selected_desktop_hub_folder_ >= static_cast<int>(folders.size())) return;
        const std::string label = ConfigEditorModel::trim(desktop_hub_folder_label_draft_);
        const std::string path = ConfigEditorModel::trim(desktop_hub_folder_path_draft_);
        if (label.empty() || path.empty()) return;
        folders[static_cast<std::size_t>(selected_desktop_hub_folder_)] = {label, path};
        model_.set("desktop_hub_folders", serialize_desktop_hub_folders(folders));
        desktop_hub_folder_label_draft_ = label;
        desktop_hub_folder_path_draft_ = path;
    }

    void remove_desktop_hub_folder()
    {
        auto folders = desktop_hub_folders();
        if (selected_desktop_hub_folder_ < 0 ||
            selected_desktop_hub_folder_ >= static_cast<int>(folders.size())) return;
        folders.erase(folders.begin() + selected_desktop_hub_folder_);
        model_.set("desktop_hub_folders", serialize_desktop_hub_folders(folders));
        selected_desktop_hub_folder_ = std::min(
            selected_desktop_hub_folder_, static_cast<int>(folders.size()) - 1);
        sync_desktop_hub_folder_draft();
    }

    void move_desktop_hub_folder(int delta)
    {
        auto folders = desktop_hub_folders();
        const int other = selected_desktop_hub_folder_ + delta;
        if (selected_desktop_hub_folder_ < 0 || other < 0 ||
            other >= static_cast<int>(folders.size())) return;
        std::swap(folders[static_cast<std::size_t>(selected_desktop_hub_folder_)],
                  folders[static_cast<std::size_t>(other)]);
        model_.set("desktop_hub_folders", serialize_desktop_hub_folders(folders));
        selected_desktop_hub_folder_ = other;
        sync_desktop_hub_folder_draft();
    }

    void save()
    {
        try {
            model_.save();
            status_ = "Saved " + model_.path.string();
            if (close_after_save_) close_window();
        } catch (const std::exception& error) {
            status_ = "Save failed: " + std::string(error.what());
        }
    }

    void reload()
    {
        try {
            model_.load();
            selected_item_ = -1;
            selected_button_ = -1;
                selected_executer_ = model_.executers.empty() ? -1 : 0;
            selected_background_ = model_.backgrounds.empty() ? 0 : model_.backgrounds.begin()->first;
            selected_desktop_hub_folder_ = -1;
            desktop_hub_folder_scroll_ = 0;
            sync_desktop_hub_folder_draft();
            sync_button_draft();
            status_ = "Reloaded " + model_.path.string();
        } catch (const std::exception& error) {
            status_ = "Reload failed: " + std::string(error.what());
        }
    }

    void open_config()
    {
        const pid_t child = fork();
        if (child == 0) {
            setsid();
            execlp("xdg-open", "xdg-open", model_.path.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        if (child > 0) {
            int status = 0;
            waitpid(child, &status, WNOHANG);
            status_ = "Opened config file";
        } else status_ = "Could not open config file";
    }

    void close_window() { close_requested_ = true; }

    void draw_close_dialog()
    {
        cairo_set_source_rgba(cr_, 0.06, 0.08, 0.10, 0.32);
        cairo_paint(cr_);
        const Rect box{width_ / 2 - 210, height_ / 2 - 90, 420, 180};
        rounded_rect(cr_, box, 10);
        cairo_set_source_rgb(cr_, 1, 1, 1);
        cairo_fill(cr_);
        draw_text(cr_, "Unsaved changes", box.x + 24, box.y + 22, box.width - 48,
                  0.12, 0.14, 0.16, "Sans Bold 13");
        draw_text(cr_, "Save your changes before closing?", box.x + 24, box.y + 54,
                  box.width - 48, 0.25, 0.27, 0.30, "Sans 10");
        draw_button("Cancel", box.x + 22, box.y + 112, 82, 34, "close-cancel");
        draw_button("Discard", box.x + 192, box.y + 112, 82, 34, "close-discard");
        draw_button("Save", box.x + 302, box.y + 112, 82, 34, "close-save", true);
    }

    static const std::array<const char*, 24>& color_palette()
    {
        static const std::array<const char*, 24> colors = {
            "#FFB6C1", "#FF6B6B", "#FA5252", "#E64980",
            "#BE4BDB", "#7950F2", "#4C6EF5", "#228BE6",
            "#15AABF", "#12B886", "#40C057", "#82C91E",
            "#FAB005", "#FD7E14", "#212529", "#1E1E2E",
            "#2D2D2D", "#343A40", "#495057", "#868E96",
            "#CED4DA", "#F8F9FA", "#FFFFFF", "#000000"};
        return colors;
    }

    std::string color_picker_hex() const
    {
        const auto end = color_picker_value_.find_first_of(" \t");
        std::string hex = color_picker_value_.substr(0, end);
        std::transform(hex.begin(), hex.end(), hex.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        return hex;
    }

    void open_color_picker(const Hit& hit)
    {
        const auto separator = hit.key.find('|');
        if (separator == std::string::npos) return;
        color_picker_target_ = hit.key.substr(0, separator);
        color_picker_key_ = hit.key.substr(separator + 1);
        color_picker_index_ = hit.index;
        std::string fallback = "#ffffff 100";
        if (color_picker_target_ == "value") {
            const Page* current = page(current_page_);
            const Field* field = current == nullptr ? nullptr : field_for(*current, color_picker_key_);
            if (field != nullptr) fallback = field->initial;
        }
        color_picker_value_ = field_value(color_picker_key_, color_picker_target_,
                                          color_picker_index_, fallback);
        color_picker_open_ = true;
        edit_target_.clear();
    }

    void draw_color_picker()
    {
        cairo_set_source_rgba(cr_, 0, 0, 0, 0.60);
        cairo_paint(cr_);
        const Rect box{width_ / 2 - 210, height_ / 2 - 140, 420, 280};
        rounded_rect(cr_, box, 8);
        cairo_set_source_rgb(cr_, 0.18, 0.18, 0.20);
        cairo_fill_preserve(cr_);
        cairo_set_source_rgb(cr_, 0.35, 0.35, 0.40);
        cairo_stroke(cr_);
        draw_text(cr_, "Select color", box.x + 20, box.y + 16, box.width - 40,
                  1, 1, 1, "Sans Bold 14");
        const auto& colors = color_palette();
        const std::string selected = color_picker_hex();
        for (std::size_t index = 0; index < colors.size(); ++index) {
            const int column = static_cast<int>(index % 6);
            const int row = static_cast<int>(index / 6);
            const Rect swatch{box.x + 20 + column * 44, box.y + 50 + row * 34, 36, 26};
            draw_swatch(colors[index], swatch.x, swatch.y, swatch.width, swatch.height);
            rounded_rect(cr_, swatch, 4);
            cairo_set_line_width(cr_, 2);
            cairo_set_source_rgb(cr_, selected == colors[index] ? 1 : 0.30,
                                 selected == colors[index] ? 1 : 0.30,
                                 selected == colors[index] ? 1 : 0.35);
            cairo_stroke(cr_);
            add_hit(swatch, "color-palette", {}, static_cast<int>(index));
        }
        draw_swatch(color_picker_value_, box.x + 20, box.y + 196, 70, 28);
        draw_text(cr_, color_picker_value_, box.x + 105, box.y + 201,
                  box.width - 210, 0.90, 0.90, 0.90, "Sans Bold 12");
        draw_button("Cancel", box.x + box.width - 190, box.y + box.height - 44,
                    80, 28, "color-cancel");
        draw_button("Select", box.x + box.width - 100, box.y + box.height - 44,
                    80, 28, "color-select", true);
    }

    void append_edit_text(const std::string& text)
    {
        set_edit_text(current_edit_text() + text);
    }

    void erase_edit_text()
    {
        std::string value = current_edit_text();
        if (!value.empty()) {
            value.pop_back();
            while (!value.empty() && (static_cast<unsigned char>(value.back()) & 0xC0) == 0x80) {
                value.pop_back();
            }
            if (!value.empty() && (static_cast<unsigned char>(value.back()) & 0x80) != 0) {
                value.pop_back();
            }
        }
        set_edit_text(std::move(value));
    }

    std::string current_edit_text() const
    {
        const auto separator = edit_target_.find(':');
        if (separator == std::string::npos) return {};
        return field_value(edit_key_, edit_target_.substr(0, separator), edit_index_, {});
    }

    void set_edit_text(std::string text)
    {
        const auto separator = edit_target_.find(':');
        if (separator == std::string::npos) return;
        set_field_value(edit_target_.substr(0, separator), edit_key_, edit_index_, std::move(text));
    }
};

}

int main()
{
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::cerr << "Could not open X11 display; under Wayland, make sure XWayland and DISPLAY are available.\n";
        return 1;
    }
    const int screen = DefaultScreen(display);
    const Window window = XCreateSimpleWindow(display, RootWindow(display, screen),
        0, 0, initial_width, initial_height, 1,
        BlackPixel(display, screen), WhitePixel(display, screen));
    char window_instance[] = "mdeon-panel-config";
    char window_class[] = "mdeon-panel-config";
    XClassHint class_hint{window_instance, window_class};
    XSetClassHint(display, window, &class_hint);
    XStoreName(display, window, "mdeon-panel settings");
    XSelectInput(display, window, ExposureMask | StructureNotifyMask | KeyPressMask |
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask | ButtonMotionMask);
    XSizeHints hints{};
    hints.flags = PMinSize;
    hints.min_width = 760;
    hints.min_height = 520;
    XSetWMNormalHints(display, window, &hints);
    const Atom delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    Atom protocols[] = {delete_window};
    XSetWMProtocols(display, window, protocols, 1);
    XMapWindow(display, window);

    int width = initial_width;
    int height = initial_height;
    cairo_surface_t* surface = cairo_xlib_surface_create(
        display, window, DefaultVisual(display, screen), width, height);
    cairo_t* context = cairo_create(surface);
    std::unique_ptr<ConfigEditor> editor;
    try {
        editor = std::make_unique<ConfigEditor>(display, window, context, width, height);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        cairo_destroy(context);
        cairo_surface_destroy(surface);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return 1;
    }
    bool running = true;
    while (running && !editor->should_close()) {
        XEvent event{};
        XNextEvent(display, &event);
        if (event.type == Expose && event.xexpose.count == 0) {
            editor->render();
        } else if (event.type == ConfigureNotify) {
            width = std::max(1, event.xconfigure.width);
            height = std::max(1, event.xconfigure.height);
            cairo_xlib_surface_set_size(surface, width, height);
            editor->resize(width, height);
            editor->render();
        } else if (event.type == ClientMessage &&
                   static_cast<Atom>(event.xclient.data.l[0]) == delete_window) {
            editor->request_close();
        } else if (event.type == ButtonPress) {
            editor->click(event.xbutton.x, event.xbutton.y, event.xbutton.button);
        } else if (event.type == MotionNotify) {
            editor->pointer_motion(event.xmotion.y);
        } else if (event.type == ButtonRelease) {
            editor->pointer_release();
        } else if (event.type == KeyPress) {
            editor->key(event.xkey);
        }
    }

    cairo_destroy(context);
    cairo_surface_destroy(surface);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
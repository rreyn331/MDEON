#include "panel/backends/x11_backend.hpp"
#include "panel/logging.hpp"

#include <cairo/cairo-xcb.h>
#include <pango/pangocairo.h>

#include "panel/icon_theme.hpp"
#include <glib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iostream>
#include <limits>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <utility>

#include "panel/icon_theme.hpp"
#include "panel/theme.hpp"
#include "panel/modules/system_commands.hpp"

namespace {

constexpr int calendar_width = 252;
constexpr int calendar_height = 250;

std::string application_menu_category_icon(const std::string& category)
{
    static const std::map<std::string, std::string> icons = {
        {"All", "view-app-grid-symbolic"},
        {"Accessories", "applications-accessories-symbolic"},
        {"Graphics", "applications-graphics-symbolic"},
        {"Internet", "applications-internet-symbolic"},
        {"Office", "applications-office-symbolic"},
        {"Sound & Video", "applications-multimedia-symbolic"},
        {"Development", "applications-development-symbolic"},
        {"Education", "applications-science-symbolic"},
        {"Games", "applications-games-symbolic"},
        {"System Tools", "applications-system-symbolic"},
        {"Other", "application-x-executable-symbolic"}
    };
    const auto found = icons.find(category);
    return found == icons.end() ? "application-x-executable-symbolic" : found->second;
}

void draw_centered_text(cairo_t* cr, const std::string& text, double center_x,
                        double top_y, const char* font_name, RgbColor color)
{
    PangoLayout* layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text.c_str(), -1);
    PangoFontDescription* font = pango_font_description_from_string(font_name);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    int text_width = 0;
    int text_height = 0;
    pango_layout_get_pixel_size(layout, &text_width, &text_height);
    cairo_set_source_rgb(cr, color.red, color.green, color.blue);
    cairo_move_to(cr, center_x - text_width / 2.0, top_y + (24.0 - text_height) / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

xcb_atom_t intern_atom(xcb_connection_t* connection, const char* name)
{
    const auto cookie = xcb_intern_atom(connection, 0,
                                        static_cast<uint16_t>(std::char_traits<char>::length(name)),
                                        name);
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(connection, cookie, nullptr);
    if (reply == nullptr) {
        return XCB_ATOM_NONE;
    }
    const xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

xcb_visualtype_t* find_visual(xcb_screen_t* screen, xcb_visualid_t visual_id)
{
    for (xcb_depth_iterator_t depth = xcb_screen_allowed_depths_iterator(screen);
         depth.rem != 0; xcb_depth_next(&depth)) {
        for (xcb_visualtype_iterator_t visual = xcb_depth_visuals_iterator(depth.data);
             visual.rem != 0; xcb_visualtype_next(&visual)) {
            if (visual.data->visual_id == visual_id) {
                return visual.data;
            }
        }
    }
    return nullptr;
}

void set_cardinals(xcb_connection_t* connection, xcb_window_t window,
                   xcb_atom_t property, const uint32_t* values, uint32_t count)
{
    if (property != XCB_ATOM_NONE) {
        xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, property,
                            XCB_ATOM_CARDINAL, 32, count, values);
    }
}

bool has_cardinal_property(xcb_connection_t* connection, xcb_window_t window,
                           const char* name)
{
    const xcb_atom_t property = intern_atom(connection, name);
    if (property == XCB_ATOM_NONE) {
        return false;
    }
    const auto cookie = xcb_get_property(connection, 0, window, property,
                                          XCB_ATOM_CARDINAL, 0, 1);
    xcb_get_property_reply_t* reply = xcb_get_property_reply(connection, cookie, nullptr);
    const bool available = reply != nullptr && reply->format == 32 &&
        xcb_get_property_value_length(reply) >= static_cast<int>(sizeof(uint32_t));
    free(reply);
    return available;
}

bool read_cardinal_property(xcb_connection_t* connection, xcb_window_t window,
                            const char* name, uint32_t& value)
{
    const xcb_atom_t property = intern_atom(connection, name);
    if (property == XCB_ATOM_NONE) {
        return false;
    }
    const auto cookie = xcb_get_property(connection, 0, window, property,
                                          XCB_ATOM_CARDINAL, 0, 1);
    xcb_get_property_reply_t* reply = xcb_get_property_reply(connection, cookie, nullptr);
    const bool available = reply != nullptr && reply->format == 32 &&
        xcb_get_property_value_length(reply) >= static_cast<int>(sizeof(uint32_t));
    if (available) {
        value = *static_cast<uint32_t*>(xcb_get_property_value(reply));
    }
    free(reply);
    return available;
}

void send_root_client_message(xcb_connection_t* connection, xcb_screen_t* screen,
                              const char* name, uint32_t first, uint32_t second)
{
    xcb_client_message_event_t message{};
    message.response_type = XCB_CLIENT_MESSAGE;
    message.format = 32;
    message.window = screen->root;
    message.type = intern_atom(connection, name);
    message.data.data32[0] = first;
    message.data.data32[1] = second;
    xcb_send_event(connection, 0, screen->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char*>(&message));
    xcb_flush(connection);
}

}

X11Backend::~X11Backend()
{
    release_escape_key();
    destroy_application_menu();
    destroy_desktop_hub();
    destroy_task_menu();
    destroy_tooltip();
    if (calendar_cairo_ != nullptr) {
        cairo_destroy(calendar_cairo_);
    }
    if (calendar_surface_ != nullptr) {
        cairo_surface_destroy(calendar_surface_);
    }
    if (cairo_ != nullptr) {
        cairo_destroy(cairo_);
    }
    if (surface_ != nullptr) {
        cairo_surface_destroy(surface_);
    }
    if (connection_ != nullptr) {
        if (calendar_window_ != XCB_WINDOW_NONE) {
            xcb_destroy_window(connection_, calendar_window_);
        }
        if (window_ != XCB_WINDOW_NONE) {
            xcb_destroy_window(connection_, window_);
            xcb_flush(connection_);
        }
        xcb_disconnect(connection_);
    }
}

bool X11Backend::init(int requested_width, int requested_height, Edge edge)
{
    edge_ = edge;
    int screen_number = 0;
    connection_ = xcb_connect(nullptr, &screen_number);
    if (connection_ == nullptr || xcb_connection_has_error(connection_) != 0) {
        std::cerr << "Failed to connect to the X server.\n";
        return false;
    }

    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(connection_));
    for (int index = 0; index < screen_number && screens.rem != 0; ++index) {
        xcb_screen_next(&screens);
    }
    screen_ = screens.data;
    if (screen_ == nullptr) {
        std::cerr << "Failed to find the current X screen.\n";
        return false;
    }

    const char* desktop_value = std::getenv("XDG_CURRENT_DESKTOP");
    const char* session_desktop_value = std::getenv("XDG_SESSION_DESKTOP");
    const char* setup_display_value = std::getenv("GNOME_SETUP_DISPLAY");
    const char* compositor_value = std::getenv("MDEON_SESSION_COMPOSITOR");
    if (std::getenv("WAYLAND_DISPLAY") != nullptr) {
        std::string desktop = desktop_value != nullptr ? desktop_value : "";
        if (session_desktop_value != nullptr) {
            desktop += ":";
            desktop += session_desktop_value;
        }
        std::transform(desktop.begin(), desktop.end(), desktop.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        gnome_wayland_session_ = desktop.find("gnome") != std::string::npos ||
            setup_display_value != nullptr ||
            (compositor_value != nullptr &&
             std::string(compositor_value) == "gnome-shell");
    }

    int output_x = 0;
    int output_y = 0;
    int output_width = screen_->width_in_pixels;
    int output_height = screen_->height_in_pixels;
    if (monitor_ != "all") {
        const auto outputs = SystemCommands::run({"xrandr", "--query"});
        const std::regex connected_line(R"(^([^[:space:]]+) connected(.*)$)");
        const std::regex geometry_pattern(R"(([0-9]+)x([0-9]+)([+-][0-9]+)([+-][0-9]+))");
        bool found_output = false;
        std::istringstream lines(outputs.output);
        std::string line;
        std::smatch connected_match;
        while (std::getline(lines, line)) {
            if (!std::regex_search(line, connected_match, connected_line)) {
                continue;
            }
            const std::string output_name = connected_match[1].str();
            const std::string details = connected_match[2].str();
            const bool selected = monitor_ == output_name ||
                (monitor_ == "primary" && details.find("primary") != std::string::npos);
            if (!selected) {
                continue;
            }
            std::smatch geometry_match;
            if (!std::regex_search(details, geometry_match, geometry_pattern)) {
                continue;
            }
            output_width = std::stoi(geometry_match[1].str());
            output_height = std::stoi(geometry_match[2].str());
            output_x = std::stoi(geometry_match[3].str());
            output_y = std::stoi(geometry_match[4].str());
            found_output = true;
            break;
        }
        if (!found_output) {
            std::cerr << "Could not find active X11 monitor '" << monitor_ << "'.\n";
            return false;
        }
    }

    width_ = requested_width > 0 ? requested_width : output_width;
    height_ = requested_height > 0 ? requested_height : output_height;
    monitor_x_ = output_x;
    monitor_y_ = output_y;
    monitor_width_ = output_width;
    monitor_height_ = output_height;
    if (width_ <= 0 || height_ <= 0) {
        std::cerr << "Panel width and height must be positive.\n";
        return false;
    }

    const int x = output_x + (edge == Edge::Right ? output_width - width_ : 0);
    const int y = output_y + (edge == Edge::Bottom ? output_height - height_ : 0);
    panel_x_ = x;
    panel_y_ = y;
    window_ = xcb_generate_id(connection_);
    const uint32_t event_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
                                XCB_EVENT_MASK_BUTTON_RELEASE |
                                XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW |
                                XCB_EVENT_MASK_LEAVE_WINDOW;
    const uint32_t values[] = {screen_->black_pixel, event_mask};
    xcb_create_window(connection_, XCB_COPY_FROM_PARENT, window_, screen_->root,
                      x, y, static_cast<uint16_t>(width_), static_cast<uint16_t>(height_),
                      0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen_->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);

    set_dock_properties(edge);
    xcb_map_window(connection_, window_);
    const uint32_t position[] = {static_cast<uint32_t>(x), static_cast<uint32_t>(y)};
    xcb_configure_window(connection_, window_,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, position);
    xcb_flush(connection_);

    xcb_visualtype_t* visual = find_visual(screen_, screen_->root_visual);
    if (visual == nullptr) {
        std::cerr << "Failed to find the X screen visual.\n";
        return false;
    }
    surface_ = cairo_xcb_surface_create(connection_, window_, visual, width_, height_);
    if (cairo_surface_status(surface_) != CAIRO_STATUS_SUCCESS) {
        std::cerr << "Failed to create the Cairo XCB surface.\n";
        return false;
    }
    cairo_ = cairo_create(surface_);
    if (cairo_status(cairo_) != CAIRO_STATUS_SUCCESS) {
        return false;
    }
    panel_debug("X11 panel surface ready: display=", std::getenv("DISPLAY"),
        " screen=", screen_->width_in_pixels, "x", screen_->height_in_pixels,
        " panel=", width_, "x", height_, " desktop=",
        desktop_value != nullptr ? desktop_value : "(unset)");
    return true;
}

bool X11Backend::set_monitor(const std::string& monitor)
{
    monitor_ = monitor.empty() ? "all" : monitor;
    return true;
}

void X11Backend::set_task_monitor_filter(bool enabled)
{
    task_monitor_filter_ = enabled;
}

std::vector<std::string> X11Backend::list_monitors()
{
    std::vector<std::string> monitors;
    const auto outputs = SystemCommands::run({"xrandr", "--query"});
    if (outputs.exit_code != 0) {
        return monitors;
    }
    const std::regex connected_line(R"(^([^[:space:]]+) connected(.*)$)");
    const std::regex geometry_pattern(R"(([0-9]+)x([0-9]+)([+-][0-9]+)([+-][0-9]+))");
    std::istringstream lines(outputs.output);
    std::string line;
    std::smatch connected_match;
    while (std::getline(lines, line)) {
        if (!std::regex_search(line, connected_match, connected_line)) {
            continue;
        }
        std::smatch geometry_match;
        const std::string details = connected_match[2].str();
        if (std::regex_search(details, geometry_match, geometry_pattern)) {
            monitors.push_back(connected_match[1].str());
        }
    }
    return monitors;
}

cairo_t* X11Backend::get_cairo()
{
    return cairo_;
}

void X11Backend::commit()
{
    cairo_surface_flush(surface_);
    xcb_flush(connection_);
}

int X11Backend::event_fd() const
{
    return connection_ == nullptr ? -1 : xcb_get_file_descriptor(connection_);
}

bool X11Backend::grab_escape_key(std::function<void()> callback)
{
    if (connection_ == nullptr || screen_ == nullptr || !callback) {
        return false;
    }
    if (escape_keycode_ != 0) {
        escape_key_callback_ = std::move(callback);
        return true;
    }

    const xcb_setup_t* setup = xcb_get_setup(connection_);
    if (setup == nullptr) {
        return false;
    }
    const uint8_t keycode_count = static_cast<uint8_t>(
        setup->max_keycode - setup->min_keycode + 1);
    const auto mapping_cookie = xcb_get_keyboard_mapping(
        connection_, setup->min_keycode, keycode_count);
    xcb_get_keyboard_mapping_reply_t* mapping = xcb_get_keyboard_mapping_reply(
        connection_, mapping_cookie, nullptr);
    if (mapping == nullptr) {
        return false;
    }

    constexpr xcb_keysym_t escape_keysym = 0xff1b;
    const xcb_keysym_t* symbols = xcb_get_keyboard_mapping_keysyms(mapping);
    xcb_keycode_t escape_keycode = 0;
    for (int keycode_offset = 0; keycode_offset < keycode_count && escape_keycode == 0;
         ++keycode_offset) {
        for (int symbol_offset = 0; symbol_offset < mapping->keysyms_per_keycode;
             ++symbol_offset) {
            const std::size_t symbol_index = static_cast<std::size_t>(keycode_offset) *
                mapping->keysyms_per_keycode + symbol_offset;
            if (symbols[symbol_index] == escape_keysym) {
                escape_keycode = static_cast<xcb_keycode_t>(setup->min_keycode + keycode_offset);
                break;
            }
        }
    }
    free(mapping);
    if (escape_keycode == 0) {
        return false;
    }

    constexpr uint16_t lock_masks[] = {
        0, XCB_MOD_MASK_LOCK, XCB_MOD_MASK_2,
        XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2
    };
    for (const uint16_t modifiers : lock_masks) {
        const auto cookie = xcb_grab_key_checked(connection_, 1, screen_->root,
            modifiers, escape_keycode, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        xcb_generic_error_t* error = xcb_request_check(connection_, cookie);
        if (error != nullptr) {
            free(error);
            for (const uint16_t grabbed_modifiers : lock_masks) {
                xcb_ungrab_key(connection_, escape_keycode, screen_->root, grabbed_modifiers);
            }
            xcb_flush(connection_);
            return false;
        }
    }
    escape_keycode_ = escape_keycode;
    escape_key_callback_ = std::move(callback);
    xcb_flush(connection_);
    return true;
}

void X11Backend::release_escape_key()
{
    if (connection_ != nullptr && screen_ != nullptr && escape_keycode_ != 0) {
        constexpr uint16_t lock_masks[] = {
            0, XCB_MOD_MASK_LOCK, XCB_MOD_MASK_2,
            XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2
        };
        for (const uint16_t modifiers : lock_masks) {
            xcb_ungrab_key(connection_, escape_keycode_, screen_->root, modifiers);
        }
        xcb_flush(connection_);
    }
    escape_keycode_ = 0;
    escape_key_callback_ = {};
}

void X11Backend::process_events()
{
    if (connection_ == nullptr) {
        return;
    }
    while (xcb_generic_event_t* event = xcb_poll_for_event(connection_)) {
        const uint8_t response = event->response_type & ~0x80;
        if (response == XCB_EXPOSE) {
            const auto* expose = reinterpret_cast<xcb_expose_event_t*>(event);
            if (expose->window == calendar_window_) {
                draw_calendar();
            } else if (expose->window == tooltip_window_) {
                draw_tooltip();
            } else if (expose->window == task_menu_window_) {
                draw_task_menu();
            } else if (expose->window == application_menu_window_) {
                draw_application_menu();
            } else if (expose->window == application_power_window_) {
                draw_application_power_menu();
            } else if (expose->window == desktop_hub_window_) {
                draw_desktop_hub();
            }
        } else if (response == XCB_FOCUS_OUT) {
            const auto* focus = reinterpret_cast<xcb_focus_out_event_t*>(event);
            if (focus->event == calendar_window_) {
                hide_calendar();
            } else if (focus->event == task_menu_window_) {
                hide_task_menu();
            } else if (focus->event == application_power_window_) {
                hide_application_power_menu();
            } else if (focus->event == desktop_hub_window_) {
                hide_desktop_hub(true);
            }
        } else if (response == XCB_KEY_PRESS) {
            const auto* press = reinterpret_cast<xcb_key_press_event_t*>(event);
            if (press->event == calendar_window_) {
                hide_calendar();
            } else if (press->event == task_menu_window_) {
                hide_task_menu();
            } else if (press->event == application_menu_window_) {
                handle_application_menu_key(press);
            } else if (press->event == application_power_window_) {
                handle_application_power_key(press);
            } else if (press->event == desktop_hub_window_) {
                handle_desktop_hub_key(press);
            } else if (screen_ != nullptr && press->event == screen_->root &&
                       press->detail == escape_keycode_ && escape_key_callback_) {
                const auto callback = escape_key_callback_;
                callback();
            }
        } else if (response == XCB_KEY_RELEASE) {
            const auto* release = reinterpret_cast<xcb_key_release_event_t*>(event);
            if (release->event == application_menu_window_) {
                application_menu_tab_held_ = false;
            }
        } else if (response == XCB_BUTTON_RELEASE) {
            const auto* release = reinterpret_cast<xcb_button_release_event_t*>(event);
            if (release->event == application_menu_window_) {
                application_menu_scrollbar_drag_column_ = 0;
            }
            if ((release->event == window_ || release->event == application_menu_window_) &&
                application_menu_visible_) {
                if (!application_menu_pointer_grabbed_ || !application_menu_keyboard_grabbed_) {
                    grab_application_menu_inputs();
                }
            }
        } else if (response == XCB_BUTTON_PRESS) {
            const auto* press = reinterpret_cast<xcb_button_press_event_t*>(event);
            if (press->event == calendar_window_) {
                handle_calendar_click(press->event_x, press->event_y);
            } else if (press->event == task_menu_window_) {
                handle_task_menu_click(press->event_x, press->event_y);
            } else if (press->event == application_menu_window_) {
                handle_application_menu_click(press->event_x, press->event_y,
                    press->root_x, press->root_y, press->detail);
            } else if (press->event == application_power_window_) {
                handle_application_power_click(press->event_x, press->event_y,
                    press->root_x, press->root_y, press->detail);
            } else if (press->event == desktop_hub_window_) {
                handle_desktop_hub_click(press->event_x, press->event_y, press->detail);
            } else if (press->event == window_ && click_callback_ != nullptr) {
                const bool menu_was_visible = application_menu_visible_;
                hide_task_menu();
                click_callback_(press->event_x, press->event_y, press->detail);
                if (menu_was_visible && application_menu_visible_ &&
                    !application_menu_pointer_grabbed_) {
                    hide_application_menu(true);
                }
            }
        } else if (response == XCB_MOTION_NOTIFY) {
            const auto* motion = reinterpret_cast<xcb_motion_notify_event_t*>(event);
            if (motion->event == window_ && motion_callback_) {
                motion_callback_(motion->event_x, motion->event_y);
            } else if (motion->event == task_menu_window_) {
                const int hovered = motion->event_y / task_menu_row_height_;
                if (hovered != task_menu_hovered_ && hovered >= 0 && hovered < 4) {
                    task_menu_hovered_ = hovered;
                    draw_task_menu();
                }
            } else if (motion->event == application_menu_window_) {
                handle_application_menu_motion(motion->event_x, motion->event_y,
                    motion->root_x, motion->root_y);
            } else if (motion->event == application_power_window_) {
                handle_application_power_motion(motion->event_y);
            } else if (motion->event == desktop_hub_window_) {
                const int first_row = 66;
                const int hovered = motion->event_y < first_row
                    ? -1 : static_cast<int>(desktop_hub_scroll_) +
                        (motion->event_y - first_row) / desktop_hub_row_height_;
                if (hovered >= 0 && static_cast<std::size_t>(hovered) < desktop_hub_items_.size()) {
                    desktop_hub_selected_ = static_cast<std::size_t>(hovered);
                    update_desktop_hub_scroll();
                    draw_desktop_hub();
                }
            }
        } else if (response == XCB_ENTER_NOTIFY) {
            const auto* enter = reinterpret_cast<xcb_enter_notify_event_t*>(event);
            handle_popup_crossing(enter->event, true, enter->mode, enter->detail);
        } else if (response == XCB_LEAVE_NOTIFY) {
            const auto* leave = reinterpret_cast<xcb_leave_notify_event_t*>(event);
            handle_popup_crossing(leave->event, false, leave->mode, leave->detail);
            if (leave->event == window_) {
                const bool entered_tooltip = tooltip_visible_ &&
                    leave->root_x >= tooltip_x_ && leave->root_x < tooltip_x_ + tooltip_width_ &&
                    leave->root_y >= tooltip_y_ && leave->root_y < tooltip_y_ + tooltip_height_;
                if (!entered_tooltip && motion_callback_) {
                    motion_callback_(-1, -1);
                }
            } else if (leave->event == tooltip_window_) {
                const bool entered_panel = leave->root_x >= 0 && leave->root_x < width_ &&
                    leave->root_y >= panel_y_ && leave->root_y < panel_y_ + height_;
                if (!entered_panel) {
                    hide_tooltip();
                }
            }
        }
        free(event);
    }
}

void X11Backend::set_on_click(ClickCallback callback)
{
    click_callback_ = std::move(callback);
}

void X11Backend::set_on_motion(MotionCallback callback)
{
    motion_callback_ = std::move(callback);
}

void X11Backend::open_application_menu(
    int anchor_x, int anchor_width, ApplicationMenuSettings settings,
    std::vector<std::string> categories,
    std::vector<ApplicationMenuDisplayItem> items,
    std::function<void(const std::string&)> on_category,
    std::function<void(const std::string&)> on_search,
    std::function<void(std::size_t)> on_activate,
    std::function<void(bool, std::size_t)> on_shortcut,
    std::function<void(const std::string&, const std::string&)> on_power_action,
    std::function<void()> on_close)
{
    hide_application_power_menu();
    if (application_menu_visible_) {
        hide_application_menu(true);
    }
    application_menu_categories_ = std::move(categories);
    application_menu_settings_ = std::move(settings);
    application_menu_width_ = std::clamp(application_menu_settings_.width, 200,
        std::max(200, static_cast<int>(screen_->width_in_pixels)));
    if (application_menu_width_ < application_menu_settings_.width) {
        const int requested_width = application_menu_settings_.width;
        const int places_width = application_menu_settings_.show_places
            ? application_menu_settings_.places_width : 0;
        const int categories_width = application_menu_settings_.show_categories
            ? application_menu_settings_.categories_width : 0;
        const double scale = static_cast<double>(application_menu_width_) / requested_width;
        application_menu_settings_.places_width = static_cast<int>(places_width * scale);
        application_menu_settings_.categories_width =
            static_cast<int>(categories_width * scale);
        application_menu_settings_.applications_width = application_menu_width_ -
            application_menu_settings_.places_width -
            application_menu_settings_.categories_width;
        application_menu_settings_.width = application_menu_width_;
    }
    application_menu_height_ = std::clamp(application_menu_settings_.height, 200,
        std::max(200, static_cast<int>(screen_->height_in_pixels)));
    application_menu_items_ = std::move(items);
    application_menu_query_.clear();
    application_menu_active_category_ = "All";
    application_menu_selected_ = 0;
    application_menu_scroll_ = 0;
    application_menu_selected_category_ = 0;
    application_menu_category_scroll_ = 0;
    application_menu_scrollbar_drag_column_ = 0;
    application_menu_selected_shortcut_ = 0;
    application_menu_shortcut_scroll_ = 0;
    application_menu_tab_held_ = false;
    application_menu_focus_ = 0;
    application_menu_category_callback_ = std::move(on_category);
    application_menu_search_callback_ = std::move(on_search);
    application_menu_activate_callback_ = std::move(on_activate);
    application_menu_shortcut_callback_ = std::move(on_shortcut);
    application_menu_power_callback_ = std::move(on_power_action);
    application_menu_close_callback_ = std::move(on_close);

    if (application_menu_window_ == XCB_WINDOW_NONE) {
        const uint32_t event_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
            XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION |
            XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |
            XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_KEY_PRESS |
            XCB_EVENT_MASK_KEY_RELEASE;
        const uint32_t values[] = {screen_->white_pixel, 1, event_mask};
        application_menu_window_ = xcb_generate_id(connection_);
        xcb_create_window(connection_, XCB_COPY_FROM_PARENT, application_menu_window_, screen_->root,
            0, 0, application_menu_width_, application_menu_height_, 0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT, screen_->root_visual,
            XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
        const xcb_atom_t name = intern_atom(connection_, "_NET_WM_NAME");
        const xcb_atom_t utf8 = intern_atom(connection_, "UTF8_STRING");
        constexpr char title[] = "Application Menu";
        if (name != XCB_ATOM_NONE && utf8 != XCB_ATOM_NONE) {
            xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, application_menu_window_, name,
                utf8, 8, sizeof(title) - 1, title);
        }
        xcb_visualtype_t* visual = find_visual(screen_, screen_->root_visual);
        if (visual == nullptr) {
            destroy_application_menu();
            return;
        }
        application_menu_surface_ = cairo_xcb_surface_create(connection_, application_menu_window_,
            visual, application_menu_width_, application_menu_height_);
        if (cairo_surface_status(application_menu_surface_) != CAIRO_STATUS_SUCCESS) {
            destroy_application_menu();
            return;
        }
        application_menu_cairo_ = cairo_create(application_menu_surface_);
        if (cairo_status(application_menu_cairo_) != CAIRO_STATUS_SUCCESS) {
            destroy_application_menu();
            return;
        }
    }

    position_popup(anchor_x, anchor_width, application_menu_width_, application_menu_height_,
                   application_menu_x_, application_menu_y_);
    draw_application_menu();
    const uint32_t coordinates[] = {
        static_cast<uint32_t>(application_menu_x_), static_cast<uint32_t>(application_menu_y_)
    };
    xcb_configure_window(connection_, application_menu_window_,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, coordinates);
    application_menu_visible_ = true;
    xcb_map_window(connection_, application_menu_window_);
    const uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(connection_, application_menu_window_, XCB_CONFIG_WINDOW_STACK_MODE,
                         &stack_mode);
    const auto focus_cookie = xcb_get_input_focus(connection_);
    xcb_get_input_focus_reply_t* focus_reply =
        xcb_get_input_focus_reply(connection_, focus_cookie, nullptr);
    application_menu_previous_focus_valid_ = focus_reply != nullptr;
    if (focus_reply != nullptr) {
        application_menu_previous_focus_ = focus_reply->focus;
        free(focus_reply);
    }
    xcb_set_input_focus(connection_, XCB_INPUT_FOCUS_PARENT,
                        application_menu_window_, XCB_CURRENT_TIME);
    grab_application_menu_inputs();
    xcb_flush(connection_);
}

void X11Backend::grab_application_menu_inputs()
{
    if (connection_ == nullptr || application_menu_window_ == XCB_WINDOW_NONE) {
        return;
    }

    const auto pointer_cookie = xcb_grab_pointer(connection_, 1, application_menu_window_,
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_POINTER_MOTION |
            XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW,
        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_WINDOW_NONE, XCB_CURSOR_NONE,
        XCB_CURRENT_TIME);

    const auto keyboard_cookie = xcb_grab_keyboard(connection_, 0, application_menu_window_,
        XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);

    xcb_grab_pointer_reply_t* pointer_reply =
        xcb_grab_pointer_reply(connection_, pointer_cookie, nullptr);
    xcb_grab_keyboard_reply_t* keyboard_reply =
        xcb_grab_keyboard_reply(connection_, keyboard_cookie, nullptr);
    application_menu_pointer_grabbed_ = pointer_reply != nullptr &&
        pointer_reply->status == XCB_GRAB_STATUS_SUCCESS;
    application_menu_keyboard_grabbed_ = keyboard_reply != nullptr &&
        keyboard_reply->status == XCB_GRAB_STATUS_SUCCESS;
    free(pointer_reply);
    free(keyboard_reply);
}

void X11Backend::update_application_menu(std::string query,
                                         std::vector<ApplicationMenuDisplayItem> items,
                                         std::size_t selected, bool preserve_selection)
{
    const std::size_t previous_selected = application_menu_selected_;
    application_menu_query_ = std::move(query);
    application_menu_items_ = std::move(items);
    application_menu_selected_ = application_menu_items_.empty()
        ? 0 : std::min(preserve_selection ? previous_selected : selected,
                       application_menu_items_.size() - 1);
    update_application_menu_scroll();
    draw_application_menu();
}

void X11Backend::set_application_menu_countdown_active(bool active)
{
    const bool was_active = application_menu_countdown_active_;
    application_menu_countdown_active_ = active;
    if (was_active && !active && application_menu_visible_) {
        schedule_popup_dismiss();
    }
}

void X11Backend::focus_application_menu_items()
{
    if (!application_menu_visible_) {
        return;
    }
    application_menu_focus_ = 2;
    draw_application_menu();
}

void X11Backend::set_application_menu_category(std::string category)
{
    application_menu_active_category_ = std::move(category);
    const auto found = std::find(application_menu_categories_.begin(),
                                 application_menu_categories_.end(),
                                 application_menu_active_category_);
    if (found != application_menu_categories_.end()) {
        application_menu_selected_category_ = static_cast<std::size_t>(
            std::distance(application_menu_categories_.begin(), found));
        update_application_menu_category_scroll();
    }
    draw_application_menu();
}

void X11Backend::set_application_menu_categories(std::vector<std::string> categories)
{
    application_menu_categories_ = std::move(categories);
    if (std::find(application_menu_categories_.begin(), application_menu_categories_.end(),
                  application_menu_active_category_) == application_menu_categories_.end()) {
        application_menu_active_category_ = "All";
    }
    application_menu_selected_category_ = 0;
    application_menu_category_scroll_ = 0;
    draw_application_menu();
}

void X11Backend::close_application_menu()
{
    hide_application_menu(true);
}

bool X11Backend::application_menu_visible() const
{
    return application_menu_visible_;
}

bool X11Backend::application_power_menu_visible() const
{
    return application_power_visible_;
}

void X11Backend::toggle_application_power_menu(
    int anchor_x, int anchor_width,
    std::function<void(const std::string&, const std::string&)> on_power_action)
{
    if (application_power_visible_) {
        hide_application_power_menu();
        return;
    }
    if (application_menu_visible_) {
        hide_application_menu(true);
    }
    application_menu_power_callback_ = std::move(on_power_action);
    application_power_standalone_ = true;
    position_popup(anchor_x, anchor_width, application_power_width_, application_power_height_,
                   application_power_x_, application_power_y_);
    open_application_power_menu();
}

void X11Backend::open_application_power_menu()
{
    if (!application_menu_visible_ && !application_power_standalone_) {
        return;
    }
    application_power_selected_ = 0;
    application_menu_focus_ = 3;
    if (application_power_window_ == XCB_WINDOW_NONE) {
        const uint32_t event_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_FOCUS_CHANGE |
            XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_BUTTON_PRESS |
            XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW |
            XCB_EVENT_MASK_LEAVE_WINDOW;
        const uint32_t values[] = {screen_->white_pixel, 1, event_mask};
        application_power_window_ = xcb_generate_id(connection_);
        xcb_create_window(connection_, XCB_COPY_FROM_PARENT, application_power_window_, screen_->root,
            0, 0, application_power_width_, application_power_height_, 0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT, screen_->root_visual,
            XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
        xcb_visualtype_t* visual = find_visual(screen_, screen_->root_visual);
        if (visual == nullptr) {
            destroy_application_menu();
            return;
        }
        application_power_surface_ = cairo_xcb_surface_create(connection_, application_power_window_,
            visual, application_power_width_, application_power_height_);
        if (cairo_surface_status(application_power_surface_) != CAIRO_STATUS_SUCCESS) {
            destroy_application_menu();
            return;
        }
        application_power_cairo_ = cairo_create(application_power_surface_);
        if (cairo_status(application_power_cairo_) != CAIRO_STATUS_SUCCESS) {
            destroy_application_menu();
            return;
        }
    }
    if (!application_power_standalone_) {
        const int places_width = application_menu_settings_.show_places
            ? application_menu_settings_.places_width : 0;
        const int categories_width = application_menu_settings_.show_categories
            ? application_menu_settings_.categories_width : 0;
        const int categories_x = places_width;
        const int applications_x = categories_x + categories_width;
        application_power_x_ = application_menu_x_ +
            (application_menu_settings_.show_categories ? categories_x + 5 : applications_x + 5);
        application_power_y_ = application_menu_y_ + application_menu_height_ - 47 -
            application_power_height_;
        application_power_x_ = std::clamp(application_power_x_, 0,
            std::max(0, static_cast<int>(screen_->width_in_pixels) - application_power_width_));
        application_power_y_ = std::clamp(application_power_y_, 0,
            std::max(0, static_cast<int>(screen_->height_in_pixels) - application_power_height_));
    }
    draw_application_power_menu();
    const uint32_t coordinates[] = {
        static_cast<uint32_t>(application_power_x_), static_cast<uint32_t>(application_power_y_)
    };
    xcb_configure_window(connection_, application_power_window_,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, coordinates);
    application_power_visible_ = true;
    xcb_map_window(connection_, application_power_window_);
    const uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(connection_, application_power_window_, XCB_CONFIG_WINDOW_STACK_MODE,
                         &stack_mode);
    if (application_power_standalone_) {
        const auto pointer_cookie = xcb_grab_pointer(connection_, 1, application_power_window_,
            XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_POINTER_MOTION |
                XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_WINDOW_NONE, XCB_CURSOR_NONE,
            XCB_CURRENT_TIME);
        xcb_grab_pointer_reply_t* pointer_reply = xcb_grab_pointer_reply(
            connection_, pointer_cookie, nullptr);
        application_power_pointer_grabbed_ = pointer_reply != nullptr &&
            pointer_reply->status == XCB_GRAB_STATUS_SUCCESS;
        free(pointer_reply);
        const auto keyboard_cookie = xcb_grab_keyboard(connection_, 0, application_power_window_,
            XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        xcb_grab_keyboard_reply_t* keyboard_reply = xcb_grab_keyboard_reply(
            connection_, keyboard_cookie, nullptr);
        application_power_keyboard_grabbed_ = keyboard_reply != nullptr &&
            keyboard_reply->status == XCB_GRAB_STATUS_SUCCESS;
        free(keyboard_reply);
    }
    xcb_flush(connection_);
}

void X11Backend::draw_application_power_menu()
{
    if (application_power_cairo_ == nullptr) {
        return;
    }
    static const std::vector<std::pair<std::string, std::string>> actions = {
        {"Lock Screen", "system-lock-screen-symbolic"},
        {"Log Out", "system-log-out-symbolic"},
        {"Suspend", "media-playback-pause-symbolic"},
        {"Hibernate", "weather-clear-night-symbolic"},
        {"Restart", "system-reboot-symbolic"},
        {"Power Off", "system-shutdown-symbolic"}
    };
    cairo_t* cr = application_power_cairo_;
    cairo_set_source_rgb(cr, 245.0 / 255.0, 245.0 / 255.0, 245.0 / 255.0);
    cairo_paint(cr);
    for (std::size_t index = 0; index < actions.size(); ++index) {
        const int y = 4 + static_cast<int>(index) * 28;
        if (static_cast<int>(index) == application_power_selected_) {
            if (index >= 4) {
                cairo_set_source_rgb(cr, 239.0 / 255.0, 212.0 / 255.0, 209.0 / 255.0);
            } else {
                cairo_set_source_rgb(cr, 224.0 / 255.0, 233.0 / 255.0, 210.0 / 255.0);
            }
            cairo_rectangle(cr, 4, y, application_power_width_ - 8, 26);
            cairo_fill(cr);
        }
        paint_theme_icon(cr, actions[index].second, 18, y + 13, 16);
        PangoLayout* layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, actions[index].first.c_str(), -1);
        PangoFontDescription* font = pango_font_description_from_string("Sans 9");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        int text_height = 0;
        pango_layout_get_pixel_size(layout, nullptr, &text_height);
        cairo_set_source_rgb(cr, 37.0 / 255.0, 37.0 / 255.0, 37.0 / 255.0);
        cairo_move_to(cr, 34, y + (28 - text_height) / 2.0);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }
    cairo_set_source_rgb(cr, 119.0 / 255.0, 119.0 / 255.0, 119.0 / 255.0);
    cairo_rectangle(cr, 0.5, 0.5, application_power_width_ - 1.0,
                    application_power_height_ - 1.0);
    cairo_stroke(cr);
    cairo_surface_flush(application_power_surface_);
    xcb_flush(connection_);
}

void X11Backend::hide_application_power_menu()
{
    if (!application_power_visible_ && !application_power_pointer_grabbed_ &&
        !application_power_keyboard_grabbed_) {
        return;
    }
    application_power_visible_ = false;
    if (application_power_pointer_grabbed_ && connection_ != nullptr) {
        xcb_ungrab_pointer(connection_, XCB_CURRENT_TIME);
        application_power_pointer_grabbed_ = false;
    }
    if (application_power_keyboard_grabbed_ && connection_ != nullptr) {
        xcb_ungrab_keyboard(connection_, XCB_CURRENT_TIME);
        application_power_keyboard_grabbed_ = false;
    }
    application_power_standalone_ = false;
    if (application_power_window_ != XCB_WINDOW_NONE && connection_ != nullptr) {
        xcb_unmap_window(connection_, application_power_window_);
    }
    xcb_flush(connection_);
}

void X11Backend::close_application_power_menu()
{
    hide_application_power_menu();
}

void X11Backend::handle_application_power_key(const xcb_key_press_event_t* event)
{
    const auto mapping_cookie = xcb_get_keyboard_mapping(connection_, event->detail, 1);
    xcb_get_keyboard_mapping_reply_t* mapping = xcb_get_keyboard_mapping_reply(
        connection_, mapping_cookie, nullptr);
    if (mapping == nullptr) {
        return;
    }
    const uint32_t* symbols = xcb_get_keyboard_mapping_keysyms(mapping);
    const uint32_t symbol = symbols[(event->state & XCB_MOD_MASK_SHIFT) != 0 &&
        mapping->keysyms_per_keycode > 1 ? 1 : 0];
    free(mapping);
    constexpr uint32_t key_escape = 0xff1b;
    constexpr uint32_t key_tab = 0xff09;
    constexpr uint32_t key_iso_left_tab = 0xfe20;
    constexpr uint32_t key_left = 0xff51;
    constexpr uint32_t key_up = 0xff52;
    constexpr uint32_t key_right = 0xff53;
    constexpr uint32_t key_down = 0xff54;
    constexpr uint32_t key_return = 0xff0d;
    constexpr uint32_t key_kp_enter = 0xff8d;
    if (symbol == key_escape) {
        hide_application_power_menu();
        draw_application_menu();
        return;
    }
    if (symbol == key_tab || symbol == key_iso_left_tab) {
        application_power_selected_ = (application_power_selected_ +
            (symbol == key_iso_left_tab || (event->state & XCB_MOD_MASK_SHIFT) != 0 ? 5 : 1)) % 6;
        draw_application_power_menu();
        return;
    }
    if (symbol == key_left || symbol == key_right) {
        hide_application_power_menu();
        if (symbol == key_left) {
            application_menu_focus_ = application_menu_settings_.show_categories ? 1 :
                application_menu_settings_.show_places ? 4 : 2;
        } else {
            application_menu_focus_ = 2;
        }
        draw_application_menu();
        return;
    }
    if (symbol == key_up || symbol == key_down) {
        application_power_selected_ = (application_power_selected_ +
            (symbol == key_down ? 1 : 5)) % 6;
        draw_application_power_menu();
        return;
    }
    if (symbol == key_return || symbol == key_kp_enter) {
        static const std::vector<std::pair<std::string, std::string>> actions = {
            {"lock", "Lock Screen"}, {"logout", "Log Out"}, {"suspend", "Suspend"},
            {"hibernate", "Hibernate"}, {"reboot", "Restart"}, {"shutdown", "Power Off"}
        };
        const auto action = actions[static_cast<std::size_t>(application_power_selected_)];
        hide_application_power_menu();
        if (application_menu_power_callback_) {
            application_menu_power_callback_(action.first, action.second);
        }
    }
}

void X11Backend::handle_application_power_click(int, int, int root_x, int root_y,
                                               uint8_t button)
{
    const bool inside = root_x >= application_power_x_ &&
        root_x < application_power_x_ + application_power_width_ &&
        root_y >= application_power_y_ &&
        root_y < application_power_y_ + application_power_height_;
    if (!inside) {
        hide_application_power_menu();
        return;
    }
    if (button != 1) {
        return;
    }
    const int row = (root_y - application_power_y_ - 4) / 28;
    if (row < 0 || row >= 6) {
        return;
    }
    static const std::vector<std::pair<std::string, std::string>> actions = {
        {"lock", "Lock Screen"}, {"logout", "Log Out"}, {"suspend", "Suspend"},
        {"hibernate", "Hibernate"}, {"reboot", "Restart"}, {"shutdown", "Power Off"}
    };
    const auto action = actions[static_cast<std::size_t>(row)];
    hide_application_power_menu();
    if (application_menu_power_callback_) {
        application_menu_power_callback_(action.first, action.second);
    }
}

void X11Backend::handle_application_power_motion(int y)
{
    const int row = (y - 4) / 28;
    if (row >= 0 && row < 6 && row != application_power_selected_) {
        application_power_selected_ = row;
        draw_application_power_menu();
    }
}

void X11Backend::draw_application_menu(int redraw_column)
{
    if (application_menu_cairo_ == nullptr) {
        return;
    }
    cairo_t* cr = application_menu_cairo_;
    const int category_width = application_menu_settings_.categories_width;
    const int visible_category_width = application_menu_settings_.show_categories
        ? category_width : 0;
    const int places_width = application_menu_settings_.show_places
        ? application_menu_settings_.places_width : 0;
    const int category_x = places_width;
    const int applications_x = category_x + visible_category_width;
    const int footer_height = application_menu_settings_.show_categories ? 42 : 78;
    constexpr int list_top = 8;
    constexpr int category_top = 34;
    const int list_bottom = application_menu_height_ - footer_height;
    const int visible_rows = std::max(1, (list_bottom - list_top) / application_menu_row_height_);

    if (redraw_column != 0) {
        const int clip_x = redraw_column == 1 ? category_x :
            redraw_column == 2 ? applications_x : 0;
        const int clip_y = redraw_column == 1 ? category_top :
            redraw_column == 2 ? list_top : 0;
        const int clip_width = redraw_column == 1 ? category_width :
            redraw_column == 2 ? application_menu_width_ - applications_x : places_width;
        const int clip_height = redraw_column == 1 ? list_bottom - category_top :
            redraw_column == 2 ? list_bottom - list_top : list_bottom;
        cairo_save(cr);
        cairo_rectangle(cr, clip_x, clip_y, clip_width, clip_height);
        cairo_clip(cr);
    }

    cairo_set_source_rgb(cr, 245.0 / 255.0, 245.0 / 255.0, 245.0 / 255.0);
    cairo_paint(cr);
    if (application_menu_settings_.show_categories) {
        cairo_set_source_rgb(cr, 238.0 / 255.0, 238.0 / 255.0, 238.0 / 255.0);
        cairo_rectangle(cr, category_x, 0, category_width, list_bottom);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 211.0 / 255.0, 211.0 / 255.0, 211.0 / 255.0);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, category_x + category_width + 0.5, 0);
        cairo_line_to(cr, category_x + category_width + 0.5, application_menu_height_);
        cairo_stroke(cr);
    }
    if (places_width > 0) {
        cairo_set_source_rgb(cr, 211.0 / 255.0, 211.0 / 255.0, 211.0 / 255.0);
        cairo_move_to(cr, places_width + 0.5, 0);
        cairo_line_to(cr, places_width + 0.5, list_bottom);
        cairo_stroke(cr);
    }

    auto draw_text = [cr](const std::string& text, const char* font_name,
                          double x, double y, double width, bool centered) {
        PangoLayout* layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, text.c_str(), -1);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        if (width > 0) {
            pango_layout_set_width(layout, static_cast<int>(width * PANGO_SCALE));
        }
        PangoFontDescription* font = pango_font_description_from_string(font_name);
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        int text_width = 0;
        int text_height = 0;
        pango_layout_get_pixel_size(layout, &text_width, &text_height);
        cairo_move_to(cr, centered ? x + (width - text_width) / 2.0 : x,
                      y + (24.0 - text_height) / 2.0);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    };

    const auto draw_scrollbar = [cr](int x, int y, int track_height,
                                      std::size_t content_count,
                                      std::size_t visible_count, std::size_t scroll) {
        if (track_height <= 0 || content_count <= visible_count) return;
        const std::size_t maximum = content_count - visible_count;
        const int thumb_height = std::min(track_height, std::max(22,
            static_cast<int>(static_cast<std::size_t>(track_height) * visible_count /
                             content_count)));
        const int travel = track_height - thumb_height;
        const int thumb_y = y + static_cast<int>(
            static_cast<std::size_t>(travel) * scroll / maximum);
        cairo_set_source_rgb(cr, 0.88, 0.89, 0.90);
        cairo_rectangle(cr, x, y, 15, track_height);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.53, 0.56, 0.58);
        cairo_rectangle(cr, x, thumb_y, 15, thumb_height);
        cairo_fill(cr);
    };

    if (application_menu_settings_.show_categories) {
        cairo_set_source_rgb(cr, 76.0 / 255.0, 76.0 / 255.0, 76.0 / 255.0);
        draw_text("Applications", "Sans Bold 10", category_x + 8, 5,
                  category_width - 25, false);
    }
    if (application_menu_settings_.show_categories) {
    const int category_visible_rows = std::max(1, (list_bottom - category_top) / 27);
    for (int row = 0; row < category_visible_rows; ++row) {
        const std::size_t index = application_menu_category_scroll_ +
            static_cast<std::size_t>(row);
        if (index >= application_menu_categories_.size()) break;
        const int y = category_top + row * 27;
        const bool active = application_menu_categories_[index] == application_menu_active_category_;
        const bool focused = application_menu_focus_ == 1 && index == application_menu_selected_category_;
        if (active || focused) {
            cairo_set_source_rgb(cr, 219.0 / 255.0, 232.0 / 255.0, 202.0 / 255.0);
            cairo_rectangle(cr, category_x + 4, y, category_width - 8, 25);
            cairo_fill(cr);
        }
        if (focused) {
            cairo_set_source_rgb(cr, 79.0 / 255.0, 127.0 / 255.0, 23.0 / 255.0);
            cairo_rectangle(cr, category_x + 4.5, y + 0.5, category_width - 9, 24);
            cairo_stroke(cr);
        }
        paint_theme_icon(cr, application_menu_category_icon(
            application_menu_categories_[index]), category_x + 17, y + 12.5, 16);
        cairo_set_source_rgb(cr, 41.0 / 255.0, 41.0 / 255.0, 41.0 / 255.0);
        draw_text(application_menu_categories_[index], "Sans 9", category_x + 29, y,
                  category_width - 46, false);
    }
    draw_scrollbar(category_x + category_width - 17, category_top,
        list_bottom - category_top, application_menu_categories_.size(),
        static_cast<std::size_t>(category_visible_rows),
        application_menu_category_scroll_);
    }

    for (int row = 0; row < visible_rows; ++row) {
        const std::size_t index = application_menu_scroll_ + static_cast<std::size_t>(row);
        if (index >= application_menu_items_.size()) {
            break;
        }
        const int y = list_top + row * application_menu_row_height_;
        if (index == application_menu_selected_) {
            cairo_set_source_rgb(cr, 220.0 / 255.0, 233.0 / 255.0, 203.0 / 255.0);
            cairo_rectangle(cr, applications_x + 4, y,
                            application_menu_width_ - applications_x - 14,
                            application_menu_row_height_ - 1);
            cairo_fill(cr);
            if (application_menu_focus_ == 2) {
                cairo_set_source_rgb(cr, 79.0 / 255.0, 127.0 / 255.0, 23.0 / 255.0);
                cairo_rectangle(cr, applications_x + 4.5, y + 0.5,
                    application_menu_width_ - applications_x - 15,
                    application_menu_row_height_ - 2);
                cairo_stroke(cr);
            }
        }
        paint_theme_icon(cr, application_menu_items_[index].icon_name,
                         applications_x + 19, y + application_menu_row_height_ / 2.0, 18);
        cairo_set_source_rgb(cr, 37.0 / 255.0, 37.0 / 255.0, 37.0 / 255.0);
        draw_text(application_menu_items_[index].label, "Sans 10", applications_x + 34,
                  y, application_menu_width_ - applications_x - 61, false);
    }
    draw_scrollbar(application_menu_width_ - 17, list_top, list_bottom - list_top,
        application_menu_items_.size(), static_cast<std::size_t>(visible_rows),
        application_menu_scroll_);

    if (places_width > 0) {
        cairo_save(cr);
        cairo_rectangle(cr, 0, 0, places_width, list_bottom);
        cairo_clip(cr);
        const auto draw_shortcuts = [&](const std::string& heading,
                                        const std::vector<ApplicationMenuDisplayItem>& entries,
                                        int heading_y, bool system) {
            cairo_set_source_rgb(cr, 76.0 / 255.0, 76.0 / 255.0, 76.0 / 255.0);
            draw_text(heading, "Sans Bold 9", 10,
                      heading_y - application_menu_shortcut_scroll_,
                      places_width - 20, false);
            int row_y = heading_y + 26;
            for (std::size_t entry_index = 0; entry_index < entries.size(); ++entry_index) {
                const auto& entry = entries[entry_index];
                const int visible_y = row_y - application_menu_shortcut_scroll_;
                const std::size_t absolute_index = system
                    ? application_menu_settings_.places.size() + entry_index : entry_index;
                if (visible_y + 25 >= 0 && visible_y < list_bottom &&
                    application_menu_focus_ == 4 &&
                    absolute_index == application_menu_selected_shortcut_) {
                    cairo_set_source_rgb(cr, 220.0 / 255.0, 233.0 / 255.0, 203.0 / 255.0);
                    cairo_rectangle(cr, 4, visible_y, places_width - 8, 25);
                    cairo_fill(cr);
                }
                if (visible_y + 25 >= 0 && visible_y < list_bottom) {
                    paint_theme_icon(cr, entry.icon_name, 18, visible_y + 12.5, 16);
                    cairo_set_source_rgb(cr, 41.0 / 255.0, 41.0 / 255.0, 41.0 / 255.0);
                    draw_text(entry.label, "Sans 9", 31, visible_y,
                              places_width - 38, false);
                }
                row_y += 27;
            }
            return row_y;
        };
        const int next_heading_y = draw_shortcuts(
            "Places", application_menu_settings_.places, 8, false);
        if (!application_menu_settings_.system.empty()) {
            draw_shortcuts("System", application_menu_settings_.system,
                           next_heading_y + 5, true);
        }
        cairo_restore(cr);
    }

    const int footer_y = application_menu_height_ - footer_height;
    cairo_set_source_rgb(cr, 238.0 / 255.0, 238.0 / 255.0, 238.0 / 255.0);
    if (application_menu_settings_.show_categories) {
        cairo_rectangle(cr, category_x, footer_y, category_width, footer_height);
        cairo_fill(cr);
        cairo_rectangle(cr, applications_x, footer_y,
                        application_menu_width_ - applications_x, footer_height);
        cairo_fill(cr);
        const bool power_focused = application_menu_focus_ == 3;
        cairo_set_source_rgb(cr, 111.0 / 255.0, 157.0 / 255.0, 46.0 / 255.0);
        cairo_rectangle(cr, category_x + 5, footer_y + 4,
                        category_width - 10, footer_height - 8);
        cairo_fill(cr);
        if (power_focused) {
            cairo_set_source_rgb(cr, 32.0 / 255.0, 53.0 / 255.0, 5.0 / 255.0);
            cairo_rectangle(cr, category_x + 5.5, footer_y + 4.5,
                            category_width - 11, footer_height - 9);
            cairo_stroke(cr);
        }
        paint_theme_icon(cr, "system-shutdown-symbolic", category_x + 20,
                         footer_y + footer_height / 2.0, 16);
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        draw_text("Power", "Sans Bold 9", category_x + 31,
              footer_y + (footer_height - 24) / 2.0,
                  category_width - 44, false);
    } else {
        cairo_rectangle(cr, applications_x, footer_y,
                        application_menu_width_ - applications_x, footer_height);
        cairo_fill(cr);
        const int power_x = applications_x + 5;
        const int power_width = application_menu_width_ - applications_x - 10;
        cairo_set_source_rgb(cr, 111.0 / 255.0, 157.0 / 255.0, 46.0 / 255.0);
        cairo_rectangle(cr, power_x, footer_y + 3, power_width, 27);
        cairo_fill(cr);
        if (application_menu_focus_ == 3) {
            cairo_set_source_rgb(cr, 32.0 / 255.0, 53.0 / 255.0, 5.0 / 255.0);
            cairo_rectangle(cr, power_x + 0.5, footer_y + 3.5, power_width - 1, 26);
            cairo_stroke(cr);
        }
        paint_theme_icon(cr, "system-shutdown-symbolic", power_x + 16,
                         footer_y + 16.5, 16);
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        draw_text("Power", "Sans Bold 9", power_x + 28,
                  footer_y + 3 + (27 - 24) / 2.0,
                  power_width - 36, false);
    }

    const int search_x = applications_x + 7;
    constexpr int search_box_height = 32;
    const int search_y = application_menu_settings_.show_categories
        ? footer_y + 5 : footer_y + 34;
    const int search_width = application_menu_width_ - applications_x - 14;
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_rectangle(cr, search_x, search_y, search_width, search_box_height);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, application_menu_focus_ == 0 ? 118.0 / 255.0 : 181.0 / 255.0,
                         application_menu_focus_ == 0 ? 159.0 / 255.0 : 181.0 / 255.0,
                         application_menu_focus_ == 0 ? 49.0 / 255.0 : 181.0 / 255.0);
    cairo_rectangle(cr, search_x + 0.5, search_y + 0.5,
                    search_width - 1, search_box_height - 1);
    cairo_stroke(cr);
    paint_theme_icon(cr, "system-search-symbolic", search_x + 14,
                     search_y + search_box_height / 2.0, 16);
    cairo_set_source_rgb(cr, application_menu_query_.empty() ? 0.45 : 32.0 / 255.0,
                         application_menu_query_.empty() ? 0.45 : 32.0 / 255.0,
                         application_menu_query_.empty() ? 0.45 : 32.0 / 255.0);
    draw_text(application_menu_query_.empty() ? "Search applications..." : application_menu_query_,
              "Sans 9", search_x + 28,
              search_y + (search_box_height - 24) / 2, search_width - 35, false);
    if (application_menu_focus_ == 0) {
        PangoLayout* layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, application_menu_query_.empty()
            ? "Search applications..." : application_menu_query_.c_str(), -1);
        PangoFontDescription* font = pango_font_description_from_string("Sans 9");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        int text_width = 0;
        pango_layout_get_pixel_size(layout, &text_width, nullptr);
        const int cursor_x = search_x + 29 + std::min(text_width, search_width - 39);
        cairo_set_source_rgb(cr, 32.0 / 255.0, 32.0 / 255.0, 32.0 / 255.0);
        cairo_move_to(cr, cursor_x, search_y + (search_box_height - 16) / 2);
        cairo_line_to(cr, cursor_x, search_y + (search_box_height + 16) / 2);
        cairo_stroke(cr);
        g_object_unref(layout);
    }
    cairo_set_source_rgb(cr, 119.0 / 255.0, 119.0 / 255.0, 119.0 / 255.0);
    cairo_rectangle(cr, 0.5, 0.5, application_menu_width_ - 1.0, application_menu_height_ - 1.0);
    cairo_stroke(cr);
    if (redraw_column != 0) cairo_restore(cr);
    cairo_surface_flush(application_menu_surface_);
    xcb_flush(connection_);
}

void X11Backend::hide_application_menu(bool notify)
{
    cancel_popup_dismiss();
    if (!application_menu_visible_ && !application_menu_pointer_grabbed_ &&
        !application_menu_keyboard_grabbed_ && !application_menu_previous_focus_valid_) {
        return;
    }
    const bool was_visible = application_menu_visible_;
    application_menu_visible_ = false;
    hide_application_power_menu();
    if (application_menu_pointer_grabbed_ && connection_ != nullptr) {
        xcb_ungrab_pointer(connection_, XCB_CURRENT_TIME);
        application_menu_pointer_grabbed_ = false;
    }
    if (application_menu_keyboard_grabbed_ && connection_ != nullptr) {
        xcb_ungrab_keyboard(connection_, XCB_CURRENT_TIME);
        application_menu_keyboard_grabbed_ = false;
    }
    if (application_menu_window_ != XCB_WINDOW_NONE && connection_ != nullptr) {
        xcb_unmap_window(connection_, application_menu_window_);
    }
    if (application_menu_previous_focus_valid_ && connection_ != nullptr) {
        xcb_set_input_focus(connection_, XCB_INPUT_FOCUS_PARENT,
                            application_menu_previous_focus_, XCB_CURRENT_TIME);
    }
    application_menu_previous_focus_ = XCB_NONE;
    application_menu_previous_focus_valid_ = false;
    if (connection_ != nullptr) xcb_flush(connection_);
    if (notify && was_visible && application_menu_close_callback_) {
        application_menu_close_callback_();
    }
}

void X11Backend::handle_popup_crossing(xcb_window_t window, bool entering,
                                       uint8_t mode, uint8_t detail)
{
    if (mode != XCB_NOTIFY_MODE_NORMAL || detail == XCB_NOTIFY_DETAIL_INFERIOR) {
        return;
    }

    const bool has_popup = application_menu_visible_ || application_power_visible_ ||
        calendar_visible_ || desktop_hub_visible_ || !task_menu_target_.empty();
    const bool is_popup_surface = window == window_ ||
        (application_menu_visible_ && window == application_menu_window_) ||
        (application_power_visible_ && window == application_power_window_) ||
        (calendar_visible_ && window == calendar_window_) ||
        (desktop_hub_visible_ && window == desktop_hub_window_) ||
        (!task_menu_target_.empty() && window == task_menu_window_);
    if (!has_popup || !is_popup_surface) {
        return;
    }

    if (entering) {
        cancel_popup_dismiss();
    } else {
        schedule_popup_dismiss();
    }
}

void X11Backend::schedule_popup_dismiss()
{
    if (popup_leave_timeout_ != 0) {
        return;
    }
    popup_leave_timeout_ = g_timeout_add(75, [](gpointer data) -> gboolean {
        auto* backend = static_cast<X11Backend*>(data);
        backend->popup_leave_timeout_ = 0;
        const auto pointer_cookie = xcb_query_pointer(
            backend->connection_, backend->screen_->root);
        xcb_query_pointer_reply_t* pointer_reply = xcb_query_pointer_reply(
            backend->connection_, pointer_cookie, nullptr);
        xcb_window_t pointer_child = XCB_WINDOW_NONE;
        if (pointer_reply != nullptr) {
            pointer_child = pointer_reply->child;
        }
        free(pointer_reply);
        const bool pointer_over_popup = pointer_child == backend->window_ ||
            (backend->application_menu_visible_ &&
             pointer_child == backend->application_menu_window_) ||
            (backend->application_power_visible_ &&
             pointer_child == backend->application_power_window_) ||
            (backend->calendar_visible_ && pointer_child == backend->calendar_window_) ||
            (backend->desktop_hub_visible_ &&
             pointer_child == backend->desktop_hub_window_) ||
            (!backend->task_menu_target_.empty() &&
             pointer_child == backend->task_menu_window_);
        if (pointer_over_popup) {
            return G_SOURCE_REMOVE;
        }
        if (backend->application_menu_visible_ &&
            !backend->application_menu_countdown_active_) {
            backend->hide_application_menu(true);
        }
        if (backend->application_power_visible_) {
            backend->hide_application_power_menu();
        }
        if (backend->calendar_visible_) {
            backend->hide_calendar();
        }
        if (backend->desktop_hub_visible_ &&
            !backend->desktop_hub_countdown_active_) {
            backend->hide_desktop_hub(true);
        }
        if (!backend->task_menu_target_.empty()) {
            backend->hide_task_menu();
        }
        return G_SOURCE_REMOVE;
    }, this);
}

void X11Backend::cancel_popup_dismiss()
{
    if (popup_leave_timeout_ == 0) {
        return;
    }
    g_source_remove(popup_leave_timeout_);
    popup_leave_timeout_ = 0;
}

void X11Backend::destroy_application_menu()
{
    hide_application_power_menu();
    hide_application_menu(false);
    if (application_power_cairo_ != nullptr) {
        cairo_destroy(application_power_cairo_);
        application_power_cairo_ = nullptr;
    }
    if (application_power_surface_ != nullptr) {
        cairo_surface_destroy(application_power_surface_);
        application_power_surface_ = nullptr;
    }
    if (connection_ != nullptr && application_power_window_ != XCB_WINDOW_NONE) {
        xcb_destroy_window(connection_, application_power_window_);
        application_power_window_ = XCB_WINDOW_NONE;
    }
    if (application_menu_cairo_ != nullptr) {
        cairo_destroy(application_menu_cairo_);
        application_menu_cairo_ = nullptr;
    }
    if (application_menu_surface_ != nullptr) {
        cairo_surface_destroy(application_menu_surface_);
        application_menu_surface_ = nullptr;
    }
    if (connection_ != nullptr && application_menu_window_ != XCB_WINDOW_NONE) {
        xcb_destroy_window(connection_, application_menu_window_);
        xcb_flush(connection_);
    }
    application_menu_window_ = XCB_WINDOW_NONE;
}

void X11Backend::select_application_menu_category(const std::string& category)
{
    application_menu_active_category_ = category;
    if (category != "Power") {
        const auto found = std::find(application_menu_categories_.begin(),
                                     application_menu_categories_.end(), category);
        if (found != application_menu_categories_.end()) {
            application_menu_selected_category_ = static_cast<std::size_t>(
                std::distance(application_menu_categories_.begin(), found));
        }
    }
    update_application_menu_category_scroll();
    application_menu_selected_ = 0;
    application_menu_scroll_ = 0;
    if (application_menu_category_callback_) {
        application_menu_category_callback_(category);
    }
    draw_application_menu();
}

void X11Backend::update_application_menu_scroll()
{
    constexpr int list_top = 8;
    const int footer_height = application_menu_settings_.show_categories ? 42 : 78;
    const std::size_t visible_rows = static_cast<std::size_t>(std::max(1,
        (application_menu_height_ - footer_height - list_top) / application_menu_row_height_));
    if (application_menu_selected_ < application_menu_scroll_) {
        application_menu_scroll_ = application_menu_selected_;
    } else if (application_menu_selected_ >= application_menu_scroll_ + visible_rows) {
        application_menu_scroll_ = application_menu_selected_ - visible_rows + 1;
    }
}

void X11Backend::update_application_menu_category_scroll()
{
    constexpr int category_top = 34;
    const int footer_height = application_menu_settings_.show_categories ? 42 : 78;
    const int list_bottom = application_menu_height_ - footer_height;
    const std::size_t visible_rows = static_cast<std::size_t>(std::max(1,
        (list_bottom - category_top) / 27));
    if (application_menu_selected_category_ < application_menu_category_scroll_) {
        application_menu_category_scroll_ = application_menu_selected_category_;
    } else if (application_menu_selected_category_ >=
               application_menu_category_scroll_ + visible_rows) {
        application_menu_category_scroll_ =
            application_menu_selected_category_ - visible_rows + 1;
    }
    const std::size_t maximum = application_menu_categories_.size() > visible_rows
        ? application_menu_categories_.size() - visible_rows : 0;
    application_menu_category_scroll_ = std::min(application_menu_category_scroll_, maximum);
}

void X11Backend::update_application_menu_scrollbar_drag(int y)
{
    if (application_menu_scrollbar_drag_column_ == 0) return;
    const int footer_height = application_menu_settings_.show_categories ? 42 : 78;
    const int list_bottom = application_menu_height_ - footer_height;
    int track_y = 8;
    int track_height = list_bottom - track_y;
    std::size_t content_count = application_menu_items_.size();
    std::size_t visible_count = static_cast<std::size_t>(std::max(1,
        (list_bottom - 8) / application_menu_row_height_));
    std::size_t* scroll = &application_menu_scroll_;
    if (application_menu_scrollbar_drag_column_ == 1) {
        track_y = 34;
        track_height = list_bottom - track_y;
        content_count = application_menu_categories_.size();
        visible_count = static_cast<std::size_t>(std::max(1, track_height / 27));
        scroll = &application_menu_category_scroll_;
    }
    if (track_height <= 0 || content_count <= visible_count) return;
    const std::size_t maximum = content_count - visible_count;
    const int thumb_height = std::min(track_height, std::max(22,
        static_cast<int>(static_cast<std::size_t>(track_height) * visible_count /
                         content_count)));
    const int travel = track_height - thumb_height;
    if (travel <= 0) return;
    const int thumb_y = std::clamp(y - application_menu_scrollbar_drag_offset_,
                                   track_y, track_y + travel);
    *scroll = static_cast<std::size_t>(thumb_y - track_y) * maximum /
        static_cast<std::size_t>(travel);
    draw_application_menu(application_menu_scrollbar_drag_column_);
}

void X11Backend::update_application_menu_shortcut_scroll()
{
    const std::size_t places_count = application_menu_settings_.places.size();
    const std::size_t selected = application_menu_selected_shortcut_;
    int item_y = 34 + static_cast<int>(selected) * 27;
    if (selected >= places_count) {
        item_y = 65 + static_cast<int>(places_count) * 27 +
            static_cast<int>(selected - places_count) * 27;
    }
    const int list_bottom = application_menu_height_ -
        (application_menu_settings_.show_categories ? 42 : 78);
    if (item_y - application_menu_shortcut_scroll_ < 34) {
        application_menu_shortcut_scroll_ = item_y - 34;
    } else if (item_y + 25 - application_menu_shortcut_scroll_ > list_bottom) {
        application_menu_shortcut_scroll_ = item_y + 25 - list_bottom;
    }
    const int content_bottom = 65 + static_cast<int>(places_count) * 27 +
        static_cast<int>(application_menu_settings_.system.size()) * 27;
    application_menu_shortcut_scroll_ = std::clamp(application_menu_shortcut_scroll_, 0,
        std::max(0, content_bottom - list_bottom));
}

void X11Backend::handle_application_menu_key(const xcb_key_press_event_t* event)
{
    if (application_power_visible_) {
        handle_application_power_key(event);
        return;
    }
    const auto mapping_cookie = xcb_get_keyboard_mapping(connection_, event->detail, 1);
    xcb_get_keyboard_mapping_reply_t* mapping = xcb_get_keyboard_mapping_reply(
        connection_, mapping_cookie, nullptr);
    if (mapping == nullptr) {
        return;
    }
    const uint32_t* symbols = xcb_get_keyboard_mapping_keysyms(mapping);
    int symbol_index = (event->state & XCB_MOD_MASK_SHIFT) != 0 &&
        mapping->keysyms_per_keycode > 1 ? 1 : 0;
    if ((event->state & XCB_MOD_MASK_LOCK) != 0 && mapping->keysyms_per_keycode > 1) {
        const uint32_t base = symbols[0];
        if ((base >= 'a' && base <= 'z') || (base >= 'A' && base <= 'Z')) {
            symbol_index = symbol_index == 0 ? 1 : 0;
        }
    }
    const uint32_t symbol = symbols[symbol_index];
    free(mapping);

    constexpr uint32_t key_backspace = 0xff08;
    constexpr uint32_t key_return = 0xff0d;
    constexpr uint32_t key_escape = 0xff1b;
    constexpr uint32_t key_tab = 0xff09;
    constexpr uint32_t key_iso_left_tab = 0xfe20;
    constexpr uint32_t key_left = 0xff51;
    constexpr uint32_t key_up = 0xff52;
    constexpr uint32_t key_right = 0xff53;
    constexpr uint32_t key_down = 0xff54;
    constexpr uint32_t key_kp_enter = 0xff8d;
    if (symbol == key_escape) {
        hide_application_menu(true);
        return;
    }
    if (symbol == key_tab || symbol == key_iso_left_tab) {
        application_menu_tab_held_ = true;
        return;
    }
    if (symbol == key_left) {
        if (application_menu_tab_held_ && application_menu_settings_.show_places &&
            (!application_menu_settings_.places.empty() ||
             !application_menu_settings_.system.empty())) {
            application_menu_focus_ = 4;
            application_menu_selected_shortcut_ = 0;
            application_menu_shortcut_scroll_ = 0;
        } else {
            if (application_menu_settings_.show_categories) {
                application_menu_focus_ = 1;
            } else if (application_menu_settings_.show_places &&
                       (!application_menu_settings_.places.empty() ||
                        !application_menu_settings_.system.empty())) {
                application_menu_focus_ = 4;
                application_menu_selected_shortcut_ = 0;
                application_menu_shortcut_scroll_ = 0;
            } else {
                application_menu_focus_ = 2;
            }
        }
        draw_application_menu();
        return;
    }
    if (symbol == key_right) {
        application_menu_focus_ = 2;
        draw_application_menu();
        return;
    }
    if (symbol == key_up || symbol == key_down) {
        const int direction = symbol == key_up ? -1 : 1;
        if (application_menu_focus_ == 4) {
            const std::size_t count = application_menu_settings_.places.size() +
                application_menu_settings_.system.size();
            if (count > 0) {
                if (direction < 0 && application_menu_selected_shortcut_ > 0) {
                    --application_menu_selected_shortcut_;
                } else if (direction > 0 && application_menu_selected_shortcut_ + 1 < count) {
                    ++application_menu_selected_shortcut_;
                }
                update_application_menu_shortcut_scroll();
                draw_application_menu();
            }
        } else if (application_menu_focus_ == 1 &&
               application_menu_settings_.show_categories &&
               !application_menu_categories_.empty()) {
            const std::size_t count = application_menu_categories_.size();
            if (direction < 0 && application_menu_selected_category_ > 0) {
                --application_menu_selected_category_;
            } else if (direction > 0 && application_menu_selected_category_ + 1 < count) {
                ++application_menu_selected_category_;
            } else if (direction > 0) {
                application_menu_focus_ = 3;
                draw_application_menu();
                return;
            }
            select_application_menu_category(
                application_menu_categories_[application_menu_selected_category_]);
        } else if (application_menu_focus_ == 3 && direction < 0) {
            if (application_menu_settings_.show_categories &&
                !application_menu_categories_.empty()) {
                application_menu_focus_ = 1;
                application_menu_selected_category_ = application_menu_categories_.size() - 1;
                select_application_menu_category(
                    application_menu_categories_[application_menu_selected_category_]);
            } else if (!application_menu_items_.empty()) {
                application_menu_focus_ = 2;
                application_menu_selected_ = application_menu_items_.size() - 1;
                update_application_menu_scroll();
                draw_application_menu();
            }
        } else if (!application_menu_items_.empty()) {
            application_menu_focus_ = 2;
            if (direction < 0 && application_menu_selected_ > 0) {
                --application_menu_selected_;
            } else if (direction > 0 && application_menu_selected_ + 1 < application_menu_items_.size()) {
                ++application_menu_selected_;
            } else if (direction > 0 && !application_menu_settings_.show_categories) {
                application_menu_focus_ = 3;
            }
            update_application_menu_scroll();
            draw_application_menu(2);
        }
        return;
    }
    if (symbol == key_return || symbol == key_kp_enter ||
        (symbol == ' ' && application_menu_focus_ != 0)) {
        if (application_menu_focus_ == 3) {
            open_application_power_menu();
        } else if (application_menu_focus_ == 1) {
            application_menu_focus_ = 2;
            draw_application_menu();
        } else if (application_menu_focus_ == 4 && application_menu_shortcut_callback_) {
            const std::size_t places_count = application_menu_settings_.places.size();
            if (application_menu_selected_shortcut_ < places_count) {
                application_menu_shortcut_callback_(false,
                    application_menu_selected_shortcut_);
            } else {
                application_menu_shortcut_callback_(true,
                    application_menu_selected_shortcut_ - places_count);
            }
        } else if (application_menu_selected_ < application_menu_items_.size() &&
                   application_menu_activate_callback_) {
            application_menu_activate_callback_(application_menu_selected_);
        }
        return;
    }

    if (symbol == key_backspace) {
        if (!application_menu_query_.empty()) {
            std::size_t start = application_menu_query_.size() - 1;
            while (start > 0 &&
                   (static_cast<unsigned char>(application_menu_query_[start]) & 0xc0) == 0x80) {
                --start;
            }
            application_menu_query_.erase(start);
            application_menu_focus_ = 0;
            application_menu_active_category_ = "All";
            application_menu_selected_category_ = 0;
            application_menu_category_scroll_ = 0;
            if (application_menu_search_callback_) {
                application_menu_search_callback_(application_menu_query_);
            }
        }
        return;
    }
    const uint16_t blocked = XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_4;
    if ((event->state & blocked) != 0) {
        return;
    }
    uint32_t codepoint = symbol <= 0xff ? symbol :
        ((symbol & 0xff000000U) == 0x01000000U ? symbol & 0x00ffffffU : 0);
    if (codepoint == 0 || !g_unichar_isprint(codepoint)) {
        return;
    }
    gchar encoded[7]{};
    const int length = g_unichar_to_utf8(codepoint, encoded);
    application_menu_query_.append(encoded, static_cast<std::size_t>(length));
    application_menu_active_category_ = "All";
    application_menu_selected_category_ = 0;
    application_menu_category_scroll_ = 0;
    application_menu_focus_ = 0;
    if (application_menu_search_callback_) {
        application_menu_search_callback_(application_menu_query_);
    }
}

void X11Backend::handle_application_menu_click(int x, int y, int root_x, int root_y,
                                              uint8_t button)
{
    if (application_power_visible_) {
        const bool in_power_menu = root_x >= application_power_x_ &&
            root_x < application_power_x_ + application_power_width_ &&
            root_y >= application_power_y_ &&
            root_y < application_power_y_ + application_power_height_;
        if (in_power_menu && button == 1) {
            const int row = (root_y - application_power_y_ - 4) / 28;
            if (row >= 0 && row < 6) {
                static const std::vector<std::pair<std::string, std::string>> actions = {
                    {"lock", "Lock Screen"}, {"logout", "Log Out"}, {"suspend", "Suspend"},
                    {"hibernate", "Hibernate"}, {"reboot", "Restart"}, {"shutdown", "Power Off"}
                };
                const auto action = actions[static_cast<std::size_t>(row)];
                hide_application_power_menu();
                if (application_menu_power_callback_) {
                    application_menu_power_callback_(action.first, action.second);
                }
            }
            return;
        }
        hide_application_power_menu();
        const bool in_main_menu = x >= 0 && x < application_menu_width_ &&
            y >= 0 && y < application_menu_height_;
        if (!in_main_menu) {
            hide_application_menu(true);
            return;
        }
    }
    const bool inside_main_menu = root_x >= application_menu_x_ &&
        root_x < application_menu_x_ + application_menu_width_ &&
        root_y >= application_menu_y_ &&
        root_y < application_menu_y_ + application_menu_height_;
    if (!inside_main_menu) {
        hide_application_menu(true);
        return;
    }
    if (button == 4 || button == 5) {
        const int category_width = application_menu_settings_.categories_width;
        const int visible_category_width = application_menu_settings_.show_categories
            ? category_width : 0;
        const int places_width = application_menu_settings_.show_places
            ? application_menu_settings_.places_width : 0;
        const int category_x = places_width;
        const int applications_x = category_x + visible_category_width;
        if (places_width > 0 && x < places_width) {
            const int list_bottom = application_menu_height_ -
                (application_menu_settings_.show_categories ? 42 : 78);
            const int content_bottom = 65 +
                static_cast<int>(application_menu_settings_.places.size() +
                                 application_menu_settings_.system.size()) * 27;
            const int maximum = std::max(0, content_bottom - list_bottom);
            application_menu_shortcut_scroll_ = std::clamp(
                application_menu_shortcut_scroll_ + (button == 4 ? -54 : 54), 0, maximum);
            draw_application_menu(3);
            return;
        }
        const int footer_height = application_menu_settings_.show_categories ? 42 : 78;
        const int list_bottom = application_menu_height_ - footer_height;
        if (application_menu_settings_.show_categories && x >= category_x &&
            x < applications_x && y >= 34 && y < list_bottom) {
            const std::size_t visible_rows = static_cast<std::size_t>(std::max(1,
                (list_bottom - 34) / 27));
            const std::size_t maximum = application_menu_categories_.size() > visible_rows
                ? application_menu_categories_.size() - visible_rows : 0;
            if (button == 4 && application_menu_category_scroll_ > 0) {
                --application_menu_category_scroll_;
            } else if (button == 5 && application_menu_category_scroll_ < maximum) {
                ++application_menu_category_scroll_;
            }
            draw_application_menu(1);
            return;
        }
        if (x < applications_x) return;
        const std::size_t visible_rows = static_cast<std::size_t>(std::max(1,
            (application_menu_height_ - footer_height - 8) / application_menu_row_height_));
        const std::size_t max_scroll = application_menu_items_.size() > visible_rows
            ? application_menu_items_.size() - visible_rows : 0;
        if (button == 4 && application_menu_scroll_ > 0) --application_menu_scroll_;
        if (button == 5 && application_menu_scroll_ < max_scroll) ++application_menu_scroll_;
        draw_application_menu(2);
        return;
    }
    if (button != 1) {
        return;
    }
    const int category_width = application_menu_settings_.categories_width;
    const int visible_category_width = application_menu_settings_.show_categories
        ? category_width : 0;
    const int footer_height = application_menu_settings_.show_categories ? 42 : 78;
    const int footer_y = application_menu_height_ - footer_height;
    const int places_width = application_menu_settings_.show_places
        ? application_menu_settings_.places_width : 0;
    const int category_x = places_width;
    const int applications_x = category_x + visible_category_width;
    const int list_bottom = footer_y;
    if (y >= 8 && y < list_bottom) {
        int column = 0;
        int track_y = 8;
        int track_height = list_bottom - 8;
        std::size_t content_count = application_menu_items_.size();
        std::size_t visible_count = static_cast<std::size_t>(std::max(1,
            (list_bottom - 8) / application_menu_row_height_));
        std::size_t scroll = application_menu_scroll_;
        if (application_menu_settings_.show_categories && x >= category_x + category_width - 19 &&
            x < applications_x && y >= 34) {
            column = 1;
            track_y = 34;
            track_height = list_bottom - track_y;
            content_count = application_menu_categories_.size();
            visible_count = static_cast<std::size_t>(std::max(1, track_height / 27));
            scroll = application_menu_category_scroll_;
        } else if (x >= application_menu_width_ - 19) {
            column = 2;
        }
        if (column != 0 && track_height > 0 && content_count > visible_count) {
            const int thumb_height = std::min(track_height, std::max(22,
                static_cast<int>(static_cast<std::size_t>(track_height) * visible_count /
                                 content_count)));
            const std::size_t maximum = content_count - visible_count;
            const int thumb_y = track_y + static_cast<int>(
                static_cast<std::size_t>(track_height - thumb_height) * scroll / maximum);
            application_menu_scrollbar_drag_column_ = column;
            application_menu_scrollbar_drag_offset_ = y >= thumb_y && y < thumb_y + thumb_height
                ? y - thumb_y : thumb_height / 2;
            update_application_menu_scrollbar_drag(y);
            return;
        }
    }
    if (y >= footer_y) {
        if (application_menu_settings_.show_categories &&
            x >= category_x && x < applications_x) {
            application_menu_focus_ = 3;
            open_application_power_menu();
        } else if (x >= applications_x) {
            if (!application_menu_settings_.show_categories && y < footer_y + 34) {
                application_menu_focus_ = 3;
                open_application_power_menu();
            } else {
                application_menu_focus_ = 0;
                draw_application_menu();
            }
        }
        return;
    }
    if (places_width > 0 && x < places_width) {
        const int content_y = y + application_menu_shortcut_scroll_;
        const auto activate_entry = [&](const std::vector<ApplicationMenuDisplayItem>& entries,
                                        int first_y, bool system) {
            const int row = (content_y - first_y) / 27;
            const int row_y = first_y + row * 27;
            if (content_y >= first_y && row >= 0 && content_y < row_y + 25 &&
                static_cast<std::size_t>(row) < entries.size() &&
                application_menu_shortcut_callback_) {
                application_menu_focus_ = 4;
                application_menu_selected_shortcut_ = system
                    ? application_menu_settings_.places.size() + static_cast<std::size_t>(row)
                    : static_cast<std::size_t>(row);
                application_menu_shortcut_callback_(system, static_cast<std::size_t>(row));
                return true;
            }
            return false;
        };
        const int system_first_y = 65 +
            static_cast<int>(application_menu_settings_.places.size()) * 27;
        if (!activate_entry(application_menu_settings_.places, 34, false)) {
            activate_entry(application_menu_settings_.system, system_first_y, true);
        }
        return;
    }
    if (application_menu_settings_.show_categories &&
        x >= category_x && x < applications_x) {
        const int category_index = (y - 34) / 27;
        const int visible_category_rows = std::max(1, (footer_y - 34) / 27);
        if (category_index >= 0 && category_index < visible_category_rows &&
            static_cast<std::size_t>(category_index) < application_menu_categories_.size()) {
            application_menu_focus_ = 1;
            application_menu_selected_category_ = application_menu_category_scroll_ +
                static_cast<std::size_t>(category_index);
            select_application_menu_category(
                application_menu_categories_[application_menu_selected_category_]);
        }
        return;
    }
    if (x < applications_x) return;
    const int row = (y - 8) / application_menu_row_height_;
    const int visible_rows = std::max(1,
        (application_menu_height_ - footer_height - 8) / application_menu_row_height_);
    if (row >= 0 && row < visible_rows) {
        const std::size_t index = application_menu_scroll_ + static_cast<std::size_t>(row);
        if (index < application_menu_items_.size()) {
            application_menu_focus_ = 2;
            application_menu_selected_ = index;
            if (application_menu_activate_callback_) {
                application_menu_activate_callback_(index);
            }
        }
    }
}

void X11Backend::handle_application_menu_motion(int x, int y, int root_x, int root_y)
{
    if (application_power_visible_ && root_x >= application_power_x_ &&
        root_x < application_power_x_ + application_power_width_ &&
        root_y >= application_power_y_ &&
        root_y < application_power_y_ + application_power_height_) {
        const int row = (root_y - application_power_y_ - 4) / 28;
        if (row >= 0 && row < 6 && row != application_power_selected_) {
            application_power_selected_ = row;
            draw_application_power_menu();
        }
        return;
    }
    if (application_menu_scrollbar_drag_column_ != 0) {
        update_application_menu_scrollbar_drag(y);
        return;
    }
    const int category_width = application_menu_settings_.categories_width;
    const int visible_category_width = application_menu_settings_.show_categories
        ? category_width : 0;
    const int footer_height = application_menu_settings_.show_categories ? 42 : 78;
    if (y < 0 || y >= application_menu_height_ - footer_height) {
        return;
    }
    const int places_width = application_menu_settings_.show_places
        ? application_menu_settings_.places_width : 0;
    const int category_x = places_width;
    const int applications_x = category_x + visible_category_width;
    if (places_width > 0 && x < places_width) {
        const int content_y = y + application_menu_shortcut_scroll_;
        const int places_count = static_cast<int>(application_menu_settings_.places.size());
        const int system_first_y = 65 + places_count * 27;
        int selected = -1;
        if (content_y >= 34 && content_y < 34 + places_count * 27) {
            const int row = (content_y - 34) / 27;
            if (content_y < 34 + row * 27 + 25) selected = row;
        } else if (content_y >= system_first_y) {
            const int row = (content_y - system_first_y) / 27;
            if (row >= 0 && static_cast<std::size_t>(row) <
                application_menu_settings_.system.size() &&
                content_y < system_first_y + row * 27 + 25) {
                selected = places_count + row;
            }
        }
        if (selected >= 0 && static_cast<std::size_t>(selected) !=
            application_menu_selected_shortcut_) {
            application_menu_focus_ = 4;
            application_menu_selected_shortcut_ = static_cast<std::size_t>(selected);
            draw_application_menu();
        }
        return;
    }
    if (application_menu_settings_.show_categories &&
        x >= category_x && x < applications_x) {
        const int local_index = (y - 34) / 27;
        const int visible_rows = std::max(1, (application_menu_height_ - footer_height - 34) / 27);
        const int index = static_cast<int>(application_menu_category_scroll_) + local_index;
        if (local_index >= visible_rows) return;
        if (index >= 0 && static_cast<std::size_t>(index) < application_menu_categories_.size() &&
            application_menu_categories_[static_cast<std::size_t>(index)] !=
                application_menu_active_category_) {
            application_menu_focus_ = 1;
            application_menu_selected_category_ = static_cast<std::size_t>(index);
            select_application_menu_category(
                application_menu_categories_[application_menu_selected_category_]);
        }
        return;
    }
    if (x < applications_x) return;
    const int row = (y - 8) / application_menu_row_height_;
    const int visible_rows = std::max(1,
        (application_menu_height_ - footer_height - 8) / application_menu_row_height_);
    if (row >= 0 && row < visible_rows) {
        const std::size_t index = application_menu_scroll_ + static_cast<std::size_t>(row);
        if (index < application_menu_items_.size() && index != application_menu_selected_) {
            application_menu_focus_ = 2;
            application_menu_selected_ = index;
            draw_application_menu();
        }
    }
}

void X11Backend::open_desktop_hub(
    int anchor_x, int anchor_width, std::string icon_name,
    std::vector<HubDisplayItem> items,
    std::function<void(const std::string&)> on_search,
    std::function<void(std::size_t)> on_activate,
    std::function<void()> on_close)
{
    if (desktop_hub_visible_) {
        hide_desktop_hub(true);
        return;
    }
    desktop_hub_icon_ = std::move(icon_name);
    desktop_hub_query_.clear();
    desktop_hub_items_ = std::move(items);
    desktop_hub_selected_ = 0;
    desktop_hub_scroll_ = 0;
    desktop_hub_search_callback_ = std::move(on_search);
    desktop_hub_activate_callback_ = std::move(on_activate);
    desktop_hub_close_callback_ = std::move(on_close);

    if (desktop_hub_window_ == XCB_WINDOW_NONE) {
        const uint32_t event_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
            XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_FOCUS_CHANGE |
            XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_ENTER_WINDOW |
            XCB_EVENT_MASK_LEAVE_WINDOW;
        const uint32_t values[] = {screen_->white_pixel, 1, event_mask};
        desktop_hub_window_ = xcb_generate_id(connection_);
        xcb_create_window(connection_, XCB_COPY_FROM_PARENT, desktop_hub_window_, screen_->root,
            0, 0, desktop_hub_width_, desktop_hub_height_, 0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT, screen_->root_visual,
            XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
        const xcb_atom_t name = intern_atom(connection_, "_NET_WM_NAME");
        const xcb_atom_t utf8 = intern_atom(connection_, "UTF8_STRING");
        constexpr char title[] = "Desktop Hub";
        if (name != XCB_ATOM_NONE && utf8 != XCB_ATOM_NONE) {
            xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, desktop_hub_window_, name,
                utf8, 8, sizeof(title) - 1, title);
        }
        xcb_visualtype_t* visual = find_visual(screen_, screen_->root_visual);
        if (visual == nullptr) {
            destroy_desktop_hub();
            return;
        }
        desktop_hub_surface_ = cairo_xcb_surface_create(connection_, desktop_hub_window_, visual,
            desktop_hub_width_, desktop_hub_height_);
        if (cairo_surface_status(desktop_hub_surface_) != CAIRO_STATUS_SUCCESS) {
            destroy_desktop_hub();
            return;
        }
        desktop_hub_cairo_ = cairo_create(desktop_hub_surface_);
        if (cairo_status(desktop_hub_cairo_) != CAIRO_STATUS_SUCCESS) {
            destroy_desktop_hub();
            return;
        }
    }

    position_popup(anchor_x, anchor_width, desktop_hub_width_, desktop_hub_height_,
                   desktop_hub_x_, desktop_hub_y_);
    draw_desktop_hub();
    const uint32_t coordinates[] = {
        static_cast<uint32_t>(desktop_hub_x_), static_cast<uint32_t>(desktop_hub_y_)
    };
    xcb_configure_window(connection_, desktop_hub_window_,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, coordinates);
    desktop_hub_visible_ = true;
    xcb_map_window(connection_, desktop_hub_window_);
    const uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(connection_, desktop_hub_window_, XCB_CONFIG_WINDOW_STACK_MODE,
                         &stack_mode);
    xcb_set_input_focus(connection_, XCB_INPUT_FOCUS_PARENT,
                        desktop_hub_window_, XCB_CURRENT_TIME);
    const auto grab_cookie = xcb_grab_pointer(connection_, 1, desktop_hub_window_,
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_POINTER_MOTION |
            XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW,
        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_WINDOW_NONE, XCB_CURSOR_NONE,
        XCB_CURRENT_TIME);
    xcb_grab_pointer_reply_t* grab_reply = xcb_grab_pointer_reply(connection_, grab_cookie, nullptr);
    desktop_hub_pointer_grabbed_ = grab_reply != nullptr &&
        grab_reply->status == XCB_GRAB_STATUS_SUCCESS;
    free(grab_reply);
    xcb_flush(connection_);
}

void X11Backend::update_desktop_hub(std::string query,
                                    std::vector<HubDisplayItem> items,
                                    std::size_t selected,
                                    bool preserve_selection)
{
    const std::size_t previous_selected = desktop_hub_selected_;
    desktop_hub_query_ = std::move(query);
    desktop_hub_items_ = std::move(items);
    desktop_hub_selected_ = desktop_hub_items_.empty()
        ? 0 : std::min(preserve_selection ? previous_selected : selected,
                       desktop_hub_items_.size() - 1);
    update_desktop_hub_scroll();
    draw_desktop_hub();
}

void X11Backend::close_desktop_hub()
{
    hide_desktop_hub(true);
}

bool X11Backend::desktop_hub_visible() const
{
    return desktop_hub_visible_;
}

void X11Backend::set_desktop_hub_countdown_active(bool active)
{
    const bool was_active = desktop_hub_countdown_active_;
    desktop_hub_countdown_active_ = active;
    if (was_active && !active && desktop_hub_visible_) {
        schedule_popup_dismiss();
    }
}

void X11Backend::draw_desktop_hub()
{
    if (desktop_hub_cairo_ == nullptr) {
        return;
    }
    cairo_t* cr = desktop_hub_cairo_;
    cairo_set_source_rgb(cr, 245.0 / 255.0, 245.0 / 255.0, 245.0 / 255.0);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 119.0 / 255.0, 119.0 / 255.0, 119.0 / 255.0);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 0.5, 0.5, desktop_hub_width_ - 1.0, desktop_hub_height_ - 1.0);
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_rectangle(cr, 29.5, 29.5, desktop_hub_width_ - 59.0, 34.0);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 181.0 / 255.0, 181.0 / 255.0, 181.0 / 255.0);
    cairo_stroke(cr);
    paint_theme_icon(cr, "system-search-symbolic", 47.0, 46.0, 18);
    const std::string search_text = desktop_hub_query_.empty()
        ? "Type app, folder, file, *.deb, or command..." : desktop_hub_query_;
    PangoLayout* search_layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(search_layout, search_text.c_str(), -1);
    PangoFontDescription* search_font = pango_font_description_from_string("Sans 10");
    pango_layout_set_font_description(search_layout, search_font);
    pango_font_description_free(search_font);
    int search_height = 0;
    pango_layout_get_pixel_size(search_layout, nullptr, &search_height);
    cairo_set_source_rgb(cr, desktop_hub_query_.empty() ? 0.45 : 32.0 / 255.0,
                         desktop_hub_query_.empty() ? 0.45 : 32.0 / 255.0,
                         desktop_hub_query_.empty() ? 0.45 : 32.0 / 255.0);
    cairo_move_to(cr, 62.0, 46.0 - search_height / 2.0);
    pango_cairo_show_layout(cr, search_layout);
    if (!desktop_hub_query_.empty()) {
        int text_width = 0;
        pango_layout_get_pixel_size(search_layout, &text_width, nullptr);
        cairo_move_to(cr, 63.0 + text_width, 37.0);
        cairo_line_to(cr, 63.0 + text_width, 56.0);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
    }
    g_object_unref(search_layout);

    constexpr int first_row_y = 73;
    constexpr int visible_rows = 11;
    const int scrollbar_width = desktop_hub_items_.size() > visible_rows ? 14 : 0;
    for (std::size_t row = 0; row < visible_rows; ++row) {
        const std::size_t index = desktop_hub_scroll_ + row;
        if (index >= desktop_hub_items_.size()) {
            break;
        }
        const int y = first_row_y + static_cast<int>(row) * desktop_hub_row_height_;
        if (index == desktop_hub_selected_) {
            cairo_set_source_rgb(cr, 220.0 / 255.0, 233.0 / 255.0, 203.0 / 255.0);
            cairo_rectangle(cr, 29.0, y, desktop_hub_width_ - 58.0 - scrollbar_width,
                            desktop_hub_row_height_);
            cairo_fill(cr);
        }
        PangoLayout* row_layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(row_layout, desktop_hub_items_[index].label.c_str(), -1);
        pango_layout_set_ellipsize(row_layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_width(row_layout,
            (desktop_hub_width_ - 66 - scrollbar_width) * PANGO_SCALE);
        PangoFontDescription* row_font = pango_font_description_from_string("Sans 10");
        pango_layout_set_font_description(row_layout, row_font);
        pango_font_description_free(row_font);
        int row_height = 0;
        pango_layout_get_pixel_size(row_layout, nullptr, &row_height);
        if (index == desktop_hub_selected_) {
            cairo_set_source_rgb(cr, 23.0 / 255.0, 23.0 / 255.0, 23.0 / 255.0);
        } else {
            cairo_set_source_rgb(cr, 48.0 / 255.0, 48.0 / 255.0, 48.0 / 255.0);
        }
        cairo_move_to(cr, 37.0, y + (desktop_hub_row_height_ - row_height) / 2.0);
        pango_cairo_show_layout(cr, row_layout);
        g_object_unref(row_layout);
    }
    if (scrollbar_width > 0) {
        const double track_y = first_row_y;
        const double track_height = visible_rows * desktop_hub_row_height_;
        const double thumb_height = std::max(24.0, track_height * visible_rows /
            static_cast<double>(desktop_hub_items_.size()));
        const std::size_t max_scroll = desktop_hub_items_.size() - visible_rows;
        const double scroll_ratio = max_scroll == 0 ? 0.0 :
            static_cast<double>(desktop_hub_scroll_) / max_scroll;
        const double thumb_y = track_y + scroll_ratio * (track_height - thumb_height);
        cairo_set_source_rgb(cr, 238.0 / 255.0, 238.0 / 255.0, 238.0 / 255.0);
        cairo_rectangle(cr, desktop_hub_width_ - 43.0, track_y, scrollbar_width, track_height);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 181.0 / 255.0, 181.0 / 255.0, 181.0 / 255.0);
        cairo_rectangle(cr, desktop_hub_width_ - 43.0, thumb_y, scrollbar_width, thumb_height);
        cairo_fill(cr);
    }
    cairo_surface_flush(desktop_hub_surface_);
    xcb_flush(connection_);
}

void X11Backend::hide_desktop_hub(bool notify)
{
    if (!desktop_hub_visible_) {
        return;
    }
    desktop_hub_visible_ = false;
    if (desktop_hub_pointer_grabbed_ && connection_ != nullptr) {
        xcb_ungrab_pointer(connection_, XCB_CURRENT_TIME);
        desktop_hub_pointer_grabbed_ = false;
    }
    if (desktop_hub_window_ != XCB_WINDOW_NONE && connection_ != nullptr) {
        xcb_unmap_window(connection_, desktop_hub_window_);
        xcb_flush(connection_);
    }
    if (notify && desktop_hub_close_callback_) {
        desktop_hub_close_callback_();
    }
}

void X11Backend::destroy_desktop_hub()
{
    hide_desktop_hub(false);
    if (desktop_hub_cairo_ != nullptr) {
        cairo_destroy(desktop_hub_cairo_);
        desktop_hub_cairo_ = nullptr;
    }
    if (desktop_hub_surface_ != nullptr) {
        cairo_surface_destroy(desktop_hub_surface_);
        desktop_hub_surface_ = nullptr;
    }
    if (connection_ != nullptr && desktop_hub_window_ != XCB_WINDOW_NONE) {
        xcb_destroy_window(connection_, desktop_hub_window_);
        xcb_flush(connection_);
    }
    desktop_hub_window_ = XCB_WINDOW_NONE;
}

void X11Backend::update_desktop_hub_scroll()
{
    constexpr std::size_t visible_rows = 11;
    if (desktop_hub_selected_ < desktop_hub_scroll_) {
        desktop_hub_scroll_ = desktop_hub_selected_;
    } else if (desktop_hub_selected_ >= desktop_hub_scroll_ + visible_rows) {
        desktop_hub_scroll_ = desktop_hub_selected_ - visible_rows + 1;
    }
}

void X11Backend::handle_desktop_hub_key(const xcb_key_press_event_t* event)
{
    const xcb_setup_t* setup = xcb_get_setup(connection_);
    const auto mapping_cookie = xcb_get_keyboard_mapping(
        connection_, event->detail, 1);
    xcb_get_keyboard_mapping_reply_t* mapping = xcb_get_keyboard_mapping_reply(
        connection_, mapping_cookie, nullptr);
    if (mapping == nullptr) {
        return;
    }
    const uint32_t* symbols = xcb_get_keyboard_mapping_keysyms(mapping);
    int symbol_index = (event->state & XCB_MOD_MASK_SHIFT) != 0 &&
        mapping->keysyms_per_keycode > 1 ? 1 : 0;
    if ((event->state & XCB_MOD_MASK_LOCK) != 0 && mapping->keysyms_per_keycode > 1) {
        const uint32_t base = symbols[0];
        if ((base >= 'a' && base <= 'z') || (base >= 'A' && base <= 'Z')) {
            symbol_index = symbol_index == 0 ? 1 : 0;
        }
    }
    uint32_t symbol = symbols[symbol_index];
    free(mapping);

    constexpr uint32_t key_backspace = 0xff08;
    constexpr uint32_t key_return = 0xff0d;
    constexpr uint32_t key_escape = 0xff1b;
    constexpr uint32_t key_up = 0xff52;
    constexpr uint32_t key_down = 0xff54;
    constexpr uint32_t key_kp_enter = 0xff8d;
    if (symbol == key_escape) {
        hide_desktop_hub(true);
        return;
    }
    if (symbol == key_up || symbol == key_down) {
        if (!desktop_hub_items_.empty()) {
            if (symbol == key_up && desktop_hub_selected_ > 0) {
                --desktop_hub_selected_;
            } else if (symbol == key_down &&
                       desktop_hub_selected_ + 1 < desktop_hub_items_.size()) {
                ++desktop_hub_selected_;
            }
            update_desktop_hub_scroll();
            draw_desktop_hub();
        }
        return;
    }
    if (symbol == key_return || symbol == key_kp_enter) {
        if (desktop_hub_selected_ < desktop_hub_items_.size() &&
            desktop_hub_activate_callback_) {
            desktop_hub_activate_callback_(desktop_hub_selected_);
        }
        return;
    }
    if (symbol == key_backspace) {
        if (!desktop_hub_query_.empty()) {
            std::size_t start = desktop_hub_query_.size() - 1;
            while (start > 0 &&
                   (static_cast<unsigned char>(desktop_hub_query_[start]) & 0xc0) == 0x80) {
                --start;
            }
            desktop_hub_query_.erase(start);
            if (desktop_hub_search_callback_) {
                desktop_hub_search_callback_(desktop_hub_query_);
            }
        }
        return;
    }

    const uint16_t blocked = XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_4;
    if ((event->state & blocked) != 0) {
        return;
    }
    uint32_t codepoint = 0;
    if (symbol <= 0xff) {
        codepoint = symbol;
    } else if ((symbol & 0xff000000U) == 0x01000000U) {
        codepoint = symbol & 0x00ffffffU;
    }
    if (codepoint == 0 || !g_unichar_isprint(codepoint) ||
        (setup == nullptr)) {
        return;
    }
    gchar encoded[7]{};
    const int length = g_unichar_to_utf8(codepoint, encoded);
    desktop_hub_query_.append(encoded, static_cast<std::size_t>(length));
    if (desktop_hub_search_callback_) {
        desktop_hub_search_callback_(desktop_hub_query_);
    }
}

void X11Backend::handle_desktop_hub_click(int x, int y, uint8_t button)
{
    constexpr std::size_t visible_rows = 11;
    if ((button == 4 || button == 5) && !desktop_hub_items_.empty()) {
        const std::size_t max_scroll = desktop_hub_items_.size() > visible_rows
            ? desktop_hub_items_.size() - visible_rows : 0;
        if (button == 4 && desktop_hub_scroll_ > 0) {
            --desktop_hub_scroll_;
        } else if (button == 5 && desktop_hub_scroll_ < max_scroll) {
            ++desktop_hub_scroll_;
        }
        draw_desktop_hub();
        return;
    }
    if (button != 1) {
        return;
    }
    if (x < 0 || x >= desktop_hub_width_ || y < 0 || y >= desktop_hub_height_) {
        hide_desktop_hub(true);
        return;
    }
    constexpr int first_row_y = 73;
    if (y < first_row_y) {
        return;
    }
    if (desktop_hub_items_.size() > visible_rows && x >= desktop_hub_width_ - 43) {
        const std::size_t max_scroll = desktop_hub_items_.size() - visible_rows;
        const int track_height = visible_rows * desktop_hub_row_height_;
        const int relative_y = std::clamp(y - first_row_y, 0, track_height);
        desktop_hub_scroll_ = max_scroll * static_cast<std::size_t>(relative_y) / track_height;
        draw_desktop_hub();
        return;
    }
    const std::size_t index = desktop_hub_scroll_ +
        static_cast<std::size_t>((y - first_row_y) / desktop_hub_row_height_);
    if (index < desktop_hub_items_.size() && desktop_hub_activate_callback_) {
        desktop_hub_selected_ = index;
        desktop_hub_activate_callback_(index);
    }
}

std::vector<TaskWindow> X11Backend::get_task_windows()
{
    std::vector<TaskWindow> tasks;
    if (connection_ == nullptr || screen_ == nullptr) {
        return tasks;
    }

    const xcb_atom_t client_list_atom = intern_atom(connection_, "_NET_CLIENT_LIST_STACKING");
    const xcb_atom_t current_desktop_atom = intern_atom(connection_, "_NET_CURRENT_DESKTOP");
    const xcb_atom_t active_atom = intern_atom(connection_, "_NET_ACTIVE_WINDOW");
    const auto clients_cookie = xcb_get_property(connection_, 0, screen_->root, client_list_atom,
                                                  XCB_ATOM_WINDOW, 0, 4096);
    xcb_get_property_reply_t* clients = xcb_get_property_reply(connection_, clients_cookie, nullptr);
    if (clients == nullptr || clients->format != 32) {
        free(clients);
        return tasks;
    }

    uint32_t current_desktop = 0;
    const auto desktop_cookie = xcb_get_property(connection_, 0, screen_->root,
                                                  current_desktop_atom, XCB_ATOM_CARDINAL,
                                                  0, 1);
    xcb_get_property_reply_t* desktop_reply = xcb_get_property_reply(
        connection_, desktop_cookie, nullptr);
    if (desktop_reply != nullptr && xcb_get_property_value_length(desktop_reply) >= 4) {
        current_desktop = *static_cast<uint32_t*>(xcb_get_property_value(desktop_reply));
    }
    free(desktop_reply);

    xcb_window_t active_window = XCB_WINDOW_NONE;
    const auto active_cookie = xcb_get_property(connection_, 0, screen_->root, active_atom,
                                                 XCB_ATOM_WINDOW, 0, 1);
    xcb_get_property_reply_t* active_reply = xcb_get_property_reply(
        connection_, active_cookie, nullptr);
    if (active_reply != nullptr && xcb_get_property_value_length(active_reply) >= 4) {
        active_window = *static_cast<xcb_window_t*>(xcb_get_property_value(active_reply));
    }
    free(active_reply);

    const xcb_window_t* windows = static_cast<const xcb_window_t*>(
        xcb_get_property_value(clients));
    const int window_count = xcb_get_property_value_length(clients) / 4;
    const xcb_atom_t net_wm_name = intern_atom(connection_, "_NET_WM_NAME");
    const xcb_atom_t utf8_string = intern_atom(connection_, "UTF8_STRING");
    const xcb_atom_t net_wm_icon = intern_atom(connection_, "_NET_WM_ICON");
    const xcb_atom_t wm_name = intern_atom(connection_, "WM_NAME");
    const xcb_atom_t wm_class = intern_atom(connection_, "WM_CLASS");
    const xcb_atom_t wm_state = intern_atom(connection_, "WM_STATE");
    const xcb_atom_t net_wm_state = intern_atom(connection_, "_NET_WM_STATE");
    const xcb_atom_t net_wm_state_skip_taskbar = intern_atom(
        connection_, "_NET_WM_STATE_SKIP_TASKBAR");
    const xcb_atom_t net_wm_state_max_horz = intern_atom(
        connection_, "_NET_WM_STATE_MAXIMIZED_HORZ");
    const xcb_atom_t net_wm_state_max_vert = intern_atom(
        connection_, "_NET_WM_STATE_MAXIMIZED_VERT");
    const xcb_atom_t net_wm_desktop = intern_atom(connection_, "_NET_WM_DESKTOP");
    const xcb_atom_t net_wm_type = intern_atom(connection_, "_NET_WM_WINDOW_TYPE");
    const std::array<const char*, 8> excluded_types = {
        "_NET_WM_WINDOW_TYPE_DESKTOP", "_NET_WM_WINDOW_TYPE_DOCK",
        "_NET_WM_WINDOW_TYPE_TOOLBAR", "_NET_WM_WINDOW_TYPE_MENU",
        "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", "_NET_WM_WINDOW_TYPE_POPUP_MENU",
        "_NET_WM_WINDOW_TYPE_TOOLTIP", "_NET_WM_WINDOW_TYPE_NOTIFICATION"
    };
    std::array<xcb_atom_t, excluded_types.size()> excluded_atoms{};
    for (std::size_t index = 0; index < excluded_types.size(); ++index) {
        excluded_atoms[index] = intern_atom(connection_, excluded_types[index]);
    }

    for (int index = 0; index < window_count; ++index) {
        const xcb_window_t window = windows[index];
        if (window == window_) {
            continue;
        }

        const auto type_cookie = xcb_get_property(connection_, 0, window, net_wm_type,
                                                    XCB_ATOM_ATOM, 0, 32);
        xcb_get_property_reply_t* type_reply = xcb_get_property_reply(
            connection_, type_cookie, nullptr);
        bool excluded = false;
        if (type_reply != nullptr && type_reply->format == 32) {
            const auto* types = static_cast<const xcb_atom_t*>(
                xcb_get_property_value(type_reply));
            const int type_count = xcb_get_property_value_length(type_reply) / 4;
            for (int type_index = 0; type_index < type_count && !excluded; ++type_index) {
                excluded = std::find(excluded_atoms.begin(), excluded_atoms.end(),
                                     types[type_index]) != excluded_atoms.end();
            }
        }
        free(type_reply);
        if (excluded) {
            continue;
        }

        const auto state_cookie = xcb_get_property(connection_, 0, window, net_wm_state,
                                                    XCB_ATOM_ATOM, 0, 64);
        xcb_get_property_reply_t* state_reply = xcb_get_property_reply(
            connection_, state_cookie, nullptr);
        bool skip_taskbar = false;
        bool maximized_horz = false;
        bool maximized_vert = false;
        if (state_reply != nullptr && state_reply->format == 32) {
            const auto* states = static_cast<const xcb_atom_t*>(
                xcb_get_property_value(state_reply));
            const int state_count = xcb_get_property_value_length(state_reply) / 4;
            for (int state_index = 0; state_index < state_count; ++state_index) {
                skip_taskbar = skip_taskbar || states[state_index] == net_wm_state_skip_taskbar;
                maximized_horz = maximized_horz || states[state_index] == net_wm_state_max_horz;
                maximized_vert = maximized_vert || states[state_index] == net_wm_state_max_vert;
            }
        }
        free(state_reply);
        if (skip_taskbar) {
            continue;
        }

        const auto window_desktop_cookie = xcb_get_property(
            connection_, 0, window, net_wm_desktop, XCB_ATOM_CARDINAL, 0, 1);
        xcb_get_property_reply_t* window_desktop_reply = xcb_get_property_reply(
            connection_, window_desktop_cookie, nullptr);
        uint32_t window_desktop = 0xffffffffU;
        if (window_desktop_reply != nullptr &&
            xcb_get_property_value_length(window_desktop_reply) >= 4) {
            window_desktop = *static_cast<uint32_t*>(
                xcb_get_property_value(window_desktop_reply));
        }
        free(window_desktop_reply);
        if (window_desktop != 0xffffffffU && window_desktop != current_desktop) {
            continue;
        }

        if (task_monitor_filter_ && monitor_ != "all") {
            const auto geometry_cookie = xcb_get_geometry(connection_, window);
            xcb_get_geometry_reply_t* geometry = xcb_get_geometry_reply(
                connection_, geometry_cookie, nullptr);
            const auto position_cookie = xcb_translate_coordinates(
                connection_, window, screen_->root, 0, 0);
            xcb_translate_coordinates_reply_t* position = xcb_translate_coordinates_reply(
                connection_, position_cookie, nullptr);
            if (geometry != nullptr && position != nullptr) {
                const int left = position->dst_x;
                const int top = position->dst_y;
                const int right = left + geometry->width;
                const int bottom = top + geometry->height;
                const bool intersects = right > monitor_x_ && bottom > monitor_y_ &&
                    left < monitor_x_ + monitor_width_ && top < monitor_y_ + monitor_height_;
                free(geometry);
                free(position);
                if (!intersects) {
                    continue;
                }
            } else {
                free(geometry);
                free(position);
            }
        }

        TaskWindow task;
        task.id = window;
        task.active = window == active_window;
        task.maximized = maximized_horz && maximized_vert;

        const auto icon_cookie = xcb_get_property(connection_, 0, window, net_wm_icon,
                                                   XCB_ATOM_CARDINAL, 0, 1U << 18);
        xcb_get_property_reply_t* icon_reply = xcb_get_property_reply(
            connection_, icon_cookie, nullptr);
        if (icon_reply != nullptr && icon_reply->format == 32) {
            const auto* values = static_cast<const uint32_t*>(
                xcb_get_property_value(icon_reply));
            const std::size_t value_count = static_cast<std::size_t>(
                xcb_get_property_value_length(icon_reply) / 4);
            std::size_t best_offset = 0;
            uint32_t best_width = 0;
            uint32_t best_height = 0;
            uint64_t best_score = UINT64_MAX;
            for (std::size_t offset = 0; offset + 2 <= value_count;) {
                const uint32_t icon_width = values[offset];
                const uint32_t icon_height = values[offset + 1];
                if (icon_width == 0 || icon_height == 0 ||
                    icon_width > 512 || icon_height > 512) {
                    break;
                }
                const uint64_t pixel_count = static_cast<uint64_t>(icon_width) * icon_height;
                if (pixel_count > value_count - offset - 2) {
                    break;
                }
                const uint64_t score = static_cast<uint64_t>(std::abs(
                    static_cast<int>(icon_width) - 24)) + static_cast<uint64_t>(std::abs(
                    static_cast<int>(icon_height) - 24));
                if (score < best_score) {
                    best_offset = offset + 2;
                    best_width = icon_width;
                    best_height = icon_height;
                    best_score = score;
                }
                offset += static_cast<std::size_t>(pixel_count) + 2;
            }
            if (best_width > 0) {
                const std::size_t pixel_count = static_cast<std::size_t>(best_width) * best_height;
                task.icon_argb.assign(values + best_offset, values + best_offset + pixel_count);
                task.icon_width = best_width;
                task.icon_height = best_height;
                uint64_t hash = 1469598103934665603ULL;
                for (const uint32_t pixel : task.icon_argb) {
                    hash = (hash ^ pixel) * 1099511628211ULL;
                }
                task.icon_hash = hash;
            }
        }
        free(icon_reply);

        const auto title_cookie = xcb_get_property(connection_, 0, window, net_wm_name,
                                                    utf8_string, 0, 1024);
        xcb_get_property_reply_t* title_reply = xcb_get_property_reply(
            connection_, title_cookie, nullptr);
        if (title_reply != nullptr && xcb_get_property_value_length(title_reply) > 0) {
            task.title.assign(static_cast<const char*>(
                xcb_get_property_value(title_reply)),
                xcb_get_property_value_length(title_reply));
        }
        free(title_reply);
        if (task.title.empty()) {
            const auto fallback_cookie = xcb_get_property(connection_, 0, window, wm_name,
                                                           XCB_GET_PROPERTY_TYPE_ANY, 0, 1024);
            xcb_get_property_reply_t* fallback_reply = xcb_get_property_reply(
                connection_, fallback_cookie, nullptr);
            if (fallback_reply != nullptr && xcb_get_property_value_length(fallback_reply) > 0) {
                task.title.assign(static_cast<const char*>(
                    xcb_get_property_value(fallback_reply)),
                    xcb_get_property_value_length(fallback_reply));
            }
            free(fallback_reply);
        }

        const auto class_cookie = xcb_get_property(connection_, 0, window, wm_class,
                                                    XCB_GET_PROPERTY_TYPE_ANY, 0, 256);
        xcb_get_property_reply_t* class_reply = xcb_get_property_reply(
            connection_, class_cookie, nullptr);
        if (class_reply != nullptr && xcb_get_property_value_length(class_reply) > 0) {
            const auto* names = static_cast<const char*>(xcb_get_property_value(class_reply));
            const int length = xcb_get_property_value_length(class_reply);
            const auto* instance_end = std::find(names, names + length, '\0');
            const int instance_length = static_cast<int>(instance_end - names);
            const int class_start = std::min(length, instance_length + 1);
            std::string instance_name(names, instance_length);
            std::string class_name;
            if (class_start < length) {
                const auto* class_end = std::find(names + class_start, names + length, '\0');
                class_name.assign(names + class_start, class_end - (names + class_start));
            }
            task.icon_name = resolve_application_icon(instance_name);
            if (task.icon_name.empty()) {
                task.icon_name = resolve_application_icon(class_name);
            }
            if (task.icon_name.empty()) {
                task.icon_name = class_name;
                std::transform(task.icon_name.begin(), task.icon_name.end(), task.icon_name.begin(),
                               [](unsigned char character) {
                                   return static_cast<char>(std::tolower(character));
                               });
            }
        }
        free(class_reply);
        if (task.title.empty()) {
            task.title = task.icon_name.empty() ? "Window" : task.icon_name;
        }

        const auto wm_state_cookie = xcb_get_property(connection_, 0, window, wm_state,
                                                       XCB_GET_PROPERTY_TYPE_ANY, 0, 2);
        xcb_get_property_reply_t* wm_state_reply = xcb_get_property_reply(
            connection_, wm_state_cookie, nullptr);
        if (wm_state_reply != nullptr && xcb_get_property_value_length(wm_state_reply) >= 4) {
            task.iconified = *static_cast<uint32_t*>(
                xcb_get_property_value(wm_state_reply)) == 3;
        }
        free(wm_state_reply);
        if (task_is_excluded(task.icon_name, task.title)) {
            continue;
        }
        tasks.push_back(std::move(task));
    }
    free(clients);
    return tasks;
}

bool X11Backend::ensure_gnome_bridge() const
{
    if (!gnome_wayland_session_) {
        return false;
    }
    if (gnome_bridge_ != nullptr) {
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < gnome_bridge_retry_after_) {
        return false;
    }
    gnome_bridge_retry_after_ = now + std::chrono::seconds(1);
    gnome_bridge_ = WaylandTaskBackend::create_gnome_bridge();
    return gnome_bridge_ != nullptr;
}

bool X11Backend::supports_task_list() const
{
    ensure_gnome_bridge();
    return gnome_bridge_ != nullptr ? gnome_bridge_->supports_context_menu()
                                    : connection_ != nullptr;
}

bool X11Backend::supports_task_context_menu() const
{
    ensure_gnome_bridge();
    return gnome_bridge_ != nullptr ? gnome_bridge_->supports_context_menu()
                                    : connection_ != nullptr;
}

bool X11Backend::supports_task_action(TaskAction action) const
{
    ensure_gnome_bridge();
    return gnome_bridge_ != nullptr ? gnome_bridge_->supports_action(action)
                                    : connection_ != nullptr;
}

std::vector<TaskWindowInfo> X11Backend::list_task_windows()
{
    ensure_gnome_bridge();
    if (gnome_bridge_ != nullptr) {
        return gnome_bridge_->list_windows();
    }
    std::vector<TaskWindowInfo> result;
    for (const TaskWindow& window : get_task_windows()) {
        TaskWindowInfo task;
        task.id = std::to_string(window.id);
        task.title = window.title;
        task.icon_name = window.icon_name;
        task.icon_argb = window.icon_argb;
        task.icon_width = window.icon_width;
        task.icon_height = window.icon_height;
        task.icon_hash = window.icon_hash;
        task.active = window.active;
        task.iconified = window.iconified;
        task.maximized = window.maximized;
        result.push_back(std::move(task));
    }
    return result;
}

void X11Backend::perform_task_action(const std::string& id, TaskAction action)
{
    ensure_gnome_bridge();
    if (gnome_bridge_ != nullptr) {
        gnome_bridge_->perform_action(id, action);
        return;
    }
    try {
        const unsigned long value = std::stoul(id);
        if (value <= std::numeric_limits<xcb_window_t>::max()) {
            perform_task_action(static_cast<xcb_window_t>(value), action);
        }
    } catch (const std::exception&) {
    }
}

void X11Backend::show_task_menu(const std::string& id, int panel_x, int panel_y,
                                bool maximized)
{
    try {
        const unsigned long value = std::stoul(id);
        if (value <= std::numeric_limits<xcb_window_t>::max()) {
            show_task_menu(static_cast<xcb_window_t>(value), panel_x, panel_y, maximized);
        }
    } catch (const std::exception&) {
    }
}

void X11Backend::perform_task_action(xcb_window_t window, TaskAction action)
{
    if (connection_ == nullptr || window == XCB_WINDOW_NONE) {
        return;
    }
    const auto send_message = [this, window](const char* name, const uint32_t* data) {
        xcb_client_message_event_t message{};
        message.response_type = XCB_CLIENT_MESSAGE;
        message.format = 32;
        message.window = window;
        message.type = intern_atom(connection_, name);
        std::copy_n(data, 5, message.data.data32);
        xcb_send_event(connection_, 0, screen_->root,
                       XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char*>(&message));
    };

    if (action == TaskAction::Activate || action == TaskAction::ToggleMinimize) {
        if (action == TaskAction::ToggleMinimize) {
            const auto active_cookie = xcb_get_property(connection_, 0, screen_->root,
                intern_atom(connection_, "_NET_ACTIVE_WINDOW"), XCB_ATOM_WINDOW, 0, 1);
            xcb_get_property_reply_t* active_reply = xcb_get_property_reply(
                connection_, active_cookie, nullptr);
            const bool active = active_reply != nullptr &&
                xcb_get_property_value_length(active_reply) >= 4 &&
                *static_cast<xcb_window_t*>(xcb_get_property_value(active_reply)) == window;
            free(active_reply);
            if (active) {
                const uint32_t data[] = {3, 0, 0, 0, 0};
                send_message("WM_CHANGE_STATE", data);
                xcb_flush(connection_);
                return;
            }
        }
        const uint32_t data[] = {1, XCB_CURRENT_TIME, 0, 0, 0};
        send_message("_NET_ACTIVE_WINDOW", data);
    } else if (action == TaskAction::Minimize) {
        const uint32_t data[] = {3, 0, 0, 0, 0};
        send_message("WM_CHANGE_STATE", data);
    } else if (action == TaskAction::ToggleMaximize) {
        bool maximized = false;
        const auto state_cookie = xcb_get_property(connection_, 0, window,
            intern_atom(connection_, "_NET_WM_STATE"), XCB_ATOM_ATOM, 0, 64);
        xcb_get_property_reply_t* state_reply = xcb_get_property_reply(
            connection_, state_cookie, nullptr);
        if (state_reply != nullptr && state_reply->format == 32) {
            const auto* states = static_cast<const xcb_atom_t*>(
                xcb_get_property_value(state_reply));
            const int count = xcb_get_property_value_length(state_reply) / 4;
            bool horizontal = false;
            bool vertical = false;
            const xcb_atom_t max_horizontal = intern_atom(
                connection_, "_NET_WM_STATE_MAXIMIZED_HORZ");
            const xcb_atom_t max_vertical = intern_atom(
                connection_, "_NET_WM_STATE_MAXIMIZED_VERT");
            for (int index = 0; index < count; ++index) {
                horizontal = horizontal || states[index] == max_horizontal;
                vertical = vertical || states[index] == max_vertical;
            }
            maximized = horizontal && vertical;
        }
        free(state_reply);
        const uint32_t data[] = {
            maximized ? 0U : 1U,
            intern_atom(connection_, "_NET_WM_STATE_MAXIMIZED_HORZ"),
            intern_atom(connection_, "_NET_WM_STATE_MAXIMIZED_VERT"), 1, 0
        };
        send_message("_NET_WM_STATE", data);
    } else if (action == TaskAction::Close) {
        const uint32_t data[] = {XCB_CURRENT_TIME, 1, 0, 0, 0};
        send_message("_NET_CLOSE_WINDOW", data);
    }
    xcb_flush(connection_);
}

void X11Backend::show_task_menu(xcb_window_t window, int panel_x, int panel_y, bool maximized)
{
    hide_task_menu();
    if (connection_ == nullptr || window == XCB_WINDOW_NONE) {
        return;
    }
    task_menu_target_ = std::to_string(window);
    task_menu_maximized_ = maximized;
    task_menu_hovered_ = -1;
    if (task_menu_window_ == XCB_WINDOW_NONE) {
        const uint32_t event_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
                        XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_FOCUS_CHANGE |
                        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_ENTER_WINDOW |
                        XCB_EVENT_MASK_LEAVE_WINDOW;
        const uint32_t values[] = {screen_->white_pixel, 1, event_mask};
        task_menu_window_ = xcb_generate_id(connection_);
        xcb_create_window(connection_, XCB_COPY_FROM_PARENT, task_menu_window_, screen_->root,
                          0, 0, task_menu_width_, task_menu_row_height_ * 4, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen_->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK,
                          values);
        const xcb_atom_t name = intern_atom(connection_, "_NET_WM_NAME");
        const xcb_atom_t utf8 = intern_atom(connection_, "UTF8_STRING");
        constexpr char title[] = "Window actions";
        if (name != XCB_ATOM_NONE && utf8 != XCB_ATOM_NONE) {
            xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, task_menu_window_, name,
                                utf8, 8, sizeof(title) - 1, title);
        }
        xcb_visualtype_t* visual = find_visual(screen_, screen_->root_visual);
        if (visual == nullptr) {
            destroy_task_menu();
            return;
        }
        task_menu_surface_ = cairo_xcb_surface_create(connection_, task_menu_window_, visual,
                                                       task_menu_width_, task_menu_row_height_ * 4);
        if (cairo_surface_status(task_menu_surface_) != CAIRO_STATUS_SUCCESS) {
            destroy_task_menu();
            return;
        }
        task_menu_cairo_ = cairo_create(task_menu_surface_);
        if (cairo_status(task_menu_cairo_) != CAIRO_STATUS_SUCCESS) {
            destroy_task_menu();
            return;
        }
    }

    const int menu_height = task_menu_row_height_ * 4;
    position_popup(edge_ == Edge::Left || edge_ == Edge::Right ? panel_y : panel_x, 1,
                   task_menu_width_, menu_height, task_menu_x_, task_menu_y_);
    draw_task_menu();
    const uint32_t coordinates[] = {
        static_cast<uint32_t>(task_menu_x_), static_cast<uint32_t>(task_menu_y_)
    };
    xcb_configure_window(connection_, task_menu_window_,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, coordinates);
    xcb_map_window(connection_, task_menu_window_);
    const uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(connection_, task_menu_window_, XCB_CONFIG_WINDOW_STACK_MODE,
                         &stack_mode);
    const auto grab_cookie = xcb_grab_pointer(connection_, 1, task_menu_window_,
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_POINTER_MOTION |
            XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW,
        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_WINDOW_NONE, XCB_CURSOR_NONE,
        XCB_CURRENT_TIME);
    xcb_grab_pointer_reply_t* grab_reply = xcb_grab_pointer_reply(
        connection_, grab_cookie, nullptr);
    task_menu_pointer_grabbed_ = grab_reply != nullptr &&
        grab_reply->status == XCB_GRAB_STATUS_SUCCESS;
    free(grab_reply);
    xcb_set_input_focus(connection_, XCB_INPUT_FOCUS_PARENT,
                        task_menu_window_, XCB_CURRENT_TIME);
    xcb_flush(connection_);
}

void X11Backend::draw_task_menu()
{
    if (task_menu_cairo_ == nullptr) {
        return;
    }
    static const std::array<const char*, 4> labels = {
        "Activate / Restore", "Minimize", "Maximize / Restore", "Close"
    };
    cairo_t* cr = task_menu_cairo_;
    cairo_set_source_rgb(cr, PanelTheme::background.red,
                         PanelTheme::background.green, PanelTheme::background.blue);
    cairo_paint(cr);
    for (int row = 0; row < 4; ++row) {
        const double top = row * task_menu_row_height_;
        if (row == task_menu_hovered_) {
            cairo_set_source_rgb(cr, 221.0 / 255.0, 221.0 / 255.0, 221.0 / 255.0);
            cairo_rectangle(cr, 1.0, top + 1.0, task_menu_width_ - 2.0,
                            task_menu_row_height_);
            cairo_fill(cr);
        }
        const char* label = row == 2
            ? (task_menu_maximized_ ? "Restore" : "Maximize") : labels[row];
        PangoLayout* layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, label, -1);
        PangoFontDescription* font = pango_font_description_from_string("Sans 9");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        int text_height = 0;
        pango_layout_get_pixel_size(layout, nullptr, &text_height);
        cairo_set_source_rgb(cr, PanelTheme::text.red,
                             PanelTheme::text.green, PanelTheme::text.blue);
        cairo_move_to(cr, 10.0, top + (task_menu_row_height_ - text_height) / 2.0);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }
    cairo_set_source_rgb(cr, PanelTheme::border.red,
                         PanelTheme::border.green, PanelTheme::border.blue);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 0.5, 0.5, task_menu_width_ - 1.0,
                    task_menu_row_height_ * 4 - 1.0);
    cairo_stroke(cr);
    cairo_surface_flush(task_menu_surface_);
    xcb_flush(connection_);
}

void X11Backend::handle_task_menu_click(int x, int y)
{
    const int row = y / task_menu_row_height_;
    if (x < 0 || x >= task_menu_width_ || row < 0 || row >= 4) {
        hide_task_menu();
        return;
    }
    const std::string target = task_menu_target_;
    hide_task_menu();
    switch (row) {
    case 0:
        perform_task_action(target, TaskAction::Activate);
        break;
    case 1:
        perform_task_action(target, TaskAction::Minimize);
        break;
    case 2:
        perform_task_action(target, TaskAction::ToggleMaximize);
        break;
    case 3:
        perform_task_action(target, TaskAction::Close);
        break;
    }
}

void X11Backend::hide_task_menu()
{
    if (task_menu_pointer_grabbed_ && connection_ != nullptr) {
        xcb_ungrab_pointer(connection_, XCB_CURRENT_TIME);
        task_menu_pointer_grabbed_ = false;
    }
    if (task_menu_window_ != XCB_WINDOW_NONE && connection_ != nullptr) {
        xcb_unmap_window(connection_, task_menu_window_);
        xcb_flush(connection_);
    }
    task_menu_target_.clear();
}

void X11Backend::destroy_task_menu()
{
    hide_task_menu();
    if (task_menu_cairo_ != nullptr) {
        cairo_destroy(task_menu_cairo_);
        task_menu_cairo_ = nullptr;
    }
    if (task_menu_surface_ != nullptr) {
        cairo_surface_destroy(task_menu_surface_);
        task_menu_surface_ = nullptr;
    }
    if (connection_ != nullptr && task_menu_window_ != XCB_WINDOW_NONE) {
        xcb_destroy_window(connection_, task_menu_window_);
        xcb_flush(connection_);
    }
    task_menu_window_ = XCB_WINDOW_NONE;
}

void X11Backend::show_tooltip(const std::string& text, int x, int y)
{
    if (text.empty()) {
        hide_tooltip();
        return;
    }
    if (tooltip_visible_ && tooltip_text_ == text) {
        return;
    }
    if (tooltip_window_ != XCB_WINDOW_NONE && tooltip_text_ != text) {
        destroy_tooltip();
    }

    if (tooltip_window_ == XCB_WINDOW_NONE) {
        PangoLayout* layout = pango_cairo_create_layout(cairo_);
        pango_layout_set_text(layout, text.c_str(), -1);
        PangoFontDescription* font = pango_font_description_from_string("Sans 9");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        int text_width = 0;
        int text_height = 0;
        pango_layout_get_pixel_size(layout, &text_width, &text_height);
        g_object_unref(layout);
        tooltip_width_ = text_width + 18;
        tooltip_height_ = text_height + 12;
        tooltip_text_ = text;

        const uint32_t event_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_LEAVE_WINDOW;
        const uint32_t values[] = {screen_->white_pixel, 1, event_mask};
        tooltip_window_ = xcb_generate_id(connection_);
        xcb_create_window(connection_, XCB_COPY_FROM_PARENT, tooltip_window_, screen_->root,
                          0, 0, static_cast<uint16_t>(tooltip_width_),
                          static_cast<uint16_t>(tooltip_height_), 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen_->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
                              XCB_CW_EVENT_MASK, values);

        const xcb_atom_t name_atom = intern_atom(connection_, "_NET_WM_NAME");
        const xcb_atom_t utf8_atom = intern_atom(connection_, "UTF8_STRING");
        constexpr char title[] = "Simpanel Tooltip";
        if (name_atom != XCB_ATOM_NONE && utf8_atom != XCB_ATOM_NONE) {
            xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, tooltip_window_, name_atom,
                                utf8_atom, 8, sizeof(title) - 1, title);
        }

        xcb_visualtype_t* visual = find_visual(screen_, screen_->root_visual);
        if (visual == nullptr) {
            destroy_tooltip();
            return;
        }
        tooltip_surface_ = cairo_xcb_surface_create(connection_, tooltip_window_, visual,
                                                    tooltip_width_, tooltip_height_);
        if (cairo_surface_status(tooltip_surface_) != CAIRO_STATUS_SUCCESS) {
            destroy_tooltip();
            return;
        }
        tooltip_cairo_ = cairo_create(tooltip_surface_);
        if (cairo_status(tooltip_cairo_) != CAIRO_STATUS_SUCCESS) {
            destroy_tooltip();
            return;
        }
        draw_tooltip();
    }

    const int root_x = panel_x_ + x;
    const int root_y = panel_y_ + y;
    if (edge_ == Edge::Left || edge_ == Edge::Right) {
        tooltip_x_ = edge_ == Edge::Left ? panel_x_ + width_ + 8 : panel_x_ - tooltip_width_ - 8;
        if (tooltip_x_ < 0 || tooltip_x_ + tooltip_width_ > screen_->width_in_pixels) {
            tooltip_x_ = edge_ == Edge::Left ? panel_x_ - tooltip_width_ - 8
                                             : panel_x_ + width_ + 8;
        }
        tooltip_y_ = root_y - tooltip_height_ / 2;
    } else {
        tooltip_x_ = root_x + 12;
        int tooltip_y = root_y - tooltip_height_ - 8;
        if (tooltip_y < 0) {
            tooltip_y = root_y + height_ + 8;
        }
        tooltip_y_ = tooltip_y;
    }
    tooltip_x_ = std::clamp(tooltip_x_, 0,
        std::max(0, static_cast<int>(screen_->width_in_pixels) - tooltip_width_));
    tooltip_y_ = std::clamp(tooltip_y_, 0,
        std::max(0, static_cast<int>(screen_->height_in_pixels) - tooltip_height_));
    const uint32_t coordinates[] = {
        static_cast<uint32_t>(tooltip_x_), static_cast<uint32_t>(tooltip_y_)
    };
    xcb_configure_window(connection_, tooltip_window_,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, coordinates);
    tooltip_visible_ = true;
    xcb_map_window(connection_, tooltip_window_);
    const uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(connection_, tooltip_window_, XCB_CONFIG_WINDOW_STACK_MODE,
                         &stack_mode);
    xcb_flush(connection_);
}

void X11Backend::hide_tooltip()
{
    if (!tooltip_visible_) {
        return;
    }
    tooltip_visible_ = false;
    xcb_unmap_window(connection_, tooltip_window_);
    xcb_flush(connection_);
}

void X11Backend::draw_tooltip()
{
    if (tooltip_cairo_ == nullptr) {
        return;
    }

    cairo_set_source_rgb(tooltip_cairo_, PanelTheme::background.red,
                         PanelTheme::background.green, PanelTheme::background.blue);
    cairo_paint(tooltip_cairo_);
    cairo_set_source_rgb(tooltip_cairo_, PanelTheme::border.red,
                         PanelTheme::border.green, PanelTheme::border.blue);
    cairo_set_line_width(tooltip_cairo_, 1.0);
    cairo_rectangle(tooltip_cairo_, 0.5, 0.5, tooltip_width_ - 1.0, tooltip_height_ - 1.0);
    cairo_stroke(tooltip_cairo_);

    PangoLayout* layout = pango_cairo_create_layout(tooltip_cairo_);
    pango_layout_set_text(layout, tooltip_text_.c_str(), -1);
    PangoFontDescription* font = pango_font_description_from_string("Sans 9");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    int text_height = 0;
    pango_layout_get_pixel_size(layout, nullptr, &text_height);
    cairo_set_source_rgb(tooltip_cairo_, PanelTheme::text.red,
                         PanelTheme::text.green, PanelTheme::text.blue);
    cairo_move_to(tooltip_cairo_, 9.0, (tooltip_height_ - text_height) / 2.0);
    pango_cairo_show_layout(tooltip_cairo_, layout);
    g_object_unref(layout);
    cairo_surface_flush(tooltip_surface_);
    xcb_flush(connection_);
}

void X11Backend::destroy_tooltip()
{
    if (tooltip_cairo_ != nullptr) {
        cairo_destroy(tooltip_cairo_);
        tooltip_cairo_ = nullptr;
    }
    if (tooltip_surface_ != nullptr) {
        cairo_surface_destroy(tooltip_surface_);
        tooltip_surface_ = nullptr;
    }
    if (connection_ != nullptr && tooltip_window_ != XCB_WINDOW_NONE) {
        xcb_destroy_window(connection_, tooltip_window_);
        xcb_flush(connection_);
    }
    tooltip_window_ = XCB_WINDOW_NONE;
    tooltip_visible_ = false;
    tooltip_text_.clear();
}

void X11Backend::toggle_calendar(int anchor_x, int anchor_width)
{
    if (calendar_visible_) {
        hide_calendar();
        return;
    }

    if (calendar_window_ == XCB_WINDOW_NONE) {
        create_calendar();
    }
    if (calendar_window_ == XCB_WINDOW_NONE || calendar_cairo_ == nullptr) {
        return;
    }
    position_calendar(anchor_x, anchor_width);

    const std::time_t now = std::time(nullptr);
    std::tm local_time{};
    localtime_r(&now, &local_time);
    calendar_year_ = local_time.tm_year + 1900;
    calendar_month_ = local_time.tm_mon + 1;

    calendar_visible_ = true;
    draw_calendar();
    xcb_map_window(connection_, calendar_window_);
    const uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(connection_, calendar_window_, XCB_CONFIG_WINDOW_STACK_MODE,
                         &stack_mode);
    xcb_set_input_focus(connection_, XCB_INPUT_FOCUS_PARENT, calendar_window_, XCB_CURRENT_TIME);
    xcb_flush(connection_);
}

void X11Backend::position_calendar(int anchor_x, int anchor_width)
{
    int x = 0;
    int y = 0;
    position_popup(anchor_x, anchor_width, calendar_width, calendar_height, x, y);
    const uint32_t coordinates[] = {
        static_cast<uint32_t>(x), static_cast<uint32_t>(y)
    };
    xcb_configure_window(connection_, calendar_window_,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, coordinates);
}

void X11Backend::position_popup(int anchor_position, int anchor_extent,
                                int popup_width, int popup_height, int& x, int& y) const
{
    const int screen_width = screen_->width_in_pixels;
    const int screen_height = screen_->height_in_pixels;
    if (edge_ == Edge::Left || edge_ == Edge::Right) {
        y = panel_y_ + anchor_position + (anchor_extent - popup_height) / 2;
        x = edge_ == Edge::Left ? panel_x_ + width_ : panel_x_ - popup_width;
        if (x < 0 || x + popup_width > screen_width) {
            x = edge_ == Edge::Left ? panel_x_ - popup_width : panel_x_ + width_;
        }
    } else {
        x = panel_x_ + anchor_position + (anchor_extent - popup_width) / 2;
        y = edge_ == Edge::Top ? panel_y_ + height_ : panel_y_ - popup_height;
        if (y < 0 || y + popup_height > screen_height) {
            y = edge_ == Edge::Top ? panel_y_ - popup_height : panel_y_ + height_;
        }
    }
    x = std::clamp(x, 0, std::max(0, screen_width - popup_width));
    y = std::clamp(y, 0, std::max(0, screen_height - popup_height));
}

void X11Backend::create_calendar()
{
    const uint32_t event_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
                                XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_KEY_PRESS |
                                XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW;
    const uint32_t values[] = {screen_->white_pixel, 1, event_mask};
    calendar_window_ = xcb_generate_id(connection_);
    xcb_create_window(connection_, XCB_COPY_FROM_PARENT, calendar_window_, screen_->root,
                      0, 0, calendar_width, calendar_height, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen_->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
                          XCB_CW_EVENT_MASK, values);

    const xcb_atom_t window_type = intern_atom(connection_, "_NET_WM_WINDOW_TYPE");
    const xcb_atom_t popup_type = intern_atom(connection_, "_NET_WM_WINDOW_TYPE_POPUP_MENU");
    if (window_type != XCB_ATOM_NONE && popup_type != XCB_ATOM_NONE) {
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, calendar_window_, window_type,
                            XCB_ATOM_ATOM, 32, 1, &popup_type);
    }
    const xcb_atom_t name_atom = intern_atom(connection_, "_NET_WM_NAME");
    const xcb_atom_t utf8_atom = intern_atom(connection_, "UTF8_STRING");
    constexpr char title[] = "Calendar";
    if (name_atom != XCB_ATOM_NONE && utf8_atom != XCB_ATOM_NONE) {
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, calendar_window_, name_atom,
                            utf8_atom, 8, sizeof(title) - 1, title);
    }
    xcb_visualtype_t* visual = find_visual(screen_, screen_->root_visual);
    if (visual == nullptr) {
        xcb_destroy_window(connection_, calendar_window_);
        calendar_window_ = XCB_WINDOW_NONE;
        return;
    }
    calendar_surface_ = cairo_xcb_surface_create(connection_, calendar_window_, visual,
                                                 calendar_width, calendar_height);
    if (cairo_surface_status(calendar_surface_) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(calendar_surface_);
        calendar_surface_ = nullptr;
        xcb_destroy_window(connection_, calendar_window_);
        calendar_window_ = XCB_WINDOW_NONE;
        return;
    }
    calendar_cairo_ = cairo_create(calendar_surface_);
}

void X11Backend::draw_calendar()
{
    if (calendar_cairo_ == nullptr) {
        return;
    }

    cairo_t* cr = calendar_cairo_;
    cairo_set_source_rgb(cr, PanelTheme::background.red,
                         PanelTheme::background.green, PanelTheme::background.blue);
    cairo_paint(cr);

    std::tm month_date{};
    month_date.tm_year = calendar_year_ - 1900;
    month_date.tm_mon = calendar_month_ - 1;
    month_date.tm_mday = 1;
    mktime(&month_date);
    char month_title[64]{};
    std::strftime(month_title, sizeof(month_title), "%B %Y", &month_date);
    draw_centered_text(cr, "<", 26.0, 8.0, "Sans Bold 14", PanelTheme::text);
    draw_centered_text(cr, month_title, calendar_width / 2.0, 8.0,
                       "Sans Bold 10", PanelTheme::text);
    draw_centered_text(cr, ">", calendar_width - 26.0, 8.0,
                       "Sans Bold 14", PanelTheme::text);

    constexpr std::array<const char*, 7> weekdays = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    constexpr double grid_left = 10.0;
    constexpr double cell_width = 33.0;
    constexpr double grid_top = 48.0;
    constexpr double cell_height = 32.0;
    const RgbColor muted_text{90.0 / 255.0, 90.0 / 255.0, 90.0 / 255.0};
    for (std::size_t column = 0; column < weekdays.size(); ++column) {
        draw_centered_text(cr, weekdays[column], grid_left + (column + 0.5) * cell_width,
                           grid_top, "Sans 9", muted_text);
    }

    std::tm first_day{};
    first_day.tm_year = calendar_year_ - 1900;
    first_day.tm_mon = calendar_month_ - 1;
    first_day.tm_mday = 1;
    mktime(&first_day);
    const int first_column = (first_day.tm_wday + 6) % 7;

    std::tm last_day{};
    last_day.tm_year = calendar_year_ - 1900;
    last_day.tm_mon = calendar_month_;
    last_day.tm_mday = 0;
    mktime(&last_day);

    const std::time_t now = std::time(nullptr);
    std::tm today{};
    localtime_r(&now, &today);
    for (int day = 1; day <= last_day.tm_mday; ++day) {
        const int cell = first_column + day - 1;
        const int row = cell / 7;
        const int column = cell % 7;
        const double center_x = grid_left + (column + 0.5) * cell_width;
        const double top_y = grid_top + 22.0 + row * cell_height;
        const bool is_today = calendar_year_ == today.tm_year + 1900 &&
                              calendar_month_ == today.tm_mon + 1 && day == today.tm_mday;
        if (is_today) {
            cairo_set_source_rgb(cr, PanelTheme::text.red,
                                 PanelTheme::text.green, PanelTheme::text.blue);
            cairo_arc(cr, center_x, top_y + 12.0, 11.0, 0.0, 6.283185307179586);
            cairo_fill(cr);
        }
        const RgbColor color = is_today
            ? RgbColor{1.0, 1.0, 1.0} : PanelTheme::text;
        draw_centered_text(cr, std::to_string(day), center_x, top_y,
                           "Sans 9", color);
    }

    cairo_surface_flush(calendar_surface_);
    xcb_flush(connection_);
}

void X11Backend::change_calendar_month(int delta)
{
    calendar_month_ += delta;
    if (calendar_month_ < 1) {
        calendar_month_ = 12;
        --calendar_year_;
    } else if (calendar_month_ > 12) {
        calendar_month_ = 1;
        ++calendar_year_;
    }
    draw_calendar();
}

void X11Backend::hide_calendar()
{
    if (!calendar_visible_) {
        return;
    }
    calendar_visible_ = false;
    if (calendar_window_ != XCB_WINDOW_NONE) {
        xcb_unmap_window(connection_, calendar_window_);
        xcb_flush(connection_);
    }
}

void X11Backend::handle_calendar_click(int x, int y)
{
    if (y < 42 && x < 52) {
        change_calendar_month(-1);
    } else if (y < 42 && x >= calendar_width - 52) {
        change_calendar_month(1);
    }
}

bool X11Backend::supports_workspaces() const
{
    ensure_gnome_bridge();
    if (gnome_bridge_ != nullptr) {
        return gnome_bridge_->supports_workspaces();
    }
    if (gnome_wayland_session_) {
        return true;
    }
    if (connection_ == nullptr || screen_ == nullptr) {
        return false;
    }
    const bool current_desktop =
        has_cardinal_property(connection_, screen_->root, "_NET_CURRENT_DESKTOP") ||
        has_cardinal_property(connection_, screen_->root, "_WIN_WORKSPACE");
    const bool desktop_count =
        has_cardinal_property(connection_, screen_->root, "_NET_NUMBER_OF_DESKTOPS") ||
        has_cardinal_property(connection_, screen_->root, "_WIN_WORKSPACE_COUNT");
    return (current_desktop && desktop_count) ||
        !SystemCommands::find_executable("wmctrl").empty();
}

WorkspaceSnapshot X11Backend::get_workspaces()
{
    ensure_gnome_bridge();
    if (gnome_bridge_ != nullptr) {
        return gnome_bridge_->get_workspaces();
    }
    if (gnome_wayland_session_) {
        return {};
    }
    WorkspaceSnapshot snapshot;
    uint32_t desktop_count = 0;
    uint32_t current_desktop = 0;
    if (connection_ != nullptr && screen_ != nullptr &&
        read_cardinal_property(connection_, screen_->root, "_NET_NUMBER_OF_DESKTOPS",
                               desktop_count) && desktop_count > 0 &&
        read_cardinal_property(connection_, screen_->root, "_NET_CURRENT_DESKTOP",
                               current_desktop)) {
        snapshot.ids.reserve(desktop_count);
        for (uint32_t id = 0; id < desktop_count; ++id) {
            snapshot.ids.push_back(static_cast<int>(id));
        }
        snapshot.current_id = current_desktop < desktop_count
            ? static_cast<int>(current_desktop) : 0;
        return snapshot;
    }
    const auto result = SystemCommands::run({"wmctrl", "-d"});
    if (result.exit_code != 0) {
        return snapshot;
    }
    std::istringstream lines(result.output);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        int id = -1;
        char active = ' ';
        if (!(fields >> id >> active)) {
            continue;
        }
        snapshot.ids.push_back(id);
        if (active == '*') {
            snapshot.current_id = id;
        }
    }
    return snapshot;
}

bool X11Backend::configure_workspace_count(int count)
{
    ensure_gnome_bridge();
    if (gnome_bridge_ != nullptr) {
        return gnome_bridge_->configure_workspace_count(count);
    }
    if (gnome_wayland_session_) {
        return count > 0;
    }
    uint32_t current_count = 0;
    if (connection_ != nullptr && screen_ != nullptr &&
        read_cardinal_property(connection_, screen_->root, "_NET_NUMBER_OF_DESKTOPS",
                               current_count)) {
        const uint32_t requested_count = static_cast<uint32_t>(std::max(1, count));
        if (requested_count != current_count) {
            send_root_client_message(connection_, screen_, "_NET_NUMBER_OF_DESKTOPS",
                                     requested_count, 0);
        }
        return true;
    }
    return SystemCommands::run({"wmctrl", "-n", std::to_string(std::max(1, count))}).exit_code == 0;
}

bool X11Backend::switch_workspace(int id)
{
    ensure_gnome_bridge();
    if (gnome_bridge_ != nullptr) {
        return gnome_bridge_->switch_workspace(id);
    }
    if (gnome_wayland_session_) {
        return false;
    }
    if (connection_ != nullptr && screen_ != nullptr && id >= 0 &&
        has_cardinal_property(connection_, screen_->root, "_NET_CURRENT_DESKTOP")) {
        send_root_client_message(connection_, screen_, "_NET_CURRENT_DESKTOP",
                                 static_cast<uint32_t>(id), XCB_CURRENT_TIME);
        return true;
    }
    return SystemCommands::run({"wmctrl", "-s", std::to_string(id)}).exit_code == 0;
}

bool X11Backend::supports_show_desktop() const
{
    ensure_gnome_bridge();
    if (gnome_bridge_ != nullptr) {
        return gnome_bridge_->supports_show_desktop();
    }
    if (gnome_wayland_session_) {
        return true;
    }
    return connection_ != nullptr && !SystemCommands::find_executable("wmctrl").empty();
}

bool X11Backend::get_show_desktop_state(bool& showing)
{
    ensure_gnome_bridge();
    if (gnome_bridge_ != nullptr) {
        return gnome_bridge_->get_show_desktop_state(showing);
    }
    if (gnome_wayland_session_) {
        return false;
    }
    const std::vector<std::string> arguments{"wmctrl", "-m"};
    const auto result = SystemCommands::run(arguments);
    if (result.exit_code != 0) {
        return false;
    }
    showing = result.output.find("mode: ON") != std::string::npos;
    return true;
}

bool X11Backend::toggle_show_desktop()
{
    ensure_gnome_bridge();
    if (gnome_bridge_ != nullptr) {
        return gnome_bridge_->toggle_show_desktop();
    }
    if (gnome_wayland_session_) {
        return false;
    }
    bool showing = false;
    if (!get_show_desktop_state(showing)) {
        return false;
    }
    return SystemCommands::run({"wmctrl", "-k", showing ? "off" : "on"}).exit_code == 0;
}

bool X11Backend::supports_xkill() const
{
    return connection_ != nullptr && !gnome_wayland_session_ &&
        !SystemCommands::find_executable("xkill").empty();
}

bool X11Backend::supports_native_window_kill() const
{
    return ensure_gnome_bridge() && gnome_bridge_->supports_window_kill_mode();
}

bool X11Backend::start_native_window_kill()
{
    return ensure_gnome_bridge() && gnome_bridge_->begin_window_kill_mode();
}

bool X11Backend::native_window_kill_active() const
{
    return ensure_gnome_bridge() && gnome_bridge_->window_kill_mode_active();
}

int X11Backend::width() const
{
    return width_;
}

int X11Backend::height() const
{
    return height_;
}

void X11Backend::set_dock_properties(Edge edge)
{
    const xcb_atom_t atom_type = intern_atom(connection_, "_NET_WM_WINDOW_TYPE");
    const xcb_atom_t dock_type = intern_atom(connection_, "_NET_WM_WINDOW_TYPE_DOCK");
    if (atom_type != XCB_ATOM_NONE && dock_type != XCB_ATOM_NONE) {
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, window_, atom_type,
                            XCB_ATOM_ATOM, 32, 1, &dock_type);
    }

    const xcb_atom_t atom_name = intern_atom(connection_, "_NET_WM_NAME");
    const xcb_atom_t utf8 = intern_atom(connection_, "UTF8_STRING");
    constexpr char name[] = "Simpanel Cairo";
    if (atom_name != XCB_ATOM_NONE && utf8 != XCB_ATOM_NONE) {
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, window_, atom_name,
                            utf8, 8, sizeof(name) - 1, name);
    }

    const std::array<const char*, 4> state_names = {
        "_NET_WM_STATE_ABOVE", "_NET_WM_STATE_STICKY",
        "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_SKIP_PAGER"
    };
    std::array<xcb_atom_t, state_names.size()> states{};
    for (std::size_t index = 0; index < state_names.size(); ++index) {
        states[index] = intern_atom(connection_, state_names[index]);
    }
    const xcb_atom_t state_property = intern_atom(connection_, "_NET_WM_STATE");
    if (state_property != XCB_ATOM_NONE) {
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, window_, state_property,
                            XCB_ATOM_ATOM, 32, states.size(), states.data());
    }

    const uint32_t all_desktops = std::numeric_limits<uint32_t>::max();
    set_cardinals(connection_, window_, intern_atom(connection_, "_NET_WM_DESKTOP"),
                  &all_desktops, 1);

    std::array<uint32_t, 12> partial_strut{};
    std::array<uint32_t, 4> strut{};
    if (edge == Edge::Top) {
        strut[2] = static_cast<uint32_t>(height_);
        partial_strut[2] = static_cast<uint32_t>(height_);
        partial_strut[8] = static_cast<uint32_t>(panel_x_);
        partial_strut[9] = static_cast<uint32_t>(panel_x_ + width_ - 1);
    } else if (edge == Edge::Bottom) {
        strut[3] = static_cast<uint32_t>(height_);
        partial_strut[3] = static_cast<uint32_t>(height_);
        partial_strut[10] = static_cast<uint32_t>(panel_x_);
        partial_strut[11] = static_cast<uint32_t>(panel_x_ + width_ - 1);
    } else if (edge == Edge::Left) {
        strut[0] = static_cast<uint32_t>(width_);
        partial_strut[0] = static_cast<uint32_t>(width_);
        partial_strut[4] = static_cast<uint32_t>(panel_y_);
        partial_strut[5] = static_cast<uint32_t>(panel_y_ + height_ - 1);
    } else {
        strut[1] = static_cast<uint32_t>(width_);
        partial_strut[1] = static_cast<uint32_t>(width_);
        partial_strut[6] = static_cast<uint32_t>(panel_y_);
        partial_strut[7] = static_cast<uint32_t>(panel_y_ + height_ - 1);
    }
    set_cardinals(connection_, window_, intern_atom(connection_, "_NET_WM_STRUT"),
                  strut.data(), strut.size());
    set_cardinals(connection_, window_, intern_atom(connection_, "_NET_WM_STRUT_PARTIAL"),
                  partial_strut.data(), partial_strut.size());
}
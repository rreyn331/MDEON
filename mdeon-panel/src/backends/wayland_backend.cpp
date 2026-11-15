#include "panel/backends/wayland_backend.hpp"
#include "panel/logging.hpp"
#include "panel/backends/wayland_task_backend.hpp"
#include "panel/icon_theme.hpp"
#include "panel/modules/system_commands.hpp"
#include "panel/theme.hpp"

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "plasma-shell-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <wayland-client.h>
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

class WaylandBackendImpl {
public:
    struct Output {
        uint32_t global_name;
        wl_output* proxy;
        std::string name;
    };

    explicit WaylandBackendImpl(WaylandBackend& backend) : backend_(backend) {}

    ~WaylandBackendImpl()
    {
        power_menu_return_state_.reset();
        if (menu_popup_ != nullptr) {
            close_menu_popup(false);
        }
        if (task_menu_popup_ != nullptr) {
            close_task_menu_popup();
        }
        close_tooltip_popup();
        if (display_ != nullptr) {
            if (surface_ != nullptr) {
                wl_surface_attach(surface_, nullptr, 0, 0);
                wl_surface_commit(surface_);
            }
            if (layer_surface_ != nullptr) {
                zwlr_layer_surface_v1_destroy(layer_surface_);
            }
            if (panel_toplevel_ != nullptr) {
                xdg_toplevel_destroy(panel_toplevel_);
            }
            if (plasma_surface_ != nullptr) {
                org_kde_plasma_surface_destroy(plasma_surface_);
            }
            if (panel_xdg_surface_ != nullptr) {
                xdg_surface_destroy(panel_xdg_surface_);
            }
            if (surface_ != nullptr) {
                wl_surface_destroy(surface_);
            }
            wl_display_flush(display_);
            wl_display_roundtrip(display_);
        }

        destroy_buffers();
        if (pointer_ != nullptr) {
            wl_pointer_destroy(pointer_);
        }
        if (keyboard_ != nullptr) {
            wl_keyboard_destroy(keyboard_);
        }
        if (seat_ != nullptr) {
            wl_seat_destroy(seat_);
        }
        if (layer_shell_ != nullptr) {
            wl_proxy_destroy(reinterpret_cast<wl_proxy*>(layer_shell_));
        }
        if (plasma_shell_ != nullptr) {
            org_kde_plasma_shell_destroy(plasma_shell_);
        }
        if (wm_base_ != nullptr) {
            xdg_wm_base_destroy(wm_base_);
        }
        if (shm_ != nullptr) {
            wl_shm_destroy(shm_);
        }
        for (auto& output : outputs_) {
            wl_output_destroy(output.proxy);
        }
        if (compositor_ != nullptr) {
            wl_compositor_destroy(compositor_);
        }
        if (registry_ != nullptr) {
            wl_registry_destroy(registry_);
        }
        if (display_ != nullptr) {
            wl_display_disconnect(display_);
        }
        if (xkb_state_ != nullptr) {
            xkb_state_unref(xkb_state_);
        }
        if (xkb_keymap_ != nullptr) {
            xkb_keymap_unref(xkb_keymap_);
        }
        if (xkb_context_ != nullptr) {
            xkb_context_unref(xkb_context_);
        }
        g_clear_object(&kwin_connection_);
    }

    bool connect()
    {
        display_ = wl_display_connect(nullptr);
        if (display_ == nullptr) {
            std::cerr << "Could not connect to the Wayland display.\n";
            return false;
        }
        registry_ = wl_display_get_registry(display_);
        static const wl_registry_listener listener = {registry_global, registry_remove};
        wl_registry_add_listener(registry_, &listener, this);
        if (wl_display_roundtrip(display_) < 0) {
            std::cerr << "Failed to read Wayland compositor globals.\n";
            return false;
        }
        if (wl_display_roundtrip(display_) < 0) {
            std::cerr << "Failed to read Wayland output names.\n";
            return false;
        }
        if (compositor_ == nullptr || shm_ == nullptr || wm_base_ == nullptr) {
            std::cerr << "The compositor must provide wl_compositor, wl_shm, and xdg-shell.\n";
            return false;
        }
        const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
        const char* session = std::getenv("XDG_SESSION_TYPE");
        panel_debug("Wayland globals: desktop=", desktop != nullptr ? desktop : "(unset)",
            " session=", session != nullptr ? session : "(unset)",
            " xdg_shell=", wm_base_ != nullptr,
            " wl_shm=", shm_ != nullptr,
            " layer_shell=", layer_shell_ != nullptr,
            " plasma_shell=", plasma_shell_ != nullptr,
            " seat=", seat_ != nullptr);
        xkb_context_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (xkb_context_ == nullptr) {
            std::cerr << "Could not create the XKB context.\n";
            return false;
        }
        return true;
    }

    std::vector<std::string> output_names() const
    {
        std::vector<std::string> names;
        names.reserve(outputs_.size());
        for (const auto& output : outputs_) {
            names.push_back(output.name);
        }
        return names;
    }

    bool create_layer_surface(Edge edge, int requested_width, int requested_height,
                              const std::string& monitor)
    {
        edge_ = edge;
        requested_width_ = requested_width;
        requested_height_ = requested_height;
        wl_output* target_output = nullptr;
        if (monitor != "all") {
            if (monitor == "primary" && !outputs_.empty()) {
                target_output = outputs_.front().proxy;
            } else {
                const auto selected = std::find_if(outputs_.begin(), outputs_.end(),
                    [&monitor](const Output& output) { return output.name == monitor; });
                if (selected == outputs_.end()) {
                    std::cerr << "Could not find Wayland monitor '" << monitor << "'.\n";
                    return false;
                }
                target_output = selected->proxy;
            }
        }
        surface_ = wl_compositor_create_surface(compositor_);
        if (surface_ == nullptr) {
            std::cerr << "Failed to create a Wayland surface.\n";
            return false;
        }
        if (layer_shell_ == nullptr) {
            if (monitor != "all") {
                std::cerr << "Selecting a monitor requires Wayland layer-shell support.\n";
                return false;
            }
            return create_toplevel_panel_surface(requested_width, requested_height);
        }
        const char* scope = plasma_shell_ != nullptr ? "dock" : "mdeon-panel";
        layer_surface_ = zwlr_layer_shell_v1_get_layer_surface(
            layer_shell_, surface_, target_output, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
            scope);
        if (layer_surface_ == nullptr) {
            std::cerr << "Failed to create the layer-shell surface.\n";
            return false;
        }
        static const zwlr_layer_surface_v1_listener listener = {
            layer_configure, layer_closed
        };
        zwlr_layer_surface_v1_add_listener(layer_surface_, &listener, this);

        const bool vertical = edge == Edge::Left || edge == Edge::Right;
        uint32_t anchors = vertical
            ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
            : ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        if (edge == Edge::Top) anchors |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
        if (edge == Edge::Bottom) anchors |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
        if (edge == Edge::Left) anchors |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
        if (edge == Edge::Right) anchors |= ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        zwlr_layer_surface_v1_set_anchor(layer_surface_, anchors);
        zwlr_layer_surface_v1_set_size(layer_surface_,
            requested_width > 0 ? static_cast<uint32_t>(requested_width) : 0,
            requested_height > 0 ? static_cast<uint32_t>(requested_height) : 0);
        zwlr_layer_surface_v1_set_exclusive_zone(layer_surface_,
            vertical ? requested_width : requested_height);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            layer_surface_, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

        wl_surface_commit(surface_);
        if (wl_display_roundtrip(display_) < 0 || !configured_ ||
            backend_.width_ <= 0 || backend_.height_ <= 0) {
            std::cerr << "The compositor did not configure a usable panel surface.\n";
            return false;
        }
        const bool buffers_ready = create_buffers();
        if (buffers_ready) {
            panel_debug("Wayland panel surface ready: shell=layer-shell scope=", scope,
                " size=", backend_.width_, "x", backend_.height_);
        }
        return buffers_ready;
    }

    bool create_plasma_panel_surface()
    {
        if (plasma_shell_ == nullptr) return true;
        plasma_surface_ = org_kde_plasma_shell_get_surface(plasma_shell_, surface_);
        if (plasma_surface_ == nullptr) {
            std::cerr << "Failed to create the Plasma shell surface.\n";
            return false;
        }
        org_kde_plasma_surface_set_role(plasma_surface_, 2);
        return true;
    }

    bool create_toplevel_panel_surface(int requested_width, int requested_height)
    {
        panel_xdg_surface_ = xdg_wm_base_get_xdg_surface(wm_base_, surface_);
        if (panel_xdg_surface_ == nullptr) {
            std::cerr << "Failed to create the fallback xdg surface.\n";
            return false;
        }
        static const xdg_surface_listener surface_listener = {panel_surface_configure};
        xdg_surface_add_listener(panel_xdg_surface_, &surface_listener, this);
        panel_toplevel_ = xdg_surface_get_toplevel(panel_xdg_surface_);
        if (panel_toplevel_ == nullptr) {
            std::cerr << "Failed to create the fallback xdg toplevel.\n";
            return false;
        }
        static const xdg_toplevel_listener toplevel_listener = {
            .configure = panel_toplevel_configure,
            .close = panel_toplevel_close,
            .configure_bounds = panel_toplevel_configure_bounds,
            .wm_capabilities = panel_toplevel_wm_capabilities
        };
        xdg_toplevel_add_listener(panel_toplevel_, &toplevel_listener, this);
        xdg_toplevel_set_title(panel_toplevel_, "mdeon-panel");
        xdg_toplevel_set_app_id(panel_toplevel_, "org.mdeon.Panel");
        if (!create_plasma_panel_surface()) return false;
        if (requested_width > 0 || requested_height > 0) {
            xdg_toplevel_set_min_size(panel_toplevel_,
                requested_width > 0 ? requested_width : 0,
                requested_height > 0 ? requested_height : 0);
        }
        if (requested_height > 0) {
            xdg_toplevel_set_max_size(panel_toplevel_, 0, requested_height);
        }
        wl_surface_commit(surface_);
        if (wl_display_roundtrip(display_) < 0 || !configured_ ||
            backend_.width_ <= 0 || backend_.height_ <= 0) {
            std::cerr << "The compositor did not configure a usable xdg-toplevel panel.\n";
            return false;
        }
        const bool buffers_ready = create_buffers();
        if (buffers_ready) {
            panel_debug("Wayland panel surface ready: shell=xdg-toplevel plasma_role=",
                plasma_surface_ != nullptr, " size=", backend_.width_, "x", backend_.height_);
        }
        return buffers_ready;
    }

    int event_fd() const
    {
        return display_ == nullptr ? -1 : wl_display_get_fd(display_);
    }

    void process_events()
    {
        if (display_ != nullptr && wl_display_dispatch(display_) < 0) {
            std::cerr << "Wayland event dispatch failed.\n";
        }
        reap_retired_buffers();
        if (menu_popup_ != nullptr && menu_popup_->closed) {
            close_menu_popup(true);
        } else if (menu_popup_ != nullptr && popup_leave_pending_) {
            popup_leave_pending_ = false;
            if (!application_menu_countdown_active_ &&
                pointer_surface_ != menu_popup_->surface && pointer_surface_ != surface_) {
                panel_debug("closing application menu after pointer left its surfaces");
                close_menu_popup(true);
            }
        }
        if (task_menu_popup_ != nullptr && task_menu_popup_->closed) {
            close_task_menu_popup();
        } else if (task_menu_popup_ != nullptr && task_menu_leave_pending_) {
            task_menu_leave_pending_ = false;
            if (pointer_surface_ != task_menu_popup_->surface && pointer_surface_ != surface_) {
                close_task_menu_popup();
            }
        }
        if (tooltip_popup_ != nullptr && tooltip_popup_->closed) {
            close_tooltip_popup();
        }
        reap_retired_tooltip_popups();
        if (calendar_popup_ != nullptr && calendar_popup_->closed) {
            close_calendar_popup();
        }
        if (desktop_hub_popup_ != nullptr && desktop_hub_popup_->closed) {
            close_desktop_hub_popup();
        } else if (desktop_hub_popup_ != nullptr &&
                   (desktop_hub_leave_pending_ || desktop_hub_focus_leave_pending_)) {
            const bool pointer_left = !desktop_hub_countdown_active_ &&
                desktop_hub_leave_pending_ &&
                pointer_surface_ != desktop_hub_popup_->surface && pointer_surface_ != surface_;
            const bool keyboard_left = desktop_hub_focus_leave_pending_ &&
                keyboard_surface_ != desktop_hub_popup_->surface && keyboard_surface_ != surface_;
            desktop_hub_leave_pending_ = false;
            desktop_hub_focus_leave_pending_ = false;
            if (pointer_left || keyboard_left) {
                close_desktop_hub_popup();
            }
        }
    }

    cairo_t* drawing_context()
    {
        if (drawing_buffer_ >= 0) {
            return buffers_[static_cast<std::size_t>(drawing_buffer_)]->cairo;
        }
        for (std::size_t index = 0; index < buffers_.size(); ++index) {
            if (!buffers_[index]->busy) {
                drawing_buffer_ = static_cast<int>(index);
                return buffers_[index]->cairo;
            }
        }
        return nullptr;
    }

    void commit()
    {
        if (drawing_buffer_ < 0 || surface_ == nullptr) {
            return;
        }
        Buffer& buffer = *buffers_[static_cast<std::size_t>(drawing_buffer_)];
        cairo_surface_flush(buffer.cairo_surface);
        wl_surface_attach(surface_, buffer.buffer, 0, 0);
        wl_surface_damage(surface_, 0, 0, backend_.width_, backend_.height_);
        buffer.busy = true;
        drawing_buffer_ = -1;
        wl_surface_commit(surface_);
        if (wl_display_flush(display_) < 0 && errno != EAGAIN) {
            std::cerr << "Failed to flush Wayland requests.\n";
        }
    }

    void configure_panel_positioner(xdg_positioner* positioner, int anchor_position,
                                    int anchor_extent, int popup_width, int popup_height)
    {
        const bool vertical = edge_ == Edge::Left || edge_ == Edge::Right;
        const int anchor_x = vertical
            ? (edge_ == Edge::Left ? backend_.width_ - 1 : 0) : anchor_position;
        const int anchor_y = vertical
            ? anchor_position : (edge_ == Edge::Top ? backend_.height_ - 1 : 0);
        const int anchor_width = vertical ? 1 : std::max(1, anchor_extent);
        const int anchor_height = vertical ? std::max(1, anchor_extent) : 1;
        xdg_positioner_set_size(positioner, popup_width, popup_height);
        xdg_positioner_set_anchor_rect(positioner, anchor_x, anchor_y,
                                       anchor_width, anchor_height);
        const uint32_t anchor = vertical
            ? (edge_ == Edge::Left ? XDG_POSITIONER_ANCHOR_RIGHT
                                   : XDG_POSITIONER_ANCHOR_LEFT)
            : (edge_ == Edge::Top ? XDG_POSITIONER_ANCHOR_BOTTOM
                                  : XDG_POSITIONER_ANCHOR_TOP);
        const uint32_t gravity = vertical
            ? (edge_ == Edge::Left ? XDG_POSITIONER_GRAVITY_RIGHT
                                   : XDG_POSITIONER_GRAVITY_LEFT)
            : (edge_ == Edge::Top ? XDG_POSITIONER_GRAVITY_BOTTOM
                                  : XDG_POSITIONER_GRAVITY_TOP);
        xdg_positioner_set_anchor(positioner, anchor);
        xdg_positioner_set_gravity(positioner, gravity);
        xdg_positioner_set_constraint_adjustment(positioner,
            XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
            XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
            XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
            XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
    }

    void open_application_menu(
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
        if (menu_popup_ != nullptr) {
            power_menu_return_state_.reset();
            close_menu_popup(false);
        }
        menu_popup_ = std::make_unique<MenuPopup>();
        menu_popup_->owner = this;
        menu_popup_->anchor_x = anchor_x;
        menu_popup_->anchor_width = anchor_width;
        menu_popup_->settings = std::move(settings);
        menu_popup_->width = std::clamp(menu_popup_->settings.width, 200, 1600);
        menu_popup_->height = std::clamp(menu_popup_->settings.height, 200, 1200);
        menu_popup_->categories = std::move(categories);
        menu_popup_->items = std::move(items);
        menu_popup_->on_category = std::move(on_category);
        menu_popup_->on_search = std::move(on_search);
        menu_popup_->on_activate = std::move(on_activate);
        menu_popup_->on_shortcut = std::move(on_shortcut);
        menu_popup_->on_power_action = std::move(on_power_action);
        menu_popup_->on_close = std::move(on_close);
        create_menu_popup();
    }

    void update_application_menu(std::string query,
                                 std::vector<ApplicationMenuDisplayItem> items,
                                 std::size_t selected, bool preserve_selection = false)
    {
        if (menu_popup_ == nullptr) {
            return;
        }
        MenuReturnState* parent = menu_popup_->power_menu
            ? power_menu_return_state_.get() : nullptr;
        if (menu_popup_->power_menu && parent == nullptr) {
            return;
        }
        auto& current_query = parent != nullptr ? parent->query : menu_popup_->query;
        auto& current_items = parent != nullptr ? parent->items : menu_popup_->items;
        auto& current_selected = parent != nullptr ? parent->selected : menu_popup_->selected;
        auto& current_scroll = parent != nullptr ? parent->scroll : menu_popup_->scroll;
        const int previous_selected = current_selected;
        current_query = std::move(query);
        current_items = std::move(items);
        current_selected = current_items.empty() ? 0 :
            static_cast<int>(std::min(preserve_selection
                ? static_cast<std::size_t>(std::max(0, previous_selected)) : selected,
                current_items.size() - 1));
        const int footer_height = menu_popup_->settings.show_categories ? 42 : 78;
        const int visible_rows = std::max(1, (menu_popup_->height - footer_height - 8) / 28);
        if (current_selected < static_cast<int>(current_scroll)) {
            current_scroll = static_cast<std::size_t>(current_selected);
        } else if (current_selected >=
                   static_cast<int>(current_scroll) + visible_rows) {
            current_scroll = static_cast<std::size_t>(current_selected - visible_rows + 1);
        }
        render_menu_popup();
    }

    void focus_application_menu_items()
    {
        if (menu_popup_ == nullptr) {
            return;
        }
        menu_popup_->focus = 2;
        render_menu_popup();
    }

    void set_application_menu_countdown_active(bool active)
    {
        if (application_menu_countdown_active_ && !active && menu_popup_ != nullptr) {
            popup_leave_pending_ = true;
        }
        application_menu_countdown_active_ = active;
    }

    void set_application_menu_category(std::string category)
    {
        if (menu_popup_ == nullptr) return;
        MenuReturnState* parent = menu_popup_->power_menu
            ? power_menu_return_state_.get() : nullptr;
        if (menu_popup_->power_menu && parent == nullptr) return;
        auto& current_category = parent != nullptr ? parent->category : menu_popup_->category;
        auto& current_categories = parent != nullptr ? parent->categories : menu_popup_->categories;
        auto& current_scroll = parent != nullptr
            ? parent->category_scroll : menu_popup_->category_scroll;
        current_category = std::move(category);
        const auto found = std::find(current_categories.begin(), current_categories.end(),
                                     current_category);
        if (found != current_categories.end()) {
            const std::size_t selected = static_cast<std::size_t>(
                std::distance(current_categories.begin(), found));
            const int footer_height = menu_popup_->settings.show_categories ? 42 : 78;
            const std::size_t visible_rows = static_cast<std::size_t>(std::max(1,
                (menu_popup_->height - footer_height - 34) / 27));
            if (selected < current_scroll) current_scroll = selected;
            else if (selected >= current_scroll + visible_rows) {
                current_scroll = selected - visible_rows + 1;
            }
        }
        render_menu_popup();
    }

    void set_application_menu_categories(std::vector<std::string> categories)
    {
        if (menu_popup_ == nullptr) return;
        MenuReturnState* parent = menu_popup_->power_menu
            ? power_menu_return_state_.get() : nullptr;
        if (menu_popup_->power_menu && parent == nullptr) return;
        auto& current_categories = parent != nullptr ? parent->categories : menu_popup_->categories;
        auto& current_scroll = parent != nullptr
            ? parent->category_scroll : menu_popup_->category_scroll;
        current_categories = std::move(categories);
        const int footer_height = menu_popup_->settings.show_categories ? 42 : 78;
        const std::size_t visible_rows = static_cast<std::size_t>(std::max(1,
            (menu_popup_->height - footer_height - 34) / 27));
        const std::size_t maximum = current_categories.size() > visible_rows
            ? current_categories.size() - visible_rows : 0;
        current_scroll = std::min(current_scroll, maximum);
        render_menu_popup();
    }

    void toggle_application_power_menu(
        int anchor_x, int anchor_width,
        std::function<void(const std::string&, const std::string&)> on_power_action)
    {
        if (menu_popup_ != nullptr && menu_popup_->power_menu) {
            if (power_menu_return_state_ != nullptr) {
                close_power_menu();
            } else {
                close_menu_popup(true);
            }
            return;
        }
        if (menu_popup_ != nullptr) {
            power_menu_return_state_ = std::make_unique<MenuReturnState>();
            power_menu_return_state_->categories = menu_popup_->categories;
            power_menu_return_state_->items = menu_popup_->items;
            power_menu_return_state_->on_category = menu_popup_->on_category;
            power_menu_return_state_->on_search = menu_popup_->on_search;
            power_menu_return_state_->on_activate = menu_popup_->on_activate;
            power_menu_return_state_->on_power_action = menu_popup_->on_power_action;
            power_menu_return_state_->on_close = menu_popup_->on_close;
            power_menu_return_state_->query = menu_popup_->query;
            power_menu_return_state_->category = menu_popup_->category;
            power_menu_return_state_->anchor_x = menu_popup_->anchor_x;
            power_menu_return_state_->anchor_width = menu_popup_->anchor_width;
            power_menu_return_state_->width = menu_popup_->width;
            power_menu_return_state_->height = menu_popup_->height;
            power_menu_return_state_->selected = menu_popup_->selected;
            power_menu_return_state_->selected_shortcut = menu_popup_->selected_shortcut;
            power_menu_return_state_->scroll = menu_popup_->scroll;
            power_menu_return_state_->category_scroll = menu_popup_->category_scroll;
            power_menu_return_state_->shortcut_scroll = menu_popup_->shortcut_scroll;
            power_menu_return_state_->hovered_category = menu_popup_->hovered_category;
            power_menu_return_state_->focus = 3;
            power_menu_return_state_->tab_held = menu_popup_->tab_held;
            menu_popup_->power_menu = true;
            menu_popup_->selected = 0;
            menu_popup_->focus = 0;
            menu_popup_->on_power_action = std::move(on_power_action);
            render_menu_popup();
            return;
        }
        menu_popup_ = std::make_unique<MenuPopup>();
        menu_popup_->owner = this;
        menu_popup_->anchor_x = anchor_x;
        menu_popup_->anchor_width = anchor_width;
        menu_popup_->power_menu = true;
        menu_popup_->on_power_action = std::move(on_power_action);
        menu_popup_->width = 190;
        menu_popup_->height = 176;
        create_menu_popup();
    }

    void close_application_menu()
    {
        power_menu_return_state_.reset();
        close_menu_popup(true);
    }

    bool application_menu_visible() const
    {
        return menu_popup_ != nullptr &&
            (!menu_popup_->power_menu || power_menu_return_state_ != nullptr) &&
            !menu_popup_->closed;
    }

    void show_tooltip(const std::string& text, int x, int y)
    {
        if (text.empty() || (layer_surface_ == nullptr && panel_xdg_surface_ == nullptr)) {
            close_tooltip_popup();
            return;
        }
        if (tooltip_popup_ != nullptr && !tooltip_popup_->closed &&
            tooltip_popup_->text == text) {
            return;
        }
        close_tooltip_popup();

        cairo_surface_t* measure_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t* measure_cr = cairo_create(measure_surface);
        PangoLayout* layout = pango_cairo_create_layout(measure_cr);
        pango_layout_set_text(layout, text.c_str(), -1);
        PangoFontDescription* font = pango_font_description_from_string("Sans 9");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        int text_width = 0;
        int text_height = 0;
        pango_layout_get_pixel_size(layout, &text_width, &text_height);
        g_object_unref(layout);
        cairo_destroy(measure_cr);
        cairo_surface_destroy(measure_surface);

        tooltip_popup_ = std::make_unique<TooltipPopup>();
        tooltip_popup_->owner = this;
        tooltip_popup_->text = text;
        tooltip_popup_->width = std::clamp(text_width + 18, 64, 420);
        tooltip_popup_->height = std::max(28, text_height + 12);
        tooltip_popup_->surface = wl_compositor_create_surface(compositor_);
        if (tooltip_popup_->surface == nullptr) {
            close_tooltip_popup();
            return;
        }
        wl_region* input_region = wl_compositor_create_region(compositor_);
        wl_surface_set_input_region(tooltip_popup_->surface, input_region);
        wl_region_destroy(input_region);
        tooltip_popup_->surface_role = xdg_wm_base_get_xdg_surface(
            wm_base_, tooltip_popup_->surface);
        if (tooltip_popup_->surface_role == nullptr) {
            close_tooltip_popup();
            return;
        }
        static const xdg_surface_listener surface_listener = {tooltip_surface_configure};
        xdg_surface_add_listener(tooltip_popup_->surface_role, &surface_listener,
                                 tooltip_popup_.get());

        xdg_positioner* positioner = xdg_wm_base_create_positioner(wm_base_);
        const bool vertical = edge_ == Edge::Left || edge_ == Edge::Right;
        configure_panel_positioner(positioner, vertical ? y : x, 1,
                                   tooltip_popup_->width, tooltip_popup_->height);
        tooltip_popup_->popup = xdg_surface_get_popup(tooltip_popup_->surface_role,
            popup_parent_surface(), positioner);
        xdg_positioner_destroy(positioner);
        if (tooltip_popup_->popup == nullptr) {
            close_tooltip_popup();
            return;
        }
        static const xdg_popup_listener popup_listener = {
            .configure = tooltip_popup_configure,
            .popup_done = tooltip_popup_done,
            .repositioned = popup_repositioned
        };
        xdg_popup_add_listener(tooltip_popup_->popup, &popup_listener, tooltip_popup_.get());
        attach_popup_to_panel(tooltip_popup_->popup);
        wl_surface_commit(tooltip_popup_->surface);
        wl_display_flush(display_);
    }

    void hide_tooltip()
    {
        close_tooltip_popup();
    }

    void close_tooltip_popup()
    {
        if (tooltip_popup_ == nullptr) {
            return;
        }
        auto popup = std::move(tooltip_popup_);
        if (popup->surface != nullptr) {
            wl_surface_attach(popup->surface, nullptr, 0, 0);
            wl_surface_commit(popup->surface);
        }
        if (popup->popup != nullptr) {
            xdg_popup_destroy(popup->popup);
        }
        if (popup->surface_role != nullptr) {
            xdg_surface_destroy(popup->surface_role);
        }
        if (popup->surface != nullptr) {
            wl_surface_destroy(popup->surface);
        }
        retired_tooltip_popups_.push_back(std::move(popup));
        wl_display_flush(display_);
    }

    void reap_retired_tooltip_popups()
    {
        std::erase_if(retired_tooltip_popups_, [](const auto& popup) {
            return std::none_of(popup->buffers.begin(), popup->buffers.end(),
                [](const auto& buffer) { return buffer->busy; });
        });
    }

    void render_tooltip_popup()
    {
        if (tooltip_popup_ == nullptr || !tooltip_popup_->configured) {
            return;
        }
        if (tooltip_popup_->buffers.empty() &&
            !create_buffer_set(tooltip_popup_->buffers, tooltip_popup_->width,
                              tooltip_popup_->height)) {
            tooltip_popup_->closed = true;
            return;
        }
        const std::size_t buffer_index = find_free_buffer(tooltip_popup_->buffers);
        if (buffer_index == tooltip_popup_->buffers.size()) {
            return;
        }
        Buffer& buffer = *tooltip_popup_->buffers[buffer_index];
        cairo_t* cr = buffer.cairo;
        cairo_set_source_rgb(cr, PanelTheme::background.red,
                             PanelTheme::background.green, PanelTheme::background.blue);
        cairo_paint(cr);
        draw_menu_text(cr, tooltip_popup_->text, 9.0, 6.0,
            tooltip_popup_->width - 18, "Sans 9", PanelTheme::text);
        cairo_set_source_rgb(cr, PanelTheme::border.red,
                             PanelTheme::border.green, PanelTheme::border.blue);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, 0.5, 0.5, tooltip_popup_->width - 1.0,
                        tooltip_popup_->height - 1.0);
        cairo_stroke(cr);
        cairo_surface_flush(buffer.cairo_surface);
        wl_surface_attach(tooltip_popup_->surface, buffer.buffer, 0, 0);
        wl_surface_damage(tooltip_popup_->surface, 0, 0,
                          tooltip_popup_->width, tooltip_popup_->height);
        buffer.busy = true;
        wl_surface_commit(tooltip_popup_->surface);
        wl_display_flush(display_);
    }

private:
    struct Buffer {
        wl_buffer* buffer{nullptr};
        cairo_surface_t* cairo_surface{nullptr};
        cairo_t* cairo{nullptr};
        void* data{MAP_FAILED};
        std::size_t size{0};
        bool busy{false};

        ~Buffer()
        {
            if (cairo != nullptr) cairo_destroy(cairo);
            if (cairo_surface != nullptr) cairo_surface_destroy(cairo_surface);
            if (buffer != nullptr) wl_buffer_destroy(buffer);
            if (data != MAP_FAILED) munmap(data, size);
        }
    };

    struct MenuPopup {
        WaylandBackendImpl* owner{nullptr};
        wl_surface* surface{nullptr};
        xdg_surface* surface_role{nullptr};
        xdg_popup* popup{nullptr};
        std::vector<std::unique_ptr<Buffer>> buffers;
        std::vector<std::string> categories;
        std::vector<ApplicationMenuDisplayItem> items;
        ApplicationMenuSettings settings;
        std::function<void(const std::string&)> on_category;
        std::function<void(const std::string&)> on_search;
        std::function<void(std::size_t)> on_activate;
        std::function<void(bool, std::size_t)> on_shortcut;
        std::function<void(const std::string&, const std::string&)> on_power_action;
        std::function<void()> on_close;
        std::string query;
        std::string category{"All"};
        int anchor_x{0};
        int anchor_width{0};
        int width{450};
        int height{500};
        int selected{0};
        std::size_t selected_shortcut{0};
        std::size_t scroll{0};
        std::size_t category_scroll{0};
        int scrollbar_drag_column{0};
        int scrollbar_drag_offset{0};
        int shortcut_scroll{0};
        int last_buffer_index{-1};
        int hovered_category{-1};
        int focus{0};
        bool configured{false};
        bool closed{false};
        bool power_menu{false};
        bool tab_held{false};
    };

    struct MenuReturnState {
        std::vector<std::string> categories;
        std::vector<ApplicationMenuDisplayItem> items;
        std::function<void(const std::string&)> on_category;
        std::function<void(const std::string&)> on_search;
        std::function<void(std::size_t)> on_activate;
        std::function<void(const std::string&, const std::string&)> on_power_action;
        std::function<void()> on_close;
        std::string query;
        std::string category{"All"};
        int anchor_x{0};
        int anchor_width{0};
        int width{450};
        int height{500};
        int selected{0};
        std::size_t selected_shortcut{0};
        std::size_t scroll{0};
        std::size_t category_scroll{0};
        int shortcut_scroll{0};
        int hovered_category{-1};
        int focus{0};
        bool tab_held{false};
    };

    struct CalendarPopup {
        WaylandBackendImpl* owner{nullptr};
        wl_surface* surface{nullptr};
        xdg_surface* surface_role{nullptr};
        xdg_popup* popup{nullptr};
        std::vector<std::unique_ptr<Buffer>> buffers;
        int anchor_x{0};
        int anchor_width{0};
        int width{252};
        int height{250};
        int year{0};
        int month{0};
        bool configured{false};
        bool closed{false};
    };

    struct DesktopHubPopup {
        WaylandBackendImpl* owner{nullptr};
        wl_surface* surface{nullptr};
        xdg_surface* surface_role{nullptr};
        xdg_popup* popup{nullptr};
        std::vector<std::unique_ptr<Buffer>> buffers;
        std::string query;
        std::string icon_name;
        std::vector<HubDisplayItem> items;
        std::function<void(const std::string&)> on_search;
        std::function<void(std::size_t)> on_activate;
        std::function<void()> on_close;
        int anchor_x{0};
        int anchor_width{0};
        int width{600};
        int height{450};
        std::size_t selected{0};
        std::size_t scroll{0};
        int row_height{30};
        bool configured{false};
        bool closed{false};
    };

    struct TaskMenuPopup {
        WaylandBackendImpl* owner{nullptr};
        wl_surface* surface{nullptr};
        xdg_surface* surface_role{nullptr};
        xdg_popup* popup{nullptr};
        std::vector<std::unique_ptr<Buffer>> buffers;
        std::string title;
        std::vector<std::string> items;
        std::function<void(std::size_t)> on_activate;
        int anchor_x{0};
        int width{190};
        int height{0};
        int row_height{28};
        std::size_t selected{0};
        bool configured{false};
        bool closed{false};
    };

    struct TooltipPopup {
        WaylandBackendImpl* owner{nullptr};
        wl_surface* surface{nullptr};
        xdg_surface* surface_role{nullptr};
        xdg_popup* popup{nullptr};
        std::vector<std::unique_ptr<Buffer>> buffers;
        std::string text;
        int width{0};
        int height{0};
        bool configured{false};
        bool closed{false};
    };

    enum class WorkspaceTool { Unchecked, None, Hyprland, Sway, Labwc, Kde, Gnome };
    enum class ShowDesktopTool { None, Gnome, Kde, Wlrctl, Ydotool, Wtype };

    WaylandBackend& backend_;
    wl_display* display_{nullptr};
    wl_registry* registry_{nullptr};
    wl_compositor* compositor_{nullptr};
    wl_shm* shm_{nullptr};
    wl_surface* surface_{nullptr};
    wl_seat* seat_{nullptr};
    std::vector<Output> outputs_;
    wl_pointer* pointer_{nullptr};
    wl_keyboard* keyboard_{nullptr};
    zwlr_layer_shell_v1* layer_shell_{nullptr};
    zwlr_layer_surface_v1* layer_surface_{nullptr};
    org_kde_plasma_shell* plasma_shell_{nullptr};
    org_kde_plasma_surface* plasma_surface_{nullptr};
    xdg_surface* panel_xdg_surface_{nullptr};
    xdg_toplevel* panel_toplevel_{nullptr};
    xdg_wm_base* wm_base_{nullptr};
    xkb_context* xkb_context_{nullptr};
    xkb_keymap* xkb_keymap_{nullptr};
    xkb_state* xkb_state_{nullptr};
    wl_surface* pointer_surface_{nullptr};
    wl_surface* keyboard_surface_{nullptr};
    std::unique_ptr<MenuPopup> menu_popup_;
    std::unique_ptr<MenuReturnState> power_menu_return_state_;
    std::unique_ptr<CalendarPopup> calendar_popup_;
    std::unique_ptr<DesktopHubPopup> desktop_hub_popup_;
    std::unique_ptr<TaskMenuPopup> task_menu_popup_;
    std::unique_ptr<TooltipPopup> tooltip_popup_;
    std::vector<std::unique_ptr<TooltipPopup>> retired_tooltip_popups_;
    std::vector<std::unique_ptr<Buffer>> buffers_;
    std::vector<std::vector<std::unique_ptr<Buffer>>> retired_buffers_;
    int drawing_buffer_{-1};
    int requested_width_{0};
    int requested_height_{0};
    int panel_configure_width_{0};
    int panel_configure_height_{0};
    uint32_t seat_version_{1};
    int pointer_x_{-1};
    int pointer_y_{-1};
    uint32_t pointer_serial_{0};
    bool popup_leave_pending_{false};
    bool application_menu_countdown_active_{false};
    bool task_menu_leave_pending_{false};
    bool desktop_hub_leave_pending_{false};
    bool desktop_hub_focus_leave_pending_{false};
    bool desktop_hub_countdown_active_{false};
    bool configured_{false};
    Edge edge_{Edge::Bottom};
    std::array<double, 2> axis_delta_{};
    std::array<int32_t, 2> axis_discrete_{};
    std::array<int32_t, 2> axis_value120_{};
    double smooth_scroll_{0.0};
    int32_t high_resolution_scroll_{0};
    std::unordered_map<std::string, std::string> wlr_task_ids_;
    std::unordered_map<std::string, std::pair<std::string, std::string>> wlr_task_selectors_;
    std::set<std::string> minimized_wlr_tasks_;
    std::set<std::string> maximized_wlr_tasks_;
    std::string active_wlr_task_id_;
    std::uint64_t next_wlr_task_id_{1};
    mutable WorkspaceTool workspace_tool_cache_{WorkspaceTool::Unchecked};
    mutable std::unique_ptr<WaylandTaskBackend> task_backend_;
    mutable std::chrono::steady_clock::time_point task_backend_retry_after_{};
    mutable GDBusConnection* kwin_connection_{nullptr};
    int labwc_current_workspace_id_{1};
    bool labwc_workspace_state_loaded_{false};
    bool showing_desktop_{false};
    std::vector<std::string> show_desktop_task_ids_;

    GDBusConnection* kwin_connection() const
    {
        if (kwin_connection_ == nullptr) {
            GError* error = nullptr;
            kwin_connection_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
            g_clear_error(&error);
        }
        return kwin_connection_;
    }

    GVariant* kwin_call(const char* path, const char* interface, const char* method,
                        GVariant* parameters, const GVariantType* result_type) const
    {
        GDBusConnection* connection = kwin_connection();
        if (connection == nullptr) return nullptr;
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(connection, "org.kde.KWin", path,
            interface, method, parameters, result_type, G_DBUS_CALL_FLAGS_NONE,
            1500, nullptr, &error);
        g_clear_error(&error);
        return reply;
    }

    GVariant* kwin_property(const char* path, const char* interface, const char* property) const
    {
        return kwin_call(path, "org.freedesktop.DBus.Properties", "Get",
            g_variant_new("(ss)", interface, property), G_VARIANT_TYPE("(v)"));
    }

    bool kwin_show_desktop_state(bool& showing) const
    {
        GVariant* reply = kwin_property("/KWin", "org.kde.KWin", "showingDesktop");
        if (reply == nullptr) return false;
        GVariant* value = nullptr;
        g_variant_get(reply, "(v)", &value);
        const bool valid = g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN);
        if (valid) showing = g_variant_get_boolean(value);
        g_variant_unref(value);
        g_variant_unref(reply);
        return valid;
    }

    bool kwin_desktop_manager_available() const
    {
        GVariant* reply = kwin_property("/VirtualDesktopManager",
            "org.kde.KWin.VirtualDesktopManager", "desktops");
        if (reply == nullptr) return false;
        GVariant* value = nullptr;
        g_variant_get(reply, "(v)", &value);
        const bool valid = g_variant_is_of_type(value, G_VARIANT_TYPE("a(iss)")) ||
            g_variant_is_of_type(value, G_VARIANT_TYPE("a(uss)"));
        g_variant_unref(value);
        g_variant_unref(reply);
        return valid;
    }

    bool kwin_read_workspaces(WorkspaceSnapshot& snapshot) const
    {
        GVariant* reply = kwin_property("/VirtualDesktopManager",
            "org.kde.KWin.VirtualDesktopManager", "desktops");
        if (reply == nullptr) return false;
        GVariant* desktops = nullptr;
        g_variant_get(reply, "(v)", &desktops);
        g_variant_unref(reply);
        const bool signed_positions = g_variant_is_of_type(
            desktops, G_VARIANT_TYPE("a(iss)"));
        const bool unsigned_positions = g_variant_is_of_type(
            desktops, G_VARIANT_TYPE("a(uss)"));
        if (!signed_positions && !unsigned_positions) {
            g_variant_unref(desktops);
            return false;
        }

        const gsize desktop_count = g_variant_n_children(desktops);
        snapshot.ids.reserve(desktop_count);
        for (gsize index = 0; index < desktop_count; ++index) {
            GVariant* desktop = g_variant_get_child_value(desktops, index);
            gint64 position = 0;
            const gchar* id = nullptr;
            const gchar* name = nullptr;
            if (signed_positions) {
                gint32 signed_position = 0;
                g_variant_get(desktop, "(i&s&s)", &signed_position, &id, &name);
                position = signed_position;
            } else {
                guint32 unsigned_position = 0;
                g_variant_get(desktop, "(u&s&s)", &unsigned_position, &id, &name);
                position = unsigned_position;
            }
            (void)id;
            (void)name;
            if (position >= 0 && position < std::numeric_limits<int>::max()) {
                snapshot.ids.push_back(static_cast<int>(position + 1));
            }
            g_variant_unref(desktop);
        }
        g_variant_unref(desktops);

        GVariant* current = kwin_call("/KWin", "org.kde.KWin", "currentDesktop",
            nullptr, G_VARIANT_TYPE("(i)"));
        if (current != nullptr) {
            g_variant_get(current, "(i)", &snapshot.current_id);
            g_variant_unref(current);
        }
        return !snapshot.ids.empty();
    }

    bool kwin_set_show_desktop(bool showing) const
    {
        GDBusConnection* connection = kwin_connection();
        if (connection == nullptr) return false;
        GDBusMessage* message = g_dbus_message_new_method_call("org.kde.KWin", "/KWin",
            "org.kde.KWin", "showDesktop");
        if (message == nullptr) return false;
        g_dbus_message_set_flags(message, G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED);
        g_dbus_message_set_body(message, g_variant_new("(b)", showing));
        GError* error = nullptr;
        const bool sent = g_dbus_connection_send_message(connection, message,
            G_DBUS_SEND_MESSAGE_FLAGS_NONE, nullptr, &error);
        g_clear_error(&error);
        g_object_unref(message);
        return sent;
    }

    std::vector<TaskWindowInfo> read_wlr_task_windows()
    {
        const auto result = SystemCommands::run({"wlrctl", "toplevel", "list"});
        if (result.exit_code != 0) {
            return {};
        }
        std::vector<TaskWindowInfo> tasks;
        std::map<std::string, unsigned> occurrences;
        std::set<std::string> live_keys;
        std::unordered_map<std::string, std::pair<std::string, std::string>> selectors;
        std::istringstream lines(result.output);
        std::string raw;
        while (std::getline(lines, raw)) {
            const std::size_t first = raw.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                continue;
            }
            const std::size_t last = raw.find_last_not_of(" \t\r\n");
            const std::string line = raw.substr(first, last - first + 1);
            const std::size_t separator = line.find(':');
            std::string app_id = separator == std::string::npos
                ? std::string{} : line.substr(0, separator);
            std::string title = separator == std::string::npos
                ? line : line.substr(separator + 1);
            const std::size_t app_end = app_id.find_last_not_of(" \t");
            if (app_end != std::string::npos) {
                app_id.erase(app_end + 1);
            }
            const std::size_t title_first = title.find_first_not_of(" \t");
            const std::size_t title_last = title.find_last_not_of(" \t");
            title = title_first == std::string::npos
                ? std::string{} : title.substr(title_first, title_last - title_first + 1);
            if (title.empty()) {
                title = app_id.empty() ? "Window" : app_id;
            }
            if (task_is_excluded(app_id, title)) {
                continue;
            }

            std::string key = app_id;
            key.push_back('\0');
            key += title;
            key.push_back('\0');
            key += std::to_string(occurrences[key]++);
            live_keys.insert(key);
            auto id = wlr_task_ids_.find(key);
            if (id == wlr_task_ids_.end()) {
                id = wlr_task_ids_.emplace(key,
                    "wlr-" + std::to_string(next_wlr_task_id_++)).first;
            }
            selectors.emplace(id->second, std::make_pair(app_id, title));
            TaskWindowInfo task;
            task.id = id->second;
            task.title = title;
            task.icon_name = resolve_application_icon(app_id);
            if (task.icon_name.empty()) {
                task.icon_name = "application-x-executable";
            }
            task.active = task.id == active_wlr_task_id_;
            task.iconified = minimized_wlr_tasks_.count(task.id) != 0;
            task.maximized = maximized_wlr_tasks_.count(task.id) != 0;
            tasks.push_back(std::move(task));
        }

        for (auto item = wlr_task_ids_.begin(); item != wlr_task_ids_.end();) {
            if (live_keys.count(item->first) == 0) {
                minimized_wlr_tasks_.erase(item->second);
                maximized_wlr_tasks_.erase(item->second);
                if (active_wlr_task_id_ == item->second) {
                    active_wlr_task_id_.clear();
                }
                item = wlr_task_ids_.erase(item);
            } else {
                ++item;
            }
        }
        wlr_task_selectors_ = std::move(selectors);
        return tasks;
    }

    void initialize_task_backend() const
    {
        if (task_backend_ != nullptr) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < task_backend_retry_after_) return;
        task_backend_retry_after_ = now + std::chrono::seconds(1);
        const char* desktop_value = std::getenv("XDG_CURRENT_DESKTOP");
        std::string desktop = desktop_value != nullptr ? desktop_value : "";
        std::transform(desktop.begin(), desktop.end(), desktop.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        const char* compositor = std::getenv("MDEON_SESSION_COMPOSITOR");
        task_backend_ = (desktop.find("gnome") != std::string::npos ||
                         (compositor != nullptr && std::string(compositor) == "gnome-shell"))
            ? WaylandTaskBackend::create_gnome_bridge() : WaylandTaskBackend::create();
    }

    bool run_wlr_task_action(const std::string& id, TaskAction action)
    {
        const auto found = wlr_task_selectors_.find(id);
        if (found == wlr_task_selectors_.end()) {
            return false;
        }
        if (action == TaskAction::ToggleMinimize) {
            action = id == active_wlr_task_id_ ? TaskAction::Minimize : TaskAction::Activate;
        }
        std::vector<std::string> verbs;
        if (action == TaskAction::Activate) {
            verbs = {"focus", "activate"};
        } else if (action == TaskAction::Minimize) {
            verbs = {"minimize", "set-minimized"};
        } else if (action == TaskAction::ToggleMaximize) {
            verbs = maximized_wlr_tasks_.count(id) != 0
                ? std::vector<std::string>{"unmaximize", "set-unmaximized"}
                : std::vector<std::string>{"maximize", "set-maximized"};
        } else if (action == TaskAction::Close) {
            verbs = {"close", "kill"};
        }

        for (const std::string& verb : verbs) {
            std::vector<std::string> arguments{"wlrctl", "toplevel", verb};
            if (!found->second.first.empty()) {
                arguments.push_back("app_id:" + found->second.first);
            }
            if (!found->second.second.empty()) {
                arguments.push_back("title:" + found->second.second);
            }
            if (SystemCommands::run(arguments).exit_code != 0) {
                continue;
            }
            if (action == TaskAction::Activate) {
                active_wlr_task_id_ = id;
                minimized_wlr_tasks_.erase(id);
            } else if (action == TaskAction::Minimize) {
                minimized_wlr_tasks_.insert(id);
                if (active_wlr_task_id_ == id) {
                    active_wlr_task_id_.clear();
                }
            } else if (action == TaskAction::ToggleMaximize) {
                if (maximized_wlr_tasks_.count(id) != 0) {
                    maximized_wlr_tasks_.erase(id);
                } else {
                    maximized_wlr_tasks_.insert(id);
                }
            }
            return true;
        }
        return false;
    }

    ShowDesktopTool show_desktop_tool() const
    {
        const char* desktop_value = std::getenv("XDG_CURRENT_DESKTOP");
        std::string desktop = desktop_value != nullptr ? desktop_value : "";
        std::transform(desktop.begin(), desktop.end(), desktop.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        const char* compositor = std::getenv("MDEON_SESSION_COMPOSITOR");
        if (desktop.find("gnome") != std::string::npos ||
            (compositor != nullptr && std::string(compositor) == "gnome-shell")) {
            initialize_task_backend();
            return ShowDesktopTool::Gnome;
        }
        bool showing = false;
        if (kwin_show_desktop_state(showing)) return ShowDesktopTool::Kde;
        if (!SystemCommands::find_executable("ydotool").empty()) {
            return ShowDesktopTool::Ydotool;
        }
        if (!SystemCommands::find_executable("wtype").empty()) {
            return ShowDesktopTool::Wtype;
        }
        if (supports_task_list()) {
            return ShowDesktopTool::Wlrctl;
        }
        return ShowDesktopTool::None;
    }

    WorkspaceTool workspace_tool() const
    {
        if (workspace_tool_cache_ != WorkspaceTool::Unchecked) {
            return workspace_tool_cache_;
        }

        const char* hyprland_signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
        const char* sway_socket = std::getenv("SWAYSOCK");
        const char* desktop_value = std::getenv("XDG_CURRENT_DESKTOP");
        std::string desktop = desktop_value != nullptr ? desktop_value : "";
        std::transform(desktop.begin(), desktop.end(), desktop.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        const char* compositor = std::getenv("MDEON_SESSION_COMPOSITOR");
        const bool gnome_session = desktop.find("gnome") != std::string::npos ||
            (compositor != nullptr && std::string(compositor) == "gnome-shell");
        const bool hyprland_session = hyprland_signature != nullptr ||
            desktop.find("hyprland") != std::string::npos;
        const bool sway_session = sway_socket != nullptr ||
            desktop.find("sway") != std::string::npos;
        if (gnome_session) {
            initialize_task_backend();
            workspace_tool_cache_ = WorkspaceTool::Gnome;
        }
        if (workspace_tool_cache_ == WorkspaceTool::Unchecked) {
              if (kwin_desktop_manager_available()) {
                workspace_tool_cache_ = WorkspaceTool::Kde;
            } else if (hyprland_session &&
                       !SystemCommands::find_executable("hyprctl").empty() &&
                       SystemCommands::run({"hyprctl", "activeworkspace", "-j"}).exit_code == 0) {
                workspace_tool_cache_ = WorkspaceTool::Hyprland;
            } else if (sway_session &&
                       !SystemCommands::find_executable("swaymsg").empty() &&
                       SystemCommands::run({"swaymsg", "-t", "get_workspaces"}).exit_code == 0) {
                workspace_tool_cache_ = WorkspaceTool::Sway;
            } else if (!SystemCommands::find_executable("labwc").empty() &&
                       !SystemCommands::find_executable("wtype").empty() &&
                       labwc_workspace_count() > 0) {
                workspace_tool_cache_ = WorkspaceTool::Labwc;
            } else {
                workspace_tool_cache_ = WorkspaceTool::None;
            }
        }
        return workspace_tool_cache_;
    }

    std::string labwc_config_path() const
    {
        const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME");
        const char* home = std::getenv("HOME");
        const std::filesystem::path config_root = xdg_config_home != nullptr &&
            *xdg_config_home != '\0' ? xdg_config_home :
            std::filesystem::path(home != nullptr ? home : ".") / ".config";
        const char* desktop_value = std::getenv("XDG_CURRENT_DESKTOP");
        std::string desktop = desktop_value != nullptr ? desktop_value : "";
        std::transform(desktop.begin(), desktop.end(), desktop.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        std::vector<std::filesystem::path> candidates;
        if (desktop.find("xfce") != std::string::npos) {
            candidates.push_back(config_root / "xfce4/labwc/rc.xml");
        }
        candidates.push_back(config_root / "labwc/rc.xml");
        for (const auto& candidate : candidates) {
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error)) {
                return candidate.string();
            }
        }
        return {};
    }

    int labwc_workspace_count() const
    {
        const std::string path = labwc_config_path();
        if (path.empty()) {
            return 0;
        }
        std::ifstream input(path);
        const std::string xml((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
        std::smatch match;
        const std::regex attribute(R"(<desktops\b[^>]*\bnumber\s*=\s*["']?([0-9]+))");
        if (std::regex_search(xml, match, attribute)) {
            return std::max(1, std::stoi(match[1].str()));
        }
        const std::regex element(R"(<desktops\b[^>]*>\s*<number>\s*([0-9]+)\s*</number>)");
        return std::regex_search(xml, match, element)
            ? std::max(1, std::stoi(match[1].str())) : 0;
    }

    std::filesystem::path labwc_workspace_state_path() const
    {
        const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
        return runtime_dir != nullptr && *runtime_dir != '\0'
            ? std::filesystem::path(runtime_dir) / "simpanel/labwc-workspace"
            : std::filesystem::path{};
    }

    static int json_integer(const std::string& json, const char* key)
    {
        const std::regex field(std::string("\"") + key + R"(\"\s*:\s*(-?[0-9]+))");
        std::smatch match;
        return std::regex_search(json, match, field) ? std::stoi(match[1].str()) : -1;
    }

    WorkspaceSnapshot read_workspaces()
    {
        WorkspaceSnapshot snapshot;
        const WorkspaceTool tool = workspace_tool();
        if (tool == WorkspaceTool::Gnome) {
            initialize_task_backend();
            if (task_backend_ != nullptr) snapshot = task_backend_->get_workspaces();
        } else if (tool == WorkspaceTool::Kde) {
            kwin_read_workspaces(snapshot);
        } else if (tool == WorkspaceTool::Hyprland) {
            const auto listing = SystemCommands::run({"hyprctl", "workspaces", "-j"});
            const auto active = SystemCommands::run({"hyprctl", "activeworkspace", "-j"});
            if (listing.exit_code != 0 || active.exit_code != 0) {
                return snapshot;
            }
            const std::regex id_field(R"(\"id\"\s*:\s*(-?[0-9]+))");
            for (std::sregex_iterator item(listing.output.begin(), listing.output.end(), id_field), end;
                 item != end; ++item) {
                const int id = std::stoi((*item)[1].str());
                if (id > 0) {
                    snapshot.ids.push_back(id);
                }
            }
            snapshot.current_id = json_integer(active.output, "id");
        } else if (tool == WorkspaceTool::Sway) {
            const auto result = SystemCommands::run({"swaymsg", "-t", "get_workspaces"});
            if (result.exit_code != 0) {
                return snapshot;
            }
            const std::regex number_field(R"(\"num\"\s*:\s*(-?[0-9]+))");
            const std::regex focused_field(R"(\"focused\"\s*:\s*true)");
            bool in_string = false;
            bool escaped = false;
            int depth = 0;
            std::size_t object_start = 0;
            for (std::size_t index = 0; index < result.output.size(); ++index) {
                const char character = result.output[index];
                if (in_string) {
                    if (escaped) escaped = false;
                    else if (character == '\\') escaped = true;
                    else if (character == '"') in_string = false;
                    continue;
                }
                if (character == '"') {
                    in_string = true;
                } else if (character == '{') {
                    if (depth++ == 0) object_start = index;
                } else if (character == '}' && depth > 0 && --depth == 0) {
                    const std::string object = result.output.substr(
                        object_start, index - object_start + 1);
                    std::smatch number;
                    if (std::regex_search(object, number, number_field)) {
                        const int id = std::stoi(number[1].str());
                        snapshot.ids.push_back(id);
                        if (std::regex_search(object, focused_field)) {
                            snapshot.current_id = id;
                        }
                    }
                }
            }
        } else if (tool == WorkspaceTool::Labwc) {
            const int count = labwc_workspace_count();
            if (!labwc_workspace_state_loaded_) {
                labwc_workspace_state_loaded_ = true;
                const auto state_path = labwc_workspace_state_path();
                std::ifstream state_file(state_path);
                int saved_workspace = 0;
                if (state_file >> saved_workspace && saved_workspace > 0 &&
                    saved_workspace <= count) {
                    labwc_current_workspace_id_ = saved_workspace;
                }
            }
            snapshot.ids.reserve(static_cast<std::size_t>(count));
            for (int id = 1; id <= count; ++id) {
                snapshot.ids.push_back(id);
            }
            snapshot.current_id = std::clamp(labwc_current_workspace_id_, 1,
                                              std::max(1, count));
        }
        std::sort(snapshot.ids.begin(), snapshot.ids.end());
        snapshot.ids.erase(std::unique(snapshot.ids.begin(), snapshot.ids.end()),
                           snapshot.ids.end());
        return snapshot;
    }

    bool activate_workspace(int id)
    {
        const WorkspaceTool tool = workspace_tool();
        if (tool == WorkspaceTool::Gnome) {
            initialize_task_backend();
            return task_backend_ != nullptr && task_backend_->switch_workspace(id);
        }
        if (tool == WorkspaceTool::Kde && id > 0) {
            GVariant* reply = kwin_call("/KWin", "org.kde.KWin", "setCurrentDesktop",
                g_variant_new("(i)", id), G_VARIANT_TYPE("(b)"));
            if (reply == nullptr) return false;
            gboolean changed = FALSE;
            g_variant_get(reply, "(b)", &changed);
            g_variant_unref(reply);
            return changed;
        }
        if (tool == WorkspaceTool::Hyprland) {
            return SystemCommands::run({"hyprctl", "dispatch", "workspace",
                                       std::to_string(id)}).exit_code == 0;
        }
        if (tool == WorkspaceTool::Sway) {
            return SystemCommands::run({"swaymsg", "workspace", "number",
                                       std::to_string(id)}).exit_code == 0;
        }
        if (tool == WorkspaceTool::Labwc && id >= 1 && id <= labwc_workspace_count()) {
            const bool switched = SystemCommands::run({"wtype", "-M", "win", "-k",
                std::to_string(id), "-m", "win"}).exit_code == 0;
            if (switched) {
                labwc_current_workspace_id_ = id;
                const auto state_path = labwc_workspace_state_path();
                if (!state_path.empty()) {
                    std::error_code error;
                    std::filesystem::create_directories(state_path.parent_path(), error);
                    if (!error) {
                        std::ofstream state_file(state_path);
                        if (state_file) state_file << id;
                    }
                }
            }
            return switched;
        }
        return false;
    }

    bool ensure_workspace_count(int count)
    {
        const WorkspaceSnapshot current = read_workspaces();
        if (current.ids.empty() || count < 1) {
            return false;
        }
        if (workspace_tool() == WorkspaceTool::Labwc) {
            return true;
        }
        if (workspace_tool() == WorkspaceTool::Gnome) {
            return true;
        }
        if (workspace_tool() == WorkspaceTool::Kde) {
            int existing_count = static_cast<int>(current.ids.size());
            while (existing_count < count) {
                GVariant* reply = kwin_call("/VirtualDesktopManager",
                    "org.kde.KWin.VirtualDesktopManager", "createDesktop",
                    g_variant_new("(us)", static_cast<guint32>(existing_count), ""),
                    G_VARIANT_TYPE("()"));
                if (reply == nullptr) return false;
                g_variant_unref(reply);
                ++existing_count;
            }
            return true;
        }
        for (int id = 1; id <= count; ++id) {
            if (std::find(current.ids.begin(), current.ids.end(), id) == current.ids.end()) {
                activate_workspace(id);
            }
        }
        if (current.current_id > 0) {
            activate_workspace(current.current_id);
        }
        return true;
    }

    static void registry_global(void* data, wl_registry* registry, uint32_t name,
                                const char* interface, uint32_t version)
    {
        auto* self = static_cast<WaylandBackendImpl*>(data);
        if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
            self->compositor_ = static_cast<wl_compositor*>(wl_registry_bind(
                registry, name, &wl_compositor_interface, std::min(version, 4u)));
        } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
            self->shm_ = static_cast<wl_shm*>(wl_registry_bind(
                registry, name, &wl_shm_interface, 1));
        } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
            self->seat_version_ = std::min(version, 8u);
            self->seat_ = static_cast<wl_seat*>(wl_registry_bind(
                registry, name, &wl_seat_interface, self->seat_version_));
            static const wl_seat_listener listener = {seat_capabilities, seat_name};
            wl_seat_add_listener(self->seat_, &listener, self);
        } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
            wl_output* output = static_cast<wl_output*>(wl_registry_bind(
                registry, name, &wl_output_interface, std::min(version, 4u)));
            self->outputs_.push_back({name, output, "output-" + std::to_string(name)});
            static const wl_output_listener listener = {
                output_geometry, output_mode, output_done, output_scale,
                output_name, output_description
            };
            wl_output_add_listener(output, &listener, self);
        } else if (std::strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
            self->layer_shell_ = static_cast<zwlr_layer_shell_v1*>(wl_registry_bind(
                registry, name, &zwlr_layer_shell_v1_interface, std::min(version, 4u)));
        } else if (std::strcmp(interface, org_kde_plasma_shell_interface.name) == 0) {
            self->plasma_shell_ = static_cast<org_kde_plasma_shell*>(wl_registry_bind(
                registry, name, &org_kde_plasma_shell_interface, std::min(version, 1u)));
        } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
            self->wm_base_ = static_cast<xdg_wm_base*>(wl_registry_bind(
                registry, name, &xdg_wm_base_interface, std::min(version, 5u)));
            static const xdg_wm_base_listener listener = {wm_base_ping};
            xdg_wm_base_add_listener(self->wm_base_, &listener, self);
        }
    }

    static void output_geometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t,
                                int32_t, const char*, const char*, int32_t) {}

    static void output_mode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}

    static void output_done(void*, wl_output*) {}

    static void output_scale(void*, wl_output*, int32_t) {}

    static void output_name(void* data, wl_output* proxy, const char* name)
    {
        auto* self = static_cast<WaylandBackendImpl*>(data);
        const auto found = std::find_if(self->outputs_.begin(), self->outputs_.end(),
            [proxy](const Output& output) { return output.proxy == proxy; });
        if (found != self->outputs_.end()) {
            found->name = name;
        }
    }

    static void output_description(void*, wl_output*, const char*) {}

    static void wm_base_ping(void* data, xdg_wm_base* wm_base, uint32_t serial)
    {
        xdg_wm_base_pong(wm_base, serial);
        wl_display_flush(static_cast<WaylandBackendImpl*>(data)->display_);
    }

    static void registry_remove(void*, wl_registry*, uint32_t) {}

    static void seat_capabilities(void* data, wl_seat* seat, uint32_t capabilities)
    {
        auto* self = static_cast<WaylandBackendImpl*>(data);
        if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && self->pointer_ == nullptr) {
            self->pointer_ = wl_seat_get_pointer(seat);
                static const wl_pointer_listener listener = {
                    .enter = pointer_enter,
                    .leave = pointer_leave,
                    .motion = pointer_motion,
                    .button = pointer_button,
                    .axis = pointer_axis,
                    .frame = pointer_frame,
                    .axis_source = pointer_axis_source,
                    .axis_stop = pointer_axis_stop,
                    .axis_discrete = pointer_axis_discrete,
                    .axis_value120 = pointer_axis_value120,
                    .axis_relative_direction = nullptr,
                    .warp = nullptr
                };
            wl_pointer_add_listener(self->pointer_, &listener, self);
        } else if ((capabilities & WL_SEAT_CAPABILITY_POINTER) == 0 &&
                   self->pointer_ != nullptr) {
            wl_pointer_destroy(self->pointer_);
            self->pointer_ = nullptr;
        }
        if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && self->keyboard_ == nullptr) {
            self->keyboard_ = wl_seat_get_keyboard(seat);
            static const wl_keyboard_listener listener = {
                .keymap = keyboard_keymap,
                .enter = keyboard_enter,
                .leave = keyboard_leave,
                .key = keyboard_key,
                .modifiers = keyboard_modifiers,
                .repeat_info = keyboard_repeat_info
            };
            wl_keyboard_add_listener(self->keyboard_, &listener, self);
        } else if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) == 0 &&
                   self->keyboard_ != nullptr) {
            wl_keyboard_destroy(self->keyboard_);
            self->keyboard_ = nullptr;
        }
    }

    static void seat_name(void*, wl_seat*, const char*) {}

    static void keyboard_keymap(void* data, wl_keyboard*, uint32_t format, int fd,
                                uint32_t size)
    {
        auto* self = static_cast<WaylandBackendImpl*>(data);
        if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
            close(fd);
            return;
        }
        void* mapping = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (mapping == MAP_FAILED) return;
        const std::string keymap_text(static_cast<const char*>(mapping), size);
        xkb_keymap* keymap = xkb_keymap_new_from_string(
            self->xkb_context_, keymap_text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1,
            XKB_KEYMAP_COMPILE_NO_FLAGS);
        munmap(mapping, size);
        if (keymap == nullptr) return;
        if (self->xkb_state_ != nullptr) xkb_state_unref(self->xkb_state_);
        if (self->xkb_keymap_ != nullptr) xkb_keymap_unref(self->xkb_keymap_);
        self->xkb_keymap_ = keymap;
        self->xkb_state_ = xkb_state_new(keymap);
    }

    static void keyboard_enter(void* data, wl_keyboard*, uint32_t, wl_surface* surface,
                               wl_array*)
    {
        auto* self = static_cast<WaylandBackendImpl*>(data);
        self->keyboard_surface_ = surface;
        self->desktop_hub_focus_leave_pending_ = false;
    }

    static void keyboard_leave(void* data, wl_keyboard*, uint32_t, wl_surface* surface)
    {
        auto* self = static_cast<WaylandBackendImpl*>(data);
        if (self->keyboard_surface_ == surface) self->keyboard_surface_ = nullptr;
        if (self->desktop_hub_popup_ != nullptr &&
            (surface == self->desktop_hub_popup_->surface || surface == self->surface_)) {
            self->desktop_hub_focus_leave_pending_ = true;
        }
    }

    static void keyboard_key(void* data, wl_keyboard*, uint32_t, uint32_t,
                             uint32_t key, uint32_t state)
    {
        auto* self = static_cast<WaylandBackendImpl*>(data);
        if (self->xkb_state_ == nullptr) {
            return;
        }
        const xkb_keycode_t keycode = key + 8;
        const xkb_keysym_t symbol = xkb_state_key_get_one_sym(self->xkb_state_, keycode);
        if (self->menu_popup_ != nullptr &&
            (self->keyboard_surface_ == self->menu_popup_->surface ||
             self->keyboard_surface_ == self->surface_) && symbol == XKB_KEY_Tab) {
            self->menu_popup_->tab_held = state == WL_KEYBOARD_KEY_STATE_PRESSED;
            return;
        }
        if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
        if (self->task_menu_popup_ != nullptr &&
            (self->keyboard_surface_ == self->task_menu_popup_->surface ||
             self->keyboard_surface_ == self->surface_)) {
            self->handle_task_menu_key(symbol);
            return;
        }
        if (self->desktop_hub_popup_ != nullptr &&
            (self->keyboard_surface_ == self->desktop_hub_popup_->surface ||
             self->keyboard_surface_ == self->surface_)) {
            self->handle_desktop_hub_key(symbol, keycode);
            return;
        }
        if (self->calendar_popup_ != nullptr &&
            self->keyboard_surface_ == self->calendar_popup_->surface) {
            self->handle_calendar_key(symbol);
            return;
        }
        if (self->menu_popup_ == nullptr ||
            (self->keyboard_surface_ != self->menu_popup_->surface &&
             self->keyboard_surface_ != self->surface_)) {
            return;
        }
        if (symbol == XKB_KEY_Escape) {
            if (self->menu_popup_->power_menu &&
                self->power_menu_return_state_ != nullptr) {
                self->close_power_menu();
            } else {
                self->close_menu_popup(true);
            }
        } else if (self->menu_popup_->power_menu &&
                   (symbol == XKB_KEY_Left || symbol == XKB_KEY_Right)) {
            self->close_power_menu();
            if (self->menu_popup_ != nullptr) {
                if (symbol == XKB_KEY_Left) {
                    self->menu_popup_->focus = self->menu_popup_->settings.show_categories ? 1 :
                        self->menu_popup_->settings.show_places ? 4 : 2;
                } else {
                    self->menu_popup_->focus = 2;
                }
                self->render_menu_popup();
            }
        } else if (symbol == XKB_KEY_Up || symbol == XKB_KEY_Down) {
            const int direction = symbol == XKB_KEY_Up ? -1 : 1;
            if (self->menu_popup_->focus == 3 && direction < 0 &&
                self->menu_popup_->settings.show_categories &&
                !self->menu_popup_->categories.empty()) {
                self->menu_popup_->focus = 1;
                self->menu_popup_->category = self->menu_popup_->categories.back();
                if (self->menu_popup_->on_category) {
                    self->menu_popup_->on_category(self->menu_popup_->category);
                }
            } else if (self->menu_popup_->focus == 3 && direction < 0 &&
                       !self->menu_popup_->settings.show_categories &&
                       !self->menu_popup_->items.empty()) {
                self->menu_popup_->focus = 2;
                self->menu_popup_->selected =
                    static_cast<int>(self->menu_popup_->items.size()) - 1;
                self->move_popup_selection(0);
            } else if (self->menu_popup_->focus == 3 && direction > 0) {
                self->menu_popup_->focus = 2;
                self->move_popup_selection(direction);
            } else if (self->menu_popup_->focus == 1 &&
                       self->menu_popup_->settings.show_categories) {
                const std::size_t count = self->menu_popup_->settings.places.size() +
                    self->menu_popup_->settings.system.size();
                if (count > 0) {
                    if (direction < 0 && self->menu_popup_->selected_shortcut > 0) {
                        --self->menu_popup_->selected_shortcut;
                    } else if (direction > 0 &&
                               self->menu_popup_->selected_shortcut + 1 < count) {
                        ++self->menu_popup_->selected_shortcut;
                    }
                    self->update_popup_shortcut_scroll();
                    self->render_menu_popup();
                }
            } else if (self->menu_popup_->focus == 1) {
                self->move_popup_category(direction);
            } else {
                self->move_popup_selection(direction);
            }
        } else if (symbol == XKB_KEY_Left && !self->menu_popup_->power_menu) {
            if (self->menu_popup_->tab_held && self->menu_popup_->settings.show_places &&
                (!self->menu_popup_->settings.places.empty() ||
                 !self->menu_popup_->settings.system.empty())) {
                self->menu_popup_->focus = 4;
                self->menu_popup_->selected_shortcut = 0;
                self->menu_popup_->shortcut_scroll = 0;
            } else if (self->menu_popup_->settings.show_categories) {
                self->menu_popup_->focus = 1;
            } else if (self->menu_popup_->settings.show_places &&
                       (!self->menu_popup_->settings.places.empty() ||
                        !self->menu_popup_->settings.system.empty())) {
                self->menu_popup_->focus = 4;
                self->menu_popup_->selected_shortcut = 0;
                self->menu_popup_->shortcut_scroll = 0;
            } else {
                self->menu_popup_->focus = 2;
            }
            self->render_menu_popup();
        } else if (symbol == XKB_KEY_Right && !self->menu_popup_->power_menu) {
            self->menu_popup_->focus = 2;
            self->render_menu_popup();
        } else if ((symbol == XKB_KEY_Tab || symbol == XKB_KEY_ISO_Left_Tab) &&
                   self->menu_popup_->power_menu) {
            const bool reverse = symbol == XKB_KEY_ISO_Left_Tab ||
                xkb_state_mod_name_is_active(self->xkb_state_, XKB_MOD_NAME_SHIFT,
                                             XKB_STATE_MODS_EFFECTIVE);
            self->move_popup_selection(reverse ? -1 : 1);
        } else if (symbol == XKB_KEY_Return || symbol == XKB_KEY_KP_Enter ||
                   (symbol == XKB_KEY_space && self->menu_popup_->focus != 0)) {
            if (self->menu_popup_->focus == 3 && !self->menu_popup_->power_menu) {
                self->toggle_application_power_menu(self->menu_popup_->anchor_x,
                    self->menu_popup_->anchor_width,
                    self->menu_popup_->on_power_action);
            } else if (self->menu_popup_->focus == 1 &&
                       self->menu_popup_->settings.show_categories) {
                self->menu_popup_->focus = 2;
                self->render_menu_popup();
            } else if (self->menu_popup_->focus == 4) {
                const std::size_t places_count = self->menu_popup_->settings.places.size();
                if (self->menu_popup_->selected_shortcut < places_count) {
                    if (self->menu_popup_->on_shortcut) {
                        self->menu_popup_->on_shortcut(false,
                            self->menu_popup_->selected_shortcut);
                    }
                } else if (self->menu_popup_->on_shortcut) {
                    self->menu_popup_->on_shortcut(true,
                        self->menu_popup_->selected_shortcut - places_count);
                }
            } else {
                self->activate_popup_selection();
            }
        } else if (!self->menu_popup_->power_menu && symbol == XKB_KEY_BackSpace) {
            std::string& query = self->menu_popup_->query;
            if (!query.empty()) {
                do {
                    const unsigned char character = static_cast<unsigned char>(query.back());
                    query.pop_back();
                    if ((character & 0xc0) != 0x80) break;
                } while (!query.empty());
                if (self->menu_popup_->on_search) {
                    self->menu_popup_->on_search(query);
                }
                self->menu_popup_->focus = 0;
                self->render_menu_popup();
            }
        } else if (!self->menu_popup_->power_menu &&
                   !xkb_state_mod_name_is_active(self->xkb_state_, XKB_MOD_NAME_CTRL,
                                                 XKB_STATE_MODS_EFFECTIVE) &&
                   !xkb_state_mod_name_is_active(self->xkb_state_, XKB_MOD_NAME_ALT,
                                                 XKB_STATE_MODS_EFFECTIVE)) {
            char text[64]{};
            const int length = xkb_state_key_get_utf8(self->xkb_state_, keycode,
                                                       text, sizeof(text));
            if (length > 0 && text[0] >= 0x20) {
                self->menu_popup_->query.append(text, static_cast<std::size_t>(length));
                self->menu_popup_->focus = 0;
                if (self->menu_popup_->on_search) {
                    self->menu_popup_->on_search(self->menu_popup_->query);
                }
                self->render_menu_popup();
            }
        }
    }

    static void keyboard_modifiers(void* data, wl_keyboard*, uint32_t,
                                   uint32_t depressed, uint32_t latched,
                                   uint32_t locked, uint32_t group)
    {
        auto* self = static_cast<WaylandBackendImpl*>(data);
        if (self->xkb_state_ != nullptr) {
            xkb_state_update_mask(self->xkb_state_, depressed, latched, locked,
                                  0, 0, group);
        }
    }

    static void keyboard_repeat_info(void*, wl_keyboard*, int32_t, int32_t)
    {
    }

    void update_popup_hover(int x, int y)
    {
        if (menu_popup_ == nullptr) return;
        if (menu_popup_->scrollbar_drag_column != 0) {
            update_menu_scrollbar_drag(y);
            return;
        }
        const int visible_category_width = menu_popup_->settings.show_categories
            ? menu_popup_->settings.categories_width : 0;
        const int places_width = menu_popup_->settings.show_places
            ? menu_popup_->settings.places_width : 0;
        const int category_x = places_width;
        const int applications_x = category_x + visible_category_width;
        if (menu_popup_->power_menu) {
            if (power_menu_return_state_ != nullptr) {
                constexpr int power_height = 176;
                const int places_width = menu_popup_->settings.show_places
                    ? menu_popup_->settings.places_width : 0;
                const int visible_category_width = menu_popup_->settings.show_categories
                    ? menu_popup_->settings.categories_width : 0;
                const int category_x = places_width;
                const int applications_x = category_x + visible_category_width;
                const int power_x = menu_popup_->settings.show_categories
                    ? category_x + 5 : applications_x + 5;
                constexpr int power_width = 190;
                const int power_y = menu_popup_->height - 47 - power_height;
                if (x < power_x || x >= power_x + power_width ||
                    y < power_y || y >= power_y + power_height) {
                    MenuReturnState& parent = *power_menu_return_state_;
                    const int footer_height = menu_popup_->settings.show_categories ? 42 : 78;
                    const int footer_y = menu_popup_->height - footer_height;
                    if (y >= footer_y) {
                        if (menu_popup_->settings.show_categories &&
                            x >= category_x && x < applications_x) parent.focus = 3;
                        else if (!menu_popup_->settings.show_categories &&
                                 x >= applications_x && y < footer_y + 34) parent.focus = 3;
                        else if (x >= applications_x) parent.focus = 0;
                        render_menu_popup();
                        return;
                    }
                    if (places_width > 0 && x < places_width) {
                        const int content_y = y + parent.shortcut_scroll;
                        const int places_count = static_cast<int>(menu_popup_->settings.places.size());
                        const int system_first_y = 65 + places_count * 27;
                        int selected_shortcut = -1;
                        if (content_y >= 34 && content_y < 34 + places_count * 27) {
                            const int row = (content_y - 34) / 27;
                            if (content_y < 34 + row * 27 + 25) selected_shortcut = row;
                        } else if (content_y >= system_first_y) {
                            const int row = (content_y - system_first_y) / 27;
                            if (row >= 0 && static_cast<std::size_t>(row) <
                                menu_popup_->settings.system.size() &&
                                content_y < system_first_y + row * 27 + 25) {
                                selected_shortcut = places_count + row;
                            }
                        }
                        if (selected_shortcut >= 0) {
                            parent.focus = 4;
                            parent.selected_shortcut =
                                static_cast<std::size_t>(selected_shortcut);
                            render_menu_popup();
                        }
                        return;
                    }
                    if (menu_popup_->settings.show_categories &&
                        x >= category_x && x < applications_x && y >= 34) {
                        parent.focus = 1;
                        const int local_index = (y - 34) / 27;
                        const int visible_rows = std::max(1, (footer_y - 34) / 27);
                        const int category_index = static_cast<int>(parent.category_scroll) +
                            local_index;
                        if (local_index >= visible_rows) return;
                        if (category_index >= 0 && static_cast<std::size_t>(category_index) <
                            parent.categories.size() && category_index != parent.hovered_category) {
                            parent.hovered_category = category_index;
                            parent.category = parent.categories[
                                static_cast<std::size_t>(category_index)];
                            if (parent.on_category) parent.on_category(parent.category);
                        }
                        return;
                    }
                    parent.hovered_category = -1;
                    if (x >= applications_x && y >= 8) {
                        parent.focus = 2;
                    }
                    if (x < applications_x || y < 8) return;
                    const int local_row = (y - 8) / 28;
                    const int visible_rows = std::max(1, (footer_y - 8) / 28);
                    if (local_row >= visible_rows) return;
                    const int selected = static_cast<int>(parent.scroll) + local_row;
                    if (selected >= 0 && static_cast<std::size_t>(selected) <
                        parent.items.size() && selected != parent.selected) {
                        parent.selected = selected;
                        render_menu_popup();
                    }
                    return;
                }
                y -= power_y;
            }
            const int selected = y >= 4 ? (y - 4) / 28 : -1;
            if (selected >= 0 && selected < 6 && selected != menu_popup_->selected) {
                menu_popup_->selected = selected;
                render_menu_popup();
            }
            return;
        }
        const int footer_height = menu_popup_->settings.show_categories ? 42 : 78;
        const int footer_y = menu_popup_->height - footer_height;
        if (y >= footer_y) {
            if (menu_popup_->settings.show_categories &&
                x >= category_x && x < applications_x) menu_popup_->focus = 3;
            else if (!menu_popup_->settings.show_categories && x >= applications_x &&
                     y < footer_y + 34) menu_popup_->focus = 3;
            else if (x >= applications_x) menu_popup_->focus = 0;
            return;
        }
        if (places_width > 0 && x < places_width) {
            const int content_y = y + menu_popup_->shortcut_scroll;
            const int places_count = static_cast<int>(menu_popup_->settings.places.size());
            const int system_first_y = 65 + places_count * 27;
            int selected_shortcut = -1;
            if (content_y >= 34 && content_y < 34 + places_count * 27) {
                const int row = (content_y - 34) / 27;
                if (content_y < 34 + row * 27 + 25) selected_shortcut = row;
            } else if (content_y >= system_first_y) {
                const int row = (content_y - system_first_y) / 27;
                if (row >= 0 && static_cast<std::size_t>(row) <
                    menu_popup_->settings.system.size() &&
                    content_y < system_first_y + row * 27 + 25) {
                    selected_shortcut = places_count + row;
                }
            }
            if (selected_shortcut >= 0 && static_cast<std::size_t>(selected_shortcut) !=
                menu_popup_->selected_shortcut) {
                menu_popup_->focus = 4;
                menu_popup_->selected_shortcut = static_cast<std::size_t>(selected_shortcut);
                render_menu_popup();
            }
            return;
        }
        if (menu_popup_->settings.show_categories &&
            x >= category_x && x < applications_x && y >= 34) {
            menu_popup_->focus = 1;
            const int local_index = (y - 34) / 27;
            const int visible_rows = std::max(1, (footer_y - 34) / 27);
            const int category_index = static_cast<int>(menu_popup_->category_scroll) +
                local_index;
            if (local_index >= visible_rows) return;
            if (category_index >= 0 &&
                static_cast<std::size_t>(category_index) < menu_popup_->categories.size() &&
                category_index != menu_popup_->hovered_category) {
                menu_popup_->hovered_category = category_index;
                menu_popup_->category = menu_popup_->categories[
                    static_cast<std::size_t>(category_index)];
                if (menu_popup_->on_category) {
                    menu_popup_->on_category(menu_popup_->category);
                }
            }
            return;
        }
        menu_popup_->hovered_category = -1;
        if (x >= applications_x && y >= 8) {
            menu_popup_->focus = 2;
        }
        if (x < applications_x || y < 8) return;
        const int local_row = (y - 8) / 28;
        const int visible_rows = std::max(1, (footer_y - 8) / 28);
        if (local_row >= visible_rows) return;
        const int selected = static_cast<int>(menu_popup_->scroll) + local_row;
        if (selected >= 0 && static_cast<std::size_t>(selected) < menu_popup_->items.size() &&
            selected != menu_popup_->selected) {
            menu_popup_->selected = selected;
            render_menu_popup();
        }
    }

    void update_popup_shortcut_scroll()
    {
        if (menu_popup_ == nullptr) return;
        const std::size_t places_count = menu_popup_->settings.places.size();
        const std::size_t selected = menu_popup_->selected_shortcut;
        int item_y = 34 + static_cast<int>(selected) * 27;
        if (selected >= places_count) {
            item_y = 65 + static_cast<int>(places_count) * 27 +
                static_cast<int>(selected - places_count) * 27;
        }
        const int footer_height = menu_popup_->settings.show_categories ? 42 : 78;
        const int list_bottom = menu_popup_->height - footer_height;
        if (item_y - menu_popup_->shortcut_scroll < 34) {
            menu_popup_->shortcut_scroll = item_y - 34;
        } else if (item_y + 25 - menu_popup_->shortcut_scroll > list_bottom) {
            menu_popup_->shortcut_scroll = item_y + 25 - list_bottom;
        }
        const int content_bottom = 65 +
            static_cast<int>(places_count + menu_popup_->settings.system.size()) * 27;
        menu_popup_->shortcut_scroll = std::clamp(menu_popup_->shortcut_scroll, 0,
            std::max(0, content_bottom - list_bottom));
    }

    void move_popup_selection(int offset)
    {
        if (menu_popup_ == nullptr) return;
        const int count = menu_popup_->power_menu
            ? 6 : static_cast<int>(menu_popup_->items.size());
        if (count <= 0) return;
        if (!menu_popup_->power_menu) menu_popup_->focus = 2;
        menu_popup_->selected = menu_popup_->power_menu
            ? (menu_popup_->selected + offset + count) % count
            : std::clamp(menu_popup_->selected + offset, 0, count - 1);
        if (menu_popup_->power_menu) {
            render_menu_popup();
            return;
        }
        if (offset > 0 && !menu_popup_->settings.show_categories &&
            menu_popup_->selected + 1 >= count) {
            menu_popup_->focus = 3;
            render_menu_popup();
            return;
        }
        const int footer_height = menu_popup_->settings.show_categories ? 42 : 78;
        const int visible_rows = std::max(1, (menu_popup_->height - footer_height - 8) / 28);
        if (menu_popup_->selected < static_cast<int>(menu_popup_->scroll)) {
            menu_popup_->scroll = static_cast<std::size_t>(menu_popup_->selected);
        } else if (menu_popup_->selected >=
                   static_cast<int>(menu_popup_->scroll) + visible_rows) {
            menu_popup_->scroll = static_cast<std::size_t>(
                menu_popup_->selected - visible_rows + 1);
        }
        render_menu_popup();
    }

    void move_popup_category(int offset)
    {
        if (menu_popup_ == nullptr || menu_popup_->power_menu ||
            menu_popup_->categories.empty() || !menu_popup_->on_category) return;
        menu_popup_->focus = 1;
        const auto found = std::find(menu_popup_->categories.begin(),
            menu_popup_->categories.end(), menu_popup_->category);
        const int current = found == menu_popup_->categories.end() ? 0 :
            static_cast<int>(found - menu_popup_->categories.begin());
        if (offset > 0 && current + 1 >= static_cast<int>(menu_popup_->categories.size())) {
            menu_popup_->focus = 3;
            render_menu_popup();
            return;
        }
        const int target = std::clamp(current + offset, 0,
            static_cast<int>(menu_popup_->categories.size() - 1));
        const int footer_height = menu_popup_->settings.show_categories ? 42 : 78;
        const std::size_t visible_rows = static_cast<std::size_t>(std::max(1,
            (menu_popup_->height - footer_height - 34) / 27));
        if (static_cast<std::size_t>(target) < menu_popup_->category_scroll) {
            menu_popup_->category_scroll = static_cast<std::size_t>(target);
        } else if (static_cast<std::size_t>(target) >=
                   menu_popup_->category_scroll + visible_rows) {
            menu_popup_->category_scroll = static_cast<std::size_t>(target) - visible_rows + 1;
        }
        menu_popup_->category = menu_popup_->categories[static_cast<std::size_t>(target)];
        menu_popup_->on_category(menu_popup_->category);
    }

    void update_menu_scrollbar_drag(int y)
    {
        if (menu_popup_ == nullptr || menu_popup_->scrollbar_drag_column == 0) return;
        MenuPopup& popup = *menu_popup_;
        const int footer_height = popup.settings.show_categories ? 42 : 78;
        const int list_bottom = popup.height - footer_height;
        const bool categories = popup.scrollbar_drag_column == 1;
        const int track_y = categories ? 34 : 8;
        const int track_height = list_bottom - track_y;
        const std::size_t content_count = categories
            ? popup.categories.size() : popup.items.size();
        const std::size_t visible_count = static_cast<std::size_t>(std::max(1,
            categories ? track_height / 27 : (list_bottom - 8) / 28));
        std::size_t& scroll = categories ? popup.category_scroll : popup.scroll;
        if (track_height <= 0 || content_count <= visible_count) return;
        const std::size_t maximum = content_count - visible_count;
        const int thumb_height = std::min(track_height, std::max(22,
            static_cast<int>(static_cast<std::size_t>(track_height) * visible_count /
                             content_count)));
        const int travel = track_height - thumb_height;
        if (travel <= 0) return;
        const int thumb_y = std::clamp(y - popup.scrollbar_drag_offset,
                                       track_y, track_y + travel);
        scroll = static_cast<std::size_t>(thumb_y - track_y) * maximum /
            static_cast<std::size_t>(travel);
        render_menu_popup(categories ? 1 : 2);
    }

    void scroll_popup_list(int offset)
    {
        if (menu_popup_ == nullptr || menu_popup_->power_menu || offset == 0) return;
        const int places_width = menu_popup_->settings.show_places
            ? menu_popup_->settings.places_width : 0;
        if (places_width > 0 && pointer_x_ < places_width) {
            const int footer_height = menu_popup_->settings.show_categories ? 42 : 78;
            const int list_bottom = menu_popup_->height - footer_height;
            const int content_bottom = 65 +
                static_cast<int>(menu_popup_->settings.places.size() +
                                 menu_popup_->settings.system.size()) * 27;
            menu_popup_->shortcut_scroll = std::clamp(
                menu_popup_->shortcut_scroll + offset * 54, 0,
                std::max(0, content_bottom - list_bottom));
            render_menu_popup(3);
            return;
        }
        const int visible_category_width = menu_popup_->settings.show_categories
            ? menu_popup_->settings.categories_width : 0;
        if (visible_category_width > 0 && pointer_x_ >= places_width &&
            pointer_x_ < places_width + visible_category_width) {
            const int footer_height = menu_popup_->settings.show_categories ? 42 : 78;
            const int visible_rows = std::max(1,
                (menu_popup_->height - footer_height - 34) / 27);
            const std::size_t maximum = menu_popup_->categories.size() >
                static_cast<std::size_t>(visible_rows)
                ? menu_popup_->categories.size() - static_cast<std::size_t>(visible_rows) : 0;
            if (offset < 0 && menu_popup_->category_scroll > 0) {
                --menu_popup_->category_scroll;
            } else if (offset > 0 && menu_popup_->category_scroll < maximum) {
                ++menu_popup_->category_scroll;
            }
            render_menu_popup(1);
            return;
        }
        const int footer_height = menu_popup_->settings.show_categories ? 42 : 78;
        const std::size_t visible_rows = static_cast<std::size_t>(std::max(1,
            (menu_popup_->height - footer_height - 8) / 28));
        const std::size_t max_scroll = menu_popup_->items.size() > visible_rows
            ? menu_popup_->items.size() - visible_rows : 0;
        if (offset < 0 && menu_popup_->scroll > 0) {
            --menu_popup_->scroll;
        } else if (offset > 0 && menu_popup_->scroll < max_scroll) {
            ++menu_popup_->scroll;
        }
        render_menu_popup(2);
    }

    void activate_popup_selection()
    {
        if (menu_popup_ == nullptr) return;
        if (menu_popup_->power_menu) {
            static const std::array<std::pair<const char*, const char*>, 6> actions = {{
                {"lock", "Lock Screen"}, {"logout", "Log Out"},
                {"suspend", "Suspend"}, {"hibernate", "Hibernate"},
                {"reboot", "Restart"}, {"shutdown", "Power Off"}
            }};
            const auto action = actions[static_cast<std::size_t>(menu_popup_->selected)];
            auto callback = menu_popup_->on_power_action;
            close_power_menu();
            if (callback) callback(action.first, action.second);
        } else if (menu_popup_->on_activate &&
                   static_cast<std::size_t>(menu_popup_->selected) < menu_popup_->items.size()) {
            menu_popup_->on_activate(static_cast<std::size_t>(menu_popup_->selected));
        }
    }

    static void pointer_enter(void* data, wl_pointer*, uint32_t, wl_surface* surface,
                              wl_fixed_t x, wl_fixed_t y)
    {
        auto* self = static_cast<WaylandBackendImpl*>(data);
        self->popup_leave_pending_ = false;
        self->task_menu_leave_pending_ = false;
        self->desktop_hub_leave_pending_ = false;
        self->pointer_surface_ = surface;
        self->pointer_x_ = wl_fixed_to_int(x);
        self->pointer_y_ = wl_fixed_to_int(y);
        if (surface == self->surface_ && self->backend_.motion_callback_) {
            self->backend_.motion_callback_(self->pointer_x_, self->pointer_y_);
        }
    }

    static void pointer_leave(void* data, wl_pointer*, uint32_t, wl_surface* surface)
    {
        auto* self = static_cast<WaylandBackendImpl*>(data);
        if (self->pointer_surface_ == surface) {
            self->pointer_surface_ = nullptr;
        }
        if (self->menu_popup_ != nullptr &&
            (surface == self->menu_popup_->surface || surface == self->surface_)) {
            self->popup_leave_pending_ = true;
        }
        if (self->task_menu_popup_ != nullptr &&
            (surface == self->task_menu_popup_->surface || surface == self->surface_)) {
            self->task_menu_leave_pending_ = true;
        }
        if (self->desktop_hub_popup_ != nullptr &&
            (surface == self->desktop_hub_popup_->surface || surface == self->surface_)) {
            self->desktop_hub_leave_pending_ = true;
        }
        if (surface == self->surface_ && self->backend_.motion_callback_) {
            self->pointer_x_ = -1;
            self->pointer_y_ = -1;
            self->backend_.motion_callback_(-1, -1);
        }
    }

    static void pointer_motion(void* data, wl_pointer*, uint32_t,
                               wl_fixed_t x, wl_fixed_t y)
    {
        auto* self = static_cast<WaylandBackendImpl*>(data);
        self->pointer_x_ = wl_fixed_to_int(x);
        self->pointer_y_ = wl_fixed_to_int(y);
        if (self->pointer_surface_ == self->surface_ && self->backend_.motion_callback_) {
            self->backend_.motion_callback_(self->pointer_x_, self->pointer_y_);
        } else if (self->desktop_hub_popup_ != nullptr &&
                   self->pointer_surface_ == self->desktop_hub_popup_->surface) {
            self->update_desktop_hub_hover(self->pointer_x_, self->pointer_y_);
        } else if (self->task_menu_popup_ != nullptr &&
                   self->pointer_surface_ == self->task_menu_popup_->surface) {
            self->update_task_menu_hover(self->pointer_y_);
        } else if (self->menu_popup_ != nullptr &&
                   self->pointer_surface_ == self->menu_popup_->surface) {
            self->update_popup_hover(self->pointer_x_, self->pointer_y_);
        }
    }

    static void pointer_button(void* data, wl_pointer*, uint32_t serial, uint32_t,
                               uint32_t button, uint32_t state)
    {
        auto* self = static_cast<WaylandBackendImpl*>(data);
        if (state == WL_POINTER_BUTTON_STATE_PRESSED &&
            self->pointer_surface_ == self->surface_) {
            self->pointer_serial_ = serial;
        }
        if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
            if (self->menu_popup_ != nullptr) {
                self->menu_popup_->scrollbar_drag_column = 0;
            }
            return;
        }
        if (state != WL_POINTER_BUTTON_STATE_PRESSED || self->pointer_x_ < 0 ||
            self->pointer_y_ < 0) {
            return;
        }
        if (self->task_menu_popup_ != nullptr &&
            self->pointer_surface_ == self->task_menu_popup_->surface) {
            if (button == BTN_LEFT) {
                self->handle_task_menu_click(self->pointer_y_);
            }
            return;
        }
        if (self->desktop_hub_popup_ != nullptr &&
            self->pointer_surface_ == self->desktop_hub_popup_->surface) {
            self->handle_desktop_hub_click(self->pointer_x_, self->pointer_y_);
            return;
        }
        if (self->calendar_popup_ != nullptr &&
            self->pointer_surface_ == self->calendar_popup_->surface) {
            self->handle_calendar_click(self->pointer_x_, self->pointer_y_);
            return;
        }
        if (self->menu_popup_ != nullptr &&
            self->pointer_surface_ == self->menu_popup_->surface) {
            self->handle_menu_popup_click(self->pointer_x_, self->pointer_y_,
                                          button == BTN_LEFT ? 1 : 0);
            return;
        }
        if (self->pointer_surface_ != self->surface_ || !self->backend_.click_callback_) {
            return;
        }
        std::optional<int> mapped_button;
        switch (button) {
        case BTN_LEFT: mapped_button = 1; break;
        case BTN_MIDDLE: mapped_button = 2; break;
        case BTN_RIGHT: mapped_button = 3; break;
        default: break;
        }
        if (mapped_button.has_value()) {
            self->backend_.click_callback_(self->pointer_x_, self->pointer_y_, *mapped_button);
        }
    }

    static void pointer_axis(void* data, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value)
    {
        auto* self = static_cast<WaylandBackendImpl*>(data);
        if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
        const double delta = wl_fixed_to_double(value);
        if (self->seat_version_ >= 5) self->axis_delta_[axis] += delta;
        else self->emit_smooth_scroll(delta);
    }

    static void pointer_frame(void* data, wl_pointer*)
    {
        auto* self = static_cast<WaylandBackendImpl*>(data);
        constexpr std::size_t vertical = WL_POINTER_AXIS_VERTICAL_SCROLL;
        int scroll_steps = 0;
        if (self->axis_value120_[vertical] != 0) {
            scroll_steps = self->axis_value120_[vertical] / 120;
            self->high_resolution_scroll_ += self->axis_value120_[vertical] % 120;
            if (self->high_resolution_scroll_ >= 120) {
                ++scroll_steps;
                self->high_resolution_scroll_ -= 120;
            } else if (self->high_resolution_scroll_ <= -120) {
                --scroll_steps;
                self->high_resolution_scroll_ += 120;
            }
        } else if (self->axis_discrete_[vertical] != 0) {
            scroll_steps = self->axis_discrete_[vertical];
        } else {
            self->emit_smooth_scroll(self->axis_delta_[vertical]);
        }
        if (scroll_steps != 0 && self->menu_popup_ != nullptr &&
            self->pointer_surface_ == self->menu_popup_->surface) {
            self->scroll_popup_list(scroll_steps);
        } else if (scroll_steps != 0) {
            self->emit_scroll_steps(scroll_steps);
        }
        self->axis_delta_.fill(0.0);
        self->axis_discrete_.fill(0);
        self->axis_value120_.fill(0);
    }

    static void pointer_axis_source(void*, wl_pointer*, uint32_t) {}
    static void pointer_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}

    static void pointer_axis_discrete(void* data, wl_pointer*, uint32_t axis, int32_t discrete)
    {
        if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
            static_cast<WaylandBackendImpl*>(data)->axis_discrete_[axis] += discrete;
        }
    }

    static void pointer_axis_value120(void* data, wl_pointer*, uint32_t axis, int32_t value120)
    {
        if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
            static_cast<WaylandBackendImpl*>(data)->axis_value120_[axis] += value120;
        }
    }

    static void layer_configure(void* data, zwlr_layer_surface_v1* layer_surface,
                                uint32_t serial, uint32_t width, uint32_t height)
    {
        auto* self = static_cast<WaylandBackendImpl*>(data);
        const int previous_width = self->backend_.width_;
        const int previous_height = self->backend_.height_;
        self->backend_.width_ = width > 0 ? static_cast<int>(width) : self->requested_width_;
        self->backend_.height_ = height > 0 ? static_cast<int>(height) : self->requested_height_;
        if (self->configured_ && !self->buffers_.empty() &&
            (previous_width != self->backend_.width_ ||
             previous_height != self->backend_.height_)) {
            self->resize_panel_buffers();
        }
        self->configured_ = true;
        zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
    }

    static void layer_closed(void* data, zwlr_layer_surface_v1*)
    {
        static_cast<WaylandBackendImpl*>(data)->configured_ = false;
    }

    static void panel_toplevel_configure(void* data, xdg_toplevel*, int32_t width,
                                         int32_t height, wl_array*)
    {
        auto* self = static_cast<WaylandBackendImpl*>(data);
        self->panel_configure_width_ = width;
        self->panel_configure_height_ = height;
    }

    static void panel_toplevel_close(void* data, xdg_toplevel*)
    {
        static_cast<WaylandBackendImpl*>(data)->configured_ = false;
    }

    static void panel_toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}

    static void panel_toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}

    static void panel_surface_configure(void* data, xdg_surface* surface, uint32_t serial)
    {
        auto* self = static_cast<WaylandBackendImpl*>(data);
        const int previous_width = self->backend_.width_;
        const int previous_height = self->backend_.height_;
        xdg_surface_ack_configure(surface, serial);
        self->backend_.width_ = self->panel_configure_width_ > 0
            ? self->panel_configure_width_
            : self->requested_width_ > 0 ? self->requested_width_ : 1000;
        self->backend_.height_ = self->panel_configure_height_ > 0
            ? self->panel_configure_height_
            : self->requested_height_ > 0 ? self->requested_height_ : 30;
        if (self->configured_ && !self->buffers_.empty() &&
            (previous_width != self->backend_.width_ ||
             previous_height != self->backend_.height_)) {
            self->resize_panel_buffers();
        }
        self->configured_ = true;
    }

    static void buffer_release(void* data, wl_buffer*)
    {
        static_cast<Buffer*>(data)->busy = false;
    }

    void emit_smooth_scroll(double delta)
    {
        if (menu_popup_ != nullptr && pointer_surface_ == menu_popup_->surface) {
            if (!menu_popup_->power_menu) {
                smooth_scroll_ += delta;
                while (smooth_scroll_ >= 10.0) {
                    smooth_scroll_ -= 10.0;
                    scroll_popup_list(1);
                }
                while (smooth_scroll_ <= -10.0) {
                    smooth_scroll_ += 10.0;
                    scroll_popup_list(-1);
                }
            }
            return;
        }
        if (!backend_.click_callback_ || pointer_x_ < 0 || pointer_y_ < 0) return;
        smooth_scroll_ += delta;
        while (smooth_scroll_ >= 10.0) {
            smooth_scroll_ -= 10.0;
            backend_.click_callback_(pointer_x_, pointer_y_, 5);
        }
        while (smooth_scroll_ <= -10.0) {
            smooth_scroll_ += 10.0;
            backend_.click_callback_(pointer_x_, pointer_y_, 4);
        }
    }

    void emit_scroll_steps(int32_t steps)
    {
        if (!backend_.click_callback_ || pointer_x_ < 0 || pointer_y_ < 0) return;
        const int button = steps > 0 ? 5 : 4;
        const int64_t count = steps > 0 ? steps : -static_cast<int64_t>(steps);
        for (int64_t step = 0; step < count; ++step) {
            backend_.click_callback_(pointer_x_, pointer_y_, button);
        }
    }

    void emit_value120(int32_t value)
    {
        high_resolution_scroll_ += value;
        while (high_resolution_scroll_ >= 120) {
            high_resolution_scroll_ -= 120;
            emit_scroll_steps(1);
        }
        while (high_resolution_scroll_ <= -120) {
            high_resolution_scroll_ += 120;
            emit_scroll_steps(-1);
        }
    }

    static void draw_menu_text(cairo_t* cr, const std::string& text,
                               double x, double y, int max_width,
                               const char* font_name, RgbColor color)
    {
        PangoLayout* layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(layout, text.c_str(), -1);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_width(layout, max_width * PANGO_SCALE);
        PangoFontDescription* font = pango_font_description_from_string(font_name);
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        cairo_set_source_rgb(cr, color.red, color.green, color.blue);
        cairo_move_to(cr, x, y);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }

    void set_panel_keyboard_interactivity(uint32_t mode)
    {
        if (layer_surface_ != nullptr) {
            zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface_, mode);
            wl_surface_commit(surface_);
        }
    }

    xdg_surface* popup_parent_surface() const
    {
        return layer_surface_ == nullptr ? panel_xdg_surface_ : nullptr;
    }

    void attach_popup_to_panel(xdg_popup* popup)
    {
        if (layer_surface_ != nullptr) {
            zwlr_layer_surface_v1_get_popup(layer_surface_, popup);
        }
    }

    void open_task_menu_popup(int anchor_x, std::string title,
        std::vector<std::string> items, std::function<void(std::size_t)> on_activate)
    {
        if (task_menu_popup_ != nullptr) {
            close_task_menu_popup();
        }
        if (menu_popup_ != nullptr) close_menu_popup(false);
        if (calendar_popup_ != nullptr) close_calendar_popup();
        if (desktop_hub_popup_ != nullptr) close_desktop_hub_popup();
        task_menu_popup_ = std::make_unique<TaskMenuPopup>();
        task_menu_popup_->owner = this;
        task_menu_popup_->anchor_x = anchor_x;
        task_menu_popup_->title = std::move(title);
        task_menu_popup_->items = std::move(items);
        task_menu_popup_->on_activate = std::move(on_activate);
        task_menu_popup_->height = 34 + static_cast<int>(task_menu_popup_->items.size()) *
            task_menu_popup_->row_height + 4;
        task_menu_leave_pending_ = false;

        set_panel_keyboard_interactivity(
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
        task_menu_popup_->surface = wl_compositor_create_surface(compositor_);
        if (task_menu_popup_->surface == nullptr) {
            close_task_menu_popup();
            return;
        }
        task_menu_popup_->surface_role = xdg_wm_base_get_xdg_surface(
            wm_base_, task_menu_popup_->surface);
        static const xdg_surface_listener surface_listener = {task_menu_surface_configure};
        xdg_surface_add_listener(task_menu_popup_->surface_role, &surface_listener,
                                 task_menu_popup_.get());

        xdg_positioner* positioner = xdg_wm_base_create_positioner(wm_base_);
        configure_panel_positioner(positioner, task_menu_popup_->anchor_x, 1,
                                   task_menu_popup_->width, task_menu_popup_->height);
        task_menu_popup_->popup = xdg_surface_get_popup(
            task_menu_popup_->surface_role, popup_parent_surface(), positioner);
        xdg_positioner_destroy(positioner);
        if (task_menu_popup_->popup == nullptr) {
            close_task_menu_popup();
            return;
        }
        static const xdg_popup_listener popup_listener = {
            .configure = task_menu_configure,
            .popup_done = task_menu_done,
            .repositioned = popup_repositioned
        };
        xdg_popup_add_listener(task_menu_popup_->popup, &popup_listener,
                               task_menu_popup_.get());
        attach_popup_to_panel(task_menu_popup_->popup);
        if (seat_ != nullptr && pointer_serial_ != 0) {
            xdg_popup_grab(task_menu_popup_->popup, seat_, pointer_serial_);
            pointer_serial_ = 0;
        }
        wl_surface_commit(task_menu_popup_->surface);
        wl_display_flush(display_);
    }

    void close_task_menu_popup()
    {
        if (task_menu_popup_ == nullptr) {
            return;
        }
        if (task_menu_popup_->surface != nullptr) {
            wl_surface_attach(task_menu_popup_->surface, nullptr, 0, 0);
            wl_surface_commit(task_menu_popup_->surface);
        }
        if (task_menu_popup_->popup != nullptr) {
            xdg_popup_destroy(task_menu_popup_->popup);
        }
        if (task_menu_popup_->surface_role != nullptr) {
            xdg_surface_destroy(task_menu_popup_->surface_role);
        }
        if (task_menu_popup_->surface != nullptr) {
            wl_surface_destroy(task_menu_popup_->surface);
        }
        set_panel_keyboard_interactivity(ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        wl_display_flush(display_);
        wl_display_roundtrip(display_);
        task_menu_leave_pending_ = false;
        task_menu_popup_.reset();
    }

    void render_task_menu_popup()
    {
        if (task_menu_popup_ == nullptr || !task_menu_popup_->configured) {
            return;
        }
        std::size_t buffer_index = find_free_buffer(task_menu_popup_->buffers);
        if (buffer_index == task_menu_popup_->buffers.size()) {
            if (!create_buffer_set(task_menu_popup_->buffers, task_menu_popup_->width,
                                   task_menu_popup_->height)) {
                return;
            }
            buffer_index = find_free_buffer(task_menu_popup_->buffers);
            if (buffer_index == task_menu_popup_->buffers.size()) {
                return;
            }
        }
        Buffer& buffer = *task_menu_popup_->buffers[buffer_index];
        cairo_t* cr = buffer.cairo;
        cairo_set_source_rgb(cr, 245.0 / 255.0, 245.0 / 255.0, 245.0 / 255.0);
        cairo_paint(cr);
        draw_menu_text(cr, task_menu_popup_->title, 10.0, 8.0,
            task_menu_popup_->width - 20, "Sans Bold 9",
            {32.0 / 255.0, 32.0 / 255.0, 32.0 / 255.0});
        for (std::size_t index = 0; index < task_menu_popup_->items.size(); ++index) {
            const int y = 32 + static_cast<int>(index) * task_menu_popup_->row_height;
            if (index == task_menu_popup_->selected) {
                cairo_set_source_rgb(cr, 220.0 / 255.0, 233.0 / 255.0, 203.0 / 255.0);
                cairo_rectangle(cr, 4.0, y, task_menu_popup_->width - 8.0,
                                task_menu_popup_->row_height);
                cairo_fill(cr);
            }
            draw_menu_text(cr, task_menu_popup_->items[index], 12.0, y + 6.0,
                task_menu_popup_->width - 24, "Sans 9",
                {32.0 / 255.0, 32.0 / 255.0, 32.0 / 255.0});
        }
        cairo_set_source_rgb(cr, 119.0 / 255.0, 119.0 / 255.0, 119.0 / 255.0);
        cairo_rectangle(cr, 0.5, 0.5, task_menu_popup_->width - 1.0,
                        task_menu_popup_->height - 1.0);
        cairo_stroke(cr);
        cairo_surface_flush(buffer.cairo_surface);
        wl_surface_attach(task_menu_popup_->surface, buffer.buffer, 0, 0);
        wl_surface_damage(task_menu_popup_->surface, 0, 0,
                          task_menu_popup_->width, task_menu_popup_->height);
        buffer.busy = true;
        wl_surface_commit(task_menu_popup_->surface);
        wl_display_flush(display_);
    }

    void update_task_menu_hover(int y)
    {
        if (task_menu_popup_ == nullptr || y < 32) {
            return;
        }
        const std::size_t selected = static_cast<std::size_t>(
            (y - 32) / task_menu_popup_->row_height);
        if (selected < task_menu_popup_->items.size() && selected != task_menu_popup_->selected) {
            task_menu_popup_->selected = selected;
            render_task_menu_popup();
        }
    }

    void activate_task_menu_selection()
    {
        if (task_menu_popup_ == nullptr ||
            task_menu_popup_->selected >= task_menu_popup_->items.size()) {
            return;
        }
        auto callback = task_menu_popup_->on_activate;
        const std::size_t selected = task_menu_popup_->selected;
        close_task_menu_popup();
        if (callback) {
            callback(selected);
        }
    }

    void handle_task_menu_click(int y)
    {
        if (task_menu_popup_ == nullptr || y < 32) {
            return;
        }
        const std::size_t selected = static_cast<std::size_t>(
            (y - 32) / task_menu_popup_->row_height);
        if (selected < task_menu_popup_->items.size()) {
            task_menu_popup_->selected = selected;
            activate_task_menu_selection();
        }
    }

    void handle_task_menu_key(xkb_keysym_t symbol)
    {
        if (task_menu_popup_ == nullptr) {
            return;
        }
        if (symbol == XKB_KEY_Escape) {
            close_task_menu_popup();
        } else if ((symbol == XKB_KEY_Up || symbol == XKB_KEY_Down) &&
                   !task_menu_popup_->items.empty()) {
            const int count = static_cast<int>(task_menu_popup_->items.size());
            const int offset = symbol == XKB_KEY_Up ? -1 : 1;
            task_menu_popup_->selected = static_cast<std::size_t>(
                (static_cast<int>(task_menu_popup_->selected) + offset + count) % count);
            render_task_menu_popup();
        } else if (symbol == XKB_KEY_Return || symbol == XKB_KEY_KP_Enter) {
            activate_task_menu_selection();
        }
    }

    void create_menu_popup()
    {
        MenuPopup& popup = *menu_popup_;
        set_panel_keyboard_interactivity(
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
        popup.surface = wl_compositor_create_surface(compositor_);
        if (popup.surface == nullptr) {
            close_menu_popup(false);
            return;
        }
        popup.surface_role = xdg_wm_base_get_xdg_surface(wm_base_, popup.surface);
        static const xdg_surface_listener surface_listener = {popup_surface_configure};
        xdg_surface_add_listener(popup.surface_role, &surface_listener, &popup);

        xdg_positioner* positioner = xdg_wm_base_create_positioner(wm_base_);
        configure_panel_positioner(positioner, popup.anchor_x, popup.anchor_width,
                                   popup.width, popup.height);
        popup.popup = xdg_surface_get_popup(popup.surface_role,
                            popup_parent_surface(), positioner);
        xdg_positioner_destroy(positioner);
        if (popup.popup == nullptr) {
            close_menu_popup(false);
            return;
        }
        static const xdg_popup_listener popup_listener = {
            .configure = popup_configure,
            .popup_done = popup_done,
            .repositioned = popup_repositioned
        };
        xdg_popup_add_listener(popup.popup, &popup_listener, &popup);
        attach_popup_to_panel(popup.popup);
        if (seat_ != nullptr && pointer_serial_ != 0) {
            xdg_popup_grab(popup.popup, seat_, pointer_serial_);
            pointer_serial_ = 0;
        } else {
            panel_debug("application menu popup opened without a pointer grab serial");
        }
        wl_surface_commit(popup.surface);
        if (wl_display_flush(display_) < 0 && errno != EAGAIN) {
            close_menu_popup(false);
        }
    }

    void close_menu_popup(bool notify)
    {
        if (menu_popup_ == nullptr) {
            return;
        }
        std::function<void()> on_close = std::move(menu_popup_->on_close);
        if (menu_popup_->surface != nullptr) {
            wl_surface_attach(menu_popup_->surface, nullptr, 0, 0);
            wl_surface_commit(menu_popup_->surface);
        }
        if (menu_popup_->popup != nullptr) {
            xdg_popup_destroy(menu_popup_->popup);
        }
        if (menu_popup_->surface_role != nullptr) {
            xdg_surface_destroy(menu_popup_->surface_role);
        }
        if (menu_popup_->surface != nullptr) {
            wl_surface_destroy(menu_popup_->surface);
        }
        wl_display_flush(display_);
        wl_display_roundtrip(display_);
        menu_popup_.reset();
        power_menu_return_state_.reset();
        set_panel_keyboard_interactivity(ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        wl_display_flush(display_);
        if (notify && on_close) {
            on_close();
        }
    }

    void close_power_menu(bool notify = false)
    {
        if (menu_popup_ == nullptr || !menu_popup_->power_menu) {
            return;
        }
        if (power_menu_return_state_ == nullptr) {
            close_menu_popup(notify);
            return;
        }
        auto state = std::move(power_menu_return_state_);
        menu_popup_->categories = std::move(state->categories);
        menu_popup_->items = std::move(state->items);
        menu_popup_->on_category = std::move(state->on_category);
        menu_popup_->on_search = std::move(state->on_search);
        menu_popup_->on_activate = std::move(state->on_activate);
        menu_popup_->on_power_action = std::move(state->on_power_action);
        menu_popup_->on_close = std::move(state->on_close);
        menu_popup_->query = std::move(state->query);
        menu_popup_->category = std::move(state->category);
        menu_popup_->anchor_x = state->anchor_x;
        menu_popup_->anchor_width = state->anchor_width;
        menu_popup_->width = state->width;
        menu_popup_->height = state->height;
        menu_popup_->selected = state->selected;
        menu_popup_->selected_shortcut = state->selected_shortcut;
        menu_popup_->scroll = state->scroll;
        menu_popup_->shortcut_scroll = state->shortcut_scroll;
        menu_popup_->hovered_category = state->hovered_category;
        menu_popup_->focus = state->focus;
        menu_popup_->tab_held = state->tab_held;
        menu_popup_->power_menu = false;
        render_menu_popup();
    }

    static void popup_surface_configure(void* data, xdg_surface* surface, uint32_t serial)
    {
        auto* popup = static_cast<MenuPopup*>(data);
        xdg_surface_ack_configure(surface, serial);
        popup->configured = true;
        if (popup->buffers.empty() &&
            !popup->owner->create_buffer_set(popup->buffers, popup->width, popup->height)) {
            popup->closed = true;
            return;
        }
        popup->owner->render_menu_popup();
    }

    static void calendar_popup_surface_configure(void* data, xdg_surface* surface, uint32_t serial)
    {
        auto* popup = static_cast<CalendarPopup*>(data);
        xdg_surface_ack_configure(surface, serial);
        popup->configured = true;
        if (popup->buffers.empty() &&
            !popup->owner->create_buffer_set(popup->buffers, popup->width, popup->height)) {
            popup->closed = true;
            return;
        }
        popup->owner->render_calendar_popup();
    }

    static void desktop_hub_popup_surface_configure(void* data, xdg_surface* surface, uint32_t serial)
    {
        auto* popup = static_cast<DesktopHubPopup*>(data);
        xdg_surface_ack_configure(surface, serial);
        popup->configured = true;
        if (popup->buffers.empty() &&
            !popup->owner->create_buffer_set(popup->buffers, popup->width, popup->height)) {
            popup->closed = true;
            return;
        }
        popup->owner->render_desktop_hub_popup();
    }

    static void task_menu_surface_configure(void* data, xdg_surface* surface, uint32_t serial)
    {
        auto* popup = static_cast<TaskMenuPopup*>(data);
        xdg_surface_ack_configure(surface, serial);
        popup->configured = true;
        if (popup->buffers.empty() &&
            !popup->owner->create_buffer_set(popup->buffers, popup->width, popup->height)) {
            popup->closed = true;
            return;
        }
        popup->owner->render_task_menu_popup();
    }

    static void tooltip_surface_configure(void* data, xdg_surface* surface, uint32_t serial)
    {
        auto* popup = static_cast<TooltipPopup*>(data);
        xdg_surface_ack_configure(surface, serial);
        popup->configured = true;
        popup->owner->render_tooltip_popup();
    }

    static void popup_configure(void* data, xdg_popup*, int32_t x, int32_t y,
                                int32_t width, int32_t height)
    {
        auto* popup = static_cast<MenuPopup*>(data);
        panel_debug("application menu popup configured: x=", x, " y=", y,
            " size=", width, "x", height);
        if (width > 0) popup->width = width;
        if (height > 0) popup->height = height;
    }

    static void calendar_popup_configure(void* data, xdg_popup*, int32_t, int32_t,
                                        int32_t width, int32_t height)
    {
        auto* popup = static_cast<CalendarPopup*>(data);
        if (width > 0) popup->width = width;
        if (height > 0) popup->height = height;
    }

    static void desktop_hub_popup_configure(void* data, xdg_popup*, int32_t, int32_t,
                                          int32_t width, int32_t height)
    {
        auto* popup = static_cast<DesktopHubPopup*>(data);
        if (width > 0) popup->width = width;
        if (height > 0) popup->height = height;
    }

    static void task_menu_configure(void* data, xdg_popup*, int32_t, int32_t,
                                    int32_t width, int32_t height)
    {
        auto* popup = static_cast<TaskMenuPopup*>(data);
        if (width > 0) popup->width = width;
        if (height > 0) popup->height = height;
    }

    static void tooltip_popup_configure(void*, xdg_popup*, int32_t, int32_t,
                                        int32_t, int32_t) {}

    static void popup_done(void* data, xdg_popup*)
    {
        panel_debug("compositor dismissed application menu popup");
        static_cast<MenuPopup*>(data)->closed = true;
    }

    static void calendar_popup_done(void* data, xdg_popup*)
    {
        static_cast<CalendarPopup*>(data)->closed = true;
    }

    static void desktop_hub_popup_done(void* data, xdg_popup*)
    {
        static_cast<DesktopHubPopup*>(data)->closed = true;
    }

    static void task_menu_done(void* data, xdg_popup*)
    {
        static_cast<TaskMenuPopup*>(data)->closed = true;
    }

    static void tooltip_popup_done(void* data, xdg_popup*)
    {
        static_cast<TooltipPopup*>(data)->closed = true;
    }

    static void popup_repositioned(void*, xdg_popup*, uint32_t) {}

    bool create_buffer_set(std::vector<std::unique_ptr<Buffer>>& target,
                          int width, int height)
    {
        const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
        if (stride <= 0 || static_cast<std::size_t>(height) >
            std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(stride)) {
            std::cerr << "Invalid Cairo shared-memory buffer dimensions.\n";
            return false;
        }
        const std::size_t size = static_cast<std::size_t>(stride) * height;
        if (size > static_cast<std::size_t>(std::numeric_limits<int32_t>::max())) {
            std::cerr << "Cairo shared-memory buffer exceeds Wayland's size limit.\n";
            return false;
        }

        static uint64_t shm_id = 0;
        for (int index = 0; index < 3; ++index) {
            auto buffer = std::make_unique<Buffer>();
            buffer->size = size;
            int fd = -1;
            std::string shm_name;
            for (int attempt = 0; attempt < 16 && fd < 0; ++attempt) {
                shm_name = "/simpanel-" + std::to_string(getpid()) + "-" +
                    std::to_string(++shm_id);
                fd = shm_open(shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
            }
            if (fd < 0) {
                std::cerr << "Could not create Wayland shared memory.\n";
                return false;
            }
            shm_unlink(shm_name.c_str());
            if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
                close(fd);
                std::cerr << "Could not size Wayland shared memory.\n";
                return false;
            }
            buffer->data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (buffer->data == MAP_FAILED) {
                close(fd);
                std::cerr << "Could not map Wayland shared memory.\n";
                return false;
            }
            wl_shm_pool* pool = wl_shm_create_pool(shm_, fd, static_cast<int32_t>(size));
            close(fd);
            if (pool == nullptr) return false;
            buffer->buffer = wl_shm_pool_create_buffer(pool, 0, width,
                height, stride, WL_SHM_FORMAT_ARGB8888);
            wl_shm_pool_destroy(pool);
            if (buffer->buffer == nullptr) return false;
            static const wl_buffer_listener listener = {buffer_release};
                wl_buffer_add_listener(buffer->buffer, &listener, buffer.get());
            buffer->cairo_surface = cairo_image_surface_create_for_data(
                static_cast<unsigned char*>(buffer->data), CAIRO_FORMAT_ARGB32,
                width, height, stride);
            if (cairo_surface_status(buffer->cairo_surface) != CAIRO_STATUS_SUCCESS) return false;
            buffer->cairo = cairo_create(buffer->cairo_surface);
            if (cairo_status(buffer->cairo) != CAIRO_STATUS_SUCCESS) return false;
            target.push_back(std::move(buffer));
        }
        return true;
    }

    bool create_buffers()
    {
        return create_buffer_set(buffers_, backend_.width_, backend_.height_);
    }

    bool resize_panel_buffers()
    {
        retired_buffers_.push_back(std::move(buffers_));
        if (!create_buffers()) {
            buffers_.clear();
            std::cerr << "Could not allocate buffers for the resized panel surface.\n";
            return false;
        }
        return true;
    }

    void reap_retired_buffers()
    {
        std::erase_if(retired_buffers_, [](const auto& buffer_set) {
            return std::none_of(buffer_set.begin(), buffer_set.end(), [](const auto& buffer) {
                return buffer->busy;
            });
        });
    }

    static std::string menu_category_icon(const std::string& category)
    {
        if (category == "All") return "view-app-grid-symbolic";
        if (category == "Accessories") return "applications-accessories-symbolic";
        if (category == "Graphics") return "applications-graphics-symbolic";
        if (category == "Internet") return "applications-internet-symbolic";
        if (category == "Office") return "applications-office-symbolic";
        if (category == "Sound & Video") return "applications-multimedia-symbolic";
        if (category == "Development") return "applications-development-symbolic";
        if (category == "Education") return "applications-science-symbolic";
        if (category == "Games") return "applications-games-symbolic";
        if (category == "System Tools") return "applications-system-symbolic";
        return "application-x-executable-symbolic";
    }

    void render_menu_popup(int redraw_column = 0)
    {
        if (menu_popup_ == nullptr || !menu_popup_->configured) return;
        MenuPopup& popup = *menu_popup_;
        std::size_t buffer_index = popup.buffers.size();
        for (std::size_t index = 0; index < popup.buffers.size(); ++index) {
            if (!popup.buffers[index]->busy) {
                buffer_index = index;
                break;
            }
        }
        if (buffer_index == popup.buffers.size()) return;

        Buffer& buffer = *popup.buffers[buffer_index];
        if (redraw_column != 0 && popup.last_buffer_index >= 0 &&
            static_cast<std::size_t>(popup.last_buffer_index) < popup.buffers.size() &&
            static_cast<std::size_t>(popup.last_buffer_index) != buffer_index) {
            const Buffer& previous = *popup.buffers[
                static_cast<std::size_t>(popup.last_buffer_index)];
            if (previous.size == buffer.size) {
                std::memcpy(buffer.data, previous.data, buffer.size);
                cairo_surface_mark_dirty(buffer.cairo_surface);
            }
        }
        cairo_t* cr = buffer.cairo;
        const int category_width = popup.settings.categories_width;
        const int visible_category_width = popup.settings.show_categories ? category_width : 0;
        const int places_width = popup.settings.show_places
            ? popup.settings.places_width : 0;
        const int category_x = places_width;
        const int applications_x = category_x + visible_category_width;
        const int footer_height = popup.settings.show_categories ? 42 : 78;
        constexpr int list_top = 8;
        constexpr int category_top = 34;
        constexpr int row_height = 28;
        const int list_bottom = popup.height - footer_height;
        const int visible_rows = std::max(1, (list_bottom - list_top) / row_height);
        const double width = popup.width;
        const double height = popup.height;
        const MenuReturnState* parent_state = power_menu_return_state_.get();
        const std::size_t base_selected = parent_state != nullptr
            ? parent_state->selected : popup.selected;
        const std::size_t base_scroll = parent_state != nullptr
            ? parent_state->scroll : popup.scroll;
        const std::size_t base_category_scroll = parent_state != nullptr
            ? parent_state->category_scroll : popup.category_scroll;
        const int base_focus = parent_state != nullptr ? parent_state->focus : popup.focus;
        const std::size_t base_shortcut = parent_state != nullptr
            ? parent_state->selected_shortcut : popup.selected_shortcut;
        const int base_shortcut_scroll = parent_state != nullptr
            ? parent_state->shortcut_scroll : popup.shortcut_scroll;

        int damage_x = 0;
        int damage_y = 0;
        int damage_width = popup.width;
        int damage_height = popup.height;
        if (redraw_column != 0) {
            if (redraw_column == 1) {
                damage_x = category_x;
                damage_y = category_top;
                damage_width = category_width;
                damage_height = list_bottom - category_top;
            } else if (redraw_column == 2) {
                damage_x = applications_x;
                damage_y = list_top;
                damage_width = popup.width - applications_x;
                damage_height = list_bottom - list_top;
            } else {
                damage_width = places_width;
                damage_height = list_bottom;
            }
            if (damage_width <= 0 || damage_height <= 0) {
                redraw_column = 0;
                damage_x = 0;
                damage_y = 0;
                damage_width = popup.width;
                damage_height = popup.height;
            } else {
                cairo_save(cr);
                cairo_rectangle(cr, damage_x, damage_y, damage_width, damage_height);
                cairo_clip(cr);
            }
        }

        cairo_set_source_rgb(cr, 245.0 / 255.0, 245.0 / 255.0, 245.0 / 255.0);
        cairo_paint(cr);

        if (popup.power_menu && parent_state == nullptr) {
            static const std::array<std::pair<const char*, const char*>, 6> actions = {{
                {"Lock Screen", "system-lock-screen-symbolic"},
                {"Log Out", "system-log-out-symbolic"},
                {"Suspend", "media-playback-pause-symbolic"},
                {"Hibernate", "weather-clear-night-symbolic"},
                {"Restart", "system-reboot-symbolic"},
                {"Power Off", "system-shutdown-symbolic"}
            }};
            for (std::size_t index = 0; index < actions.size(); ++index) {
                const int y = 4 + static_cast<int>(index) * 28;
                if (static_cast<int>(index) == popup.selected) {
                    if (index >= 4) {
                        cairo_set_source_rgb(cr, 239.0 / 255.0, 212.0 / 255.0, 209.0 / 255.0);
                    } else {
                        cairo_set_source_rgb(cr, 224.0 / 255.0, 233.0 / 255.0, 210.0 / 255.0);
                    }
                    cairo_rectangle(cr, 4, y, width - 8, 26);
                    cairo_fill(cr);
                }
                paint_theme_icon(cr, actions[index].second, 18, y + 13, 16);
                draw_menu_text(cr, actions[index].first, 34, y + 5, popup.width - 40,
                               "Sans 9", {37.0 / 255.0, 37.0 / 255.0, 37.0 / 255.0});
            }
        } else {
            if (popup.settings.show_categories) {
                cairo_set_source_rgb(cr, 238.0 / 255.0, 238.0 / 255.0, 238.0 / 255.0);
                cairo_rectangle(cr, category_x, 0, category_width, list_bottom);
                cairo_fill(cr);
                cairo_set_source_rgb(cr, 211.0 / 255.0, 211.0 / 255.0, 211.0 / 255.0);
                cairo_set_line_width(cr, 1.0);
                cairo_move_to(cr, category_x + category_width + 0.5, 0);
                cairo_line_to(cr, category_x + category_width + 0.5, height);
                cairo_stroke(cr);
            }
            if (places_width > 0) {
                cairo_set_source_rgb(cr, 211.0 / 255.0, 211.0 / 255.0, 211.0 / 255.0);
                cairo_move_to(cr, places_width + 0.5, 0);
                cairo_line_to(cr, places_width + 0.5, list_bottom);
                cairo_stroke(cr);
            }

            auto draw_text = [cr](const std::string& text, const char* font_name,
                                  double x, double y, double max_width, bool centered,
                                  RgbColor color) {
                PangoLayout* layout = pango_cairo_create_layout(cr);
                pango_layout_set_text(layout, text.c_str(), -1);
                pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
                pango_layout_set_width(layout, static_cast<int>(max_width * PANGO_SCALE));
                PangoFontDescription* font = pango_font_description_from_string(font_name);
                pango_layout_set_font_description(layout, font);
                pango_font_description_free(font);
                int text_width = 0;
                int text_height = 0;
                pango_layout_get_pixel_size(layout, &text_width, &text_height);
                cairo_set_source_rgb(cr, color.red, color.green, color.blue);
                cairo_move_to(cr, centered ? x + (max_width - text_width) / 2.0 : x,
                              y + (24.0 - text_height) / 2.0);
                pango_cairo_show_layout(cr, layout);
                g_object_unref(layout);
            };

            const auto draw_scrollbar = [cr](int x, int y, int track_height,
                                              std::size_t content_count,
                                              std::size_t visible_count,
                                              std::size_t scroll) {
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

            const RgbColor dark_text{76.0 / 255.0, 76.0 / 255.0, 76.0 / 255.0};
            const RgbColor item_text{41.0 / 255.0, 41.0 / 255.0, 41.0 / 255.0};
            const RgbColor selected_fill{219.0 / 255.0, 232.0 / 255.0, 202.0 / 255.0};
            const RgbColor focus_border{79.0 / 255.0, 127.0 / 255.0, 23.0 / 255.0};
            if (popup.settings.show_categories) {
                draw_text("Applications", "Sans Bold 10", category_x + 8, 5,
                          category_width - 25, false, dark_text);
            }

            if (popup.settings.show_categories) {
            const int category_visible_rows = std::max(1,
                (list_bottom - category_top) / 27);
            for (int row = 0; row < category_visible_rows; ++row) {
                const std::size_t index = base_category_scroll +
                    static_cast<std::size_t>(row);
                if (index >= popup.categories.size()) break;
                const int y = category_top + row * 27;
                const bool active = popup.categories[index] == popup.category;
                const bool focused = base_focus == 1 &&
                    popup.categories[index] == popup.category;
                if (active || focused) {
                    cairo_set_source_rgb(cr, selected_fill.red, selected_fill.green,
                                         selected_fill.blue);
                    cairo_rectangle(cr, category_x + 4, y, category_width - 8, 25);
                    cairo_fill(cr);
                }
                if (focused) {
                    cairo_set_source_rgb(cr, focus_border.red, focus_border.green,
                                         focus_border.blue);
                    cairo_rectangle(cr, category_x + 4.5, y + 0.5,
                                    category_width - 9, 24);
                    cairo_stroke(cr);
                }
                paint_theme_icon(cr, menu_category_icon(popup.categories[index]),
                                 category_x + 17, y + 12.5, 16);
                draw_text(popup.categories[index], "Sans 9", category_x + 29, y,
                          category_width - 46, false, item_text);
            }
            draw_scrollbar(category_x + category_width - 17, category_top,
                list_bottom - category_top, popup.categories.size(),
                static_cast<std::size_t>(category_visible_rows), base_category_scroll);
            }

            for (int row = 0; row < visible_rows; ++row) {
                const std::size_t index = base_scroll + static_cast<std::size_t>(row);
                if (index >= popup.items.size()) break;
                const int y = list_top + row * row_height;
                if (index == base_selected) {
                    cairo_set_source_rgb(cr, 220.0 / 255.0, 233.0 / 255.0, 203.0 / 255.0);
                    cairo_rectangle(cr, applications_x + 4, y,
                                    popup.width - applications_x - 14, row_height - 1);
                    cairo_fill(cr);
                    if (base_focus == 2) {
                        cairo_set_source_rgb(cr, focus_border.red, focus_border.green,
                                             focus_border.blue);
                        cairo_rectangle(cr, applications_x + 4.5, y + 0.5,
                            popup.width - applications_x - 15, row_height - 2);
                        cairo_stroke(cr);
                    }
                }
                paint_theme_icon(cr, popup.items[index].icon_name,
                    applications_x + 19, y + row_height / 2.0, 18);
                draw_text(popup.items[index].label, "Sans 10", applications_x + 34,
                          y, popup.width - applications_x - 61, false, item_text);
            }
            draw_scrollbar(popup.width - 17, list_top, list_bottom - list_top,
                popup.items.size(), static_cast<std::size_t>(visible_rows), base_scroll);

            if (places_width > 0) {
                cairo_save(cr);
                cairo_rectangle(cr, 0, 0, places_width, list_bottom);
                cairo_clip(cr);
                const auto draw_shortcuts = [&](const std::string& heading,
                    const std::vector<ApplicationMenuDisplayItem>& entries, int heading_y,
                    bool system) {
                    draw_text(heading, "Sans Bold 9", 10,
                              heading_y - base_shortcut_scroll,
                              places_width - 20, false, dark_text);
                    int row_y = heading_y + 26;
                    for (std::size_t entry_index = 0; entry_index < entries.size(); ++entry_index) {
                        const auto& entry = entries[entry_index];
                        const int visible_y = row_y - base_shortcut_scroll;
                        const std::size_t absolute_index = system
                            ? popup.settings.places.size() + entry_index : entry_index;
                        if (visible_y + 25 >= 0 && visible_y < list_bottom &&
                            base_focus == 4 && absolute_index == base_shortcut) {
                            cairo_set_source_rgb(cr, selected_fill.red, selected_fill.green,
                                                 selected_fill.blue);
                            cairo_rectangle(cr, 4, visible_y, places_width - 8, 25);
                            cairo_fill(cr);
                        }
                        if (visible_y + 25 >= 0 && visible_y < list_bottom) {
                            paint_theme_icon(cr, entry.icon_name, 18, visible_y + 12.5, 16);
                            draw_text(entry.label, "Sans 9", 31, visible_y,
                                      places_width - 38, false, item_text);
                        }
                        row_y += 27;
                    }
                    return row_y;
                };
                const int next_heading_y = draw_shortcuts(
                    "Places", popup.settings.places, 8, false);
                if (!popup.settings.system.empty()) {
                    draw_shortcuts("System", popup.settings.system,
                                   next_heading_y + 5, true);
                }
                cairo_restore(cr);
            }

            const int footer_y = popup.height - footer_height;
            cairo_set_source_rgb(cr, 238.0 / 255.0, 238.0 / 255.0, 238.0 / 255.0);
            if (popup.settings.show_categories) {
                cairo_rectangle(cr, category_x, footer_y, category_width, footer_height);
                cairo_fill(cr);
                cairo_rectangle(cr, applications_x, footer_y,
                                popup.width - applications_x, footer_height);
                cairo_fill(cr);
                cairo_set_source_rgb(cr, 111.0 / 255.0, 157.0 / 255.0, 46.0 / 255.0);
                cairo_rectangle(cr, category_x + 5, footer_y + 4,
                                category_width - 10, footer_height - 8);
                cairo_fill(cr);
                if (base_focus == 3) {
                    cairo_set_source_rgb(cr, 32.0 / 255.0, 53.0 / 255.0, 5.0 / 255.0);
                    cairo_rectangle(cr, category_x + 5.5, footer_y + 4.5,
                                    category_width - 11, footer_height - 9);
                    cairo_stroke(cr);
                }
                paint_theme_icon(cr, "system-shutdown-symbolic", category_x + 20,
                                 footer_y + footer_height / 2.0, 16);
                draw_text("Power", "Sans Bold 9", category_x + 31,
                          footer_y + (footer_height - 24) / 2.0,
                          category_width - 44, false, {1.0, 1.0, 1.0});
            } else {
                cairo_rectangle(cr, applications_x, footer_y,
                                popup.width - applications_x, footer_height);
                cairo_fill(cr);
                const int power_x = applications_x + 5;
                const int power_width = popup.width - applications_x - 10;
                cairo_set_source_rgb(cr, 111.0 / 255.0, 157.0 / 255.0, 46.0 / 255.0);
                cairo_rectangle(cr, power_x, footer_y + 3, power_width, 27);
                cairo_fill(cr);
                if (base_focus == 3) {
                    cairo_set_source_rgb(cr, 32.0 / 255.0, 53.0 / 255.0, 5.0 / 255.0);
                    cairo_rectangle(cr, power_x + 0.5, footer_y + 3.5,
                                    power_width - 1, 26);
                    cairo_stroke(cr);
                }
                paint_theme_icon(cr, "system-shutdown-symbolic", power_x + 16,
                                 footer_y + 16.5, 16);
                draw_text("Power", "Sans Bold 9", power_x + 28,
                          footer_y + 3 + (27 - 24) / 2.0,
                          power_width - 36, false, {1.0, 1.0, 1.0});
            }

            const int search_x = applications_x + 7;
            constexpr int search_box_height = 32;
            const int search_y = popup.settings.show_categories ? footer_y + 5 : footer_y + 34;
            const int search_width = popup.width - applications_x - 14;
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            cairo_rectangle(cr, search_x, search_y, search_width, search_box_height);
            cairo_fill(cr);
            const RgbColor search_border = base_focus == 0
                ? RgbColor{118.0 / 255.0, 159.0 / 255.0, 49.0 / 255.0}
                : RgbColor{181.0 / 255.0, 181.0 / 255.0, 181.0 / 255.0};
            cairo_set_source_rgb(cr, search_border.red, search_border.green, search_border.blue);
            cairo_rectangle(cr, search_x + 0.5, search_y + 0.5,
                            search_width - 1, search_box_height - 1);
            cairo_stroke(cr);
            paint_theme_icon(cr, "system-search-symbolic", search_x + 14,
                             search_y + search_box_height / 2.0, 16);
            const std::string search_text = popup.query.empty()
                ? "Search applications..." : popup.query;
            const RgbColor search_text_color = popup.query.empty()
                ? RgbColor{0.45, 0.45, 0.45}
                : RgbColor{32.0 / 255.0, 32.0 / 255.0, 32.0 / 255.0};
            draw_text(search_text, "Sans 9", search_x + 28,
                      search_y + (search_box_height - 24) / 2,
                      search_width - 35, false, search_text_color);
            if (base_focus == 0) {
                PangoLayout* layout = pango_cairo_create_layout(cr);
                pango_layout_set_text(layout, search_text.c_str(), -1);
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
            if (popup.power_menu) {
                static const std::array<std::pair<const char*, const char*>, 6> actions = {{
                    {"Lock Screen", "system-lock-screen-symbolic"},
                    {"Log Out", "system-log-out-symbolic"},
                    {"Suspend", "media-playback-pause-symbolic"},
                    {"Hibernate", "weather-clear-night-symbolic"},
                    {"Restart", "system-reboot-symbolic"},
                    {"Power Off", "system-shutdown-symbolic"}
                }};
                constexpr int power_width = 190;
                constexpr int power_height = 176;
                const int visible_category_width = popup.settings.show_categories
                    ? popup.settings.categories_width : 0;
                const int places_width = popup.settings.show_places
                    ? popup.settings.places_width : 0;
                const int category_x = places_width;
                const int applications_x = category_x + visible_category_width;
                const int power_x = popup.settings.show_categories
                    ? category_x + 5 : applications_x + 5;
                const int power_y = popup.height - 47 - power_height;
                cairo_save(cr);
                cairo_translate(cr, power_x, power_y);
                cairo_set_source_rgb(cr, 245.0 / 255.0, 245.0 / 255.0,
                                     245.0 / 255.0);
                cairo_rectangle(cr, 0, 0, power_width, power_height);
                cairo_fill(cr);
                for (std::size_t index = 0; index < actions.size(); ++index) {
                    const int y = 4 + static_cast<int>(index) * 28;
                    if (static_cast<int>(index) == popup.selected) {
                        if (index >= 4) {
                            cairo_set_source_rgb(cr, 239.0 / 255.0, 212.0 / 255.0,
                                                 209.0 / 255.0);
                        } else {
                            cairo_set_source_rgb(cr, 224.0 / 255.0, 233.0 / 255.0,
                                                 210.0 / 255.0);
                        }
                        cairo_rectangle(cr, 4, y, power_width - 8, 26);
                        cairo_fill(cr);
                    }
                    paint_theme_icon(cr, actions[index].second, 18, y + 13, 16);
                    draw_menu_text(cr, actions[index].first, 34, y + 5,
                        power_width - 40, "Sans 9",
                        {37.0 / 255.0, 37.0 / 255.0, 37.0 / 255.0});
                }
                cairo_set_source_rgb(cr, 119.0 / 255.0, 119.0 / 255.0,
                                     119.0 / 255.0);
                cairo_rectangle(cr, 0.5, 0.5, power_width - 1.0, power_height - 1.0);
                cairo_stroke(cr);
                cairo_restore(cr);
            }
        }

        cairo_set_source_rgb(cr, 119.0 / 255.0, 119.0 / 255.0, 119.0 / 255.0);
        cairo_rectangle(cr, 0.5, 0.5, width - 1.0, height - 1.0);
        cairo_stroke(cr);
        if (redraw_column != 0) cairo_restore(cr);
        cairo_surface_flush(buffer.cairo_surface);
        wl_surface_attach(popup.surface, buffer.buffer, 0, 0);
        wl_surface_damage(popup.surface, damage_x, damage_y, damage_width, damage_height);
        buffer.busy = true;
        popup.last_buffer_index = static_cast<int>(buffer_index);
        wl_surface_commit(popup.surface);
        wl_display_flush(display_);
    }

    void handle_menu_popup_click(int x, int y, int button)
    {
        if (button != 1 || menu_popup_ == nullptr) return;
        MenuPopup& popup = *menu_popup_;
        if (popup.power_menu) {
            int power_y = 0;
            int action_y = y;
            if (power_menu_return_state_ != nullptr) {
                constexpr int power_height = 176;
                const int visible_category_width = popup.settings.show_categories
                    ? popup.settings.categories_width : 0;
                const int places_width = popup.settings.show_places
                    ? popup.settings.places_width : 0;
                const int category_x = places_width;
                const int applications_x = category_x + visible_category_width;
                const int power_x = popup.settings.show_categories
                    ? category_x + 5 : applications_x + 5;
                constexpr int power_width = 190;
                power_y = popup.height - 47 - power_height;
                if (x < power_x || x >= power_x + power_width ||
                    y < power_y || y >= power_y + power_height) {
                    close_power_menu();
                    handle_menu_popup_click(x, y, button);
                    return;
                }
                action_y -= power_y;
            }
            static const std::vector<std::pair<std::string, std::string>> actions = {
                {"lock", "Lock Screen"}, {"logout", "Log Out"},
                {"suspend", "Suspend"}, {"hibernate", "Hibernate"},
                {"reboot", "Restart"}, {"shutdown", "Power Off"}
            };
            const int selected = action_y >= 4 ? (action_y - 4) / 28 : -1;
            if (selected >= 0 && static_cast<std::size_t>(selected) < actions.size() &&
                popup.on_power_action) {
                const auto action = actions[static_cast<std::size_t>(selected)];
                auto callback = popup.on_power_action;
                close_power_menu();
                callback(action.first, action.second);
            }
            return;
        }
        const int visible_category_width = popup.settings.show_categories
            ? popup.settings.categories_width : 0;
        const int footer_height = popup.settings.show_categories ? 42 : 78;
        const int footer_y = popup.height - footer_height;
        const int places_width = popup.settings.show_places
            ? popup.settings.places_width : 0;
        const int category_x = places_width;
        const int applications_x = category_x + visible_category_width;
        if (y >= 8 && y < footer_y) {
            int column = 0;
            int track_y = 8;
            int track_height = footer_y - 8;
            std::size_t content_count = popup.items.size();
            std::size_t visible_count = static_cast<std::size_t>(std::max(1,
                (footer_y - 8) / 28));
            std::size_t scroll = popup.scroll;
            if (popup.settings.show_categories &&
                x >= category_x + popup.settings.categories_width - 19 &&
                x < applications_x && y >= 34) {
                column = 1;
                track_y = 34;
                track_height = footer_y - track_y;
                content_count = popup.categories.size();
                visible_count = static_cast<std::size_t>(std::max(1, track_height / 27));
                scroll = popup.category_scroll;
            } else if (x >= popup.width - 19) {
                column = 2;
            }
            if (column != 0 && track_height > 0 && content_count > visible_count) {
                const int thumb_height = std::min(track_height, std::max(22,
                    static_cast<int>(static_cast<std::size_t>(track_height) * visible_count /
                                     content_count)));
                const std::size_t maximum = content_count - visible_count;
                const int thumb_y = track_y + static_cast<int>(
                    static_cast<std::size_t>(track_height - thumb_height) * scroll / maximum);
                popup.scrollbar_drag_column = column;
                popup.scrollbar_drag_offset = y >= thumb_y && y < thumb_y + thumb_height
                    ? y - thumb_y : thumb_height / 2;
                update_menu_scrollbar_drag(y);
                return;
            }
        }
        if (y >= footer_y) {
            if (popup.settings.show_categories &&
                x >= category_x && x < applications_x) {
                toggle_application_power_menu(popup.anchor_x, popup.anchor_width,
                                               popup.on_power_action);
            } else if (x >= applications_x) {
                if (!popup.settings.show_categories && y < footer_y + 34) {
                    toggle_application_power_menu(popup.anchor_x, popup.anchor_width,
                                                   popup.on_power_action);
                } else {
                    popup.focus = 0;
                }
                render_menu_popup();
            }
            return;
        }
        if (places_width > 0 && x < places_width) {
            const int content_y = y + popup.shortcut_scroll;
            const int system_first_y = 65 +
                static_cast<int>(popup.settings.places.size()) * 27;
            const auto activate_entry = [&](const std::vector<ApplicationMenuDisplayItem>& entries,
                                            int first_y, bool system) {
                const int row = (content_y - first_y) / 27;
                const int row_y = first_y + row * 27;
                if (content_y >= first_y && row >= 0 && content_y < row_y + 25 &&
                    static_cast<std::size_t>(row) < entries.size()) {
                    popup.focus = 4;
                    popup.selected_shortcut = system
                        ? popup.settings.places.size() + static_cast<std::size_t>(row)
                        : static_cast<std::size_t>(row);
                    if (popup.on_shortcut) {
                        popup.on_shortcut(system, static_cast<std::size_t>(row));
                    }
                    return true;
                }
                return false;
            };
            if (!activate_entry(popup.settings.places, 34, false)) {
                activate_entry(popup.settings.system, system_first_y, true);
            }
            render_menu_popup();
            return;
        }
        if (popup.settings.show_categories &&
            x >= category_x && x < applications_x && y >= 34) {
            popup.focus = 1;
            const int local_index = (y - 34) / 27;
            const int visible_rows = std::max(1, (footer_y - 34) / 27);
            if (local_index >= visible_rows) return;
            const int selected = static_cast<int>(popup.category_scroll) + local_index;
            if (selected >= 0 && static_cast<std::size_t>(selected) < popup.categories.size() &&
                popup.on_category) {
                popup.category = popup.categories[static_cast<std::size_t>(selected)];
                popup.on_category(popup.category);
            }
            return;
        }
        if (x >= applications_x && y >= 8) {
            popup.focus = 2;
            const int local_row = (y - 8) / 28;
            const int visible_rows = std::max(1, (footer_y - 8) / 28);
            if (local_row >= visible_rows) return;
            const int selected = static_cast<int>(popup.scroll) + local_row;
            if (selected >= 0 && static_cast<std::size_t>(selected) < popup.items.size() &&
                popup.on_activate) {
                popup.on_activate(static_cast<std::size_t>(selected));
            }
        }
    }

    void close_calendar_popup()
    {
        if (calendar_popup_ == nullptr) {
            return;
        }
        if (calendar_popup_->surface != nullptr) {
            wl_surface_attach(calendar_popup_->surface, nullptr, 0, 0);
            wl_surface_commit(calendar_popup_->surface);
        }
        if (calendar_popup_->popup != nullptr) {
            xdg_popup_destroy(calendar_popup_->popup);
        }
        if (calendar_popup_->surface_role != nullptr) {
            xdg_surface_destroy(calendar_popup_->surface_role);
        }
        if (calendar_popup_->surface != nullptr) {
            wl_surface_destroy(calendar_popup_->surface);
        }
        set_panel_keyboard_interactivity(ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        wl_display_flush(display_);
        wl_display_roundtrip(display_);
        calendar_popup_.reset();
    }

    void create_calendar_popup(int anchor_x, int anchor_width)
    {
        if (calendar_popup_ != nullptr) {
            close_calendar_popup();
        }
        set_panel_keyboard_interactivity(
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
        calendar_popup_ = std::make_unique<CalendarPopup>();
        calendar_popup_->owner = this;
        calendar_popup_->anchor_x = anchor_x;
        calendar_popup_->anchor_width = anchor_width;
        time_t now = time(nullptr);
        tm local_time{};
        localtime_r(&now, &local_time);
        calendar_popup_->year = local_time.tm_year + 1900;
        calendar_popup_->month = local_time.tm_mon + 1;

        calendar_popup_->surface = wl_compositor_create_surface(compositor_);
        if (calendar_popup_->surface == nullptr) {
            calendar_popup_.reset();
            return;
        }
        calendar_popup_->surface_role = xdg_wm_base_get_xdg_surface(wm_base_, calendar_popup_->surface);
        static const xdg_surface_listener surface_listener = {calendar_popup_surface_configure};
        xdg_surface_add_listener(calendar_popup_->surface_role, &surface_listener, calendar_popup_.get());

        xdg_positioner* positioner = xdg_wm_base_create_positioner(wm_base_);
        configure_panel_positioner(positioner, calendar_popup_->anchor_x,
            calendar_popup_->anchor_width, calendar_popup_->width, calendar_popup_->height);
        calendar_popup_->popup = xdg_surface_get_popup(calendar_popup_->surface_role,
            popup_parent_surface(), positioner);
        xdg_positioner_destroy(positioner);
        if (calendar_popup_->popup == nullptr) {
            close_calendar_popup();
            return;
        }
        static const xdg_popup_listener popup_listener = {
            .configure = calendar_popup_configure,
            .popup_done = calendar_popup_done,
            .repositioned = popup_repositioned
        };
        xdg_popup_add_listener(calendar_popup_->popup, &popup_listener, calendar_popup_.get());
        attach_popup_to_panel(calendar_popup_->popup);
        if (seat_ != nullptr && pointer_serial_ != 0) {
            xdg_popup_grab(calendar_popup_->popup, seat_, pointer_serial_);
            pointer_serial_ = 0;
        }
        wl_surface_commit(calendar_popup_->surface);
        wl_display_flush(display_);
    }

    void render_calendar_popup()
    {
        if (calendar_popup_ == nullptr || !calendar_popup_->configured) {
            return;
        }
        const std::size_t buffer_index = find_free_buffer(calendar_popup_->buffers);
        if (buffer_index == calendar_popup_->buffers.size()) {
            if (!create_buffer_set(calendar_popup_->buffers, calendar_popup_->width, calendar_popup_->height)) {
                return;
            }
        }
        Buffer& buffer = *calendar_popup_->buffers[buffer_index];
        cairo_t* cr = buffer.cairo;
        cairo_set_source_rgb(cr, 245.0 / 255.0, 245.0 / 255.0, 245.0 / 255.0);
        cairo_paint(cr);

        tm month_date{};
        month_date.tm_year = calendar_popup_->year - 1900;
        month_date.tm_mon = calendar_popup_->month - 1;
        month_date.tm_mday = 1;
        mktime(&month_date);
        char month_title[64]{};
        strftime(month_title, sizeof(month_title), "%B %Y", &month_date);

        cairo_set_source_rgb(cr, 37.0 / 255.0, 37.0 / 255.0, 37.0 / 255.0);
        cairo_move_to(cr, 26.0, 18.0);
        PangoLayout* title_layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(title_layout, month_title, -1);
        PangoFontDescription* title_font = pango_font_description_from_string("Sans Bold 10");
        pango_layout_set_font_description(title_layout, title_font);
        pango_font_description_free(title_font);
        pango_cairo_show_layout(cr, title_layout);
        g_object_unref(title_layout);

        cairo_set_source_rgb(cr, 90.0 / 255.0, 90.0 / 255.0, 90.0 / 255.0);
        cairo_move_to(cr, 18.0, 20.0);
        cairo_show_text(cr, "<");
        cairo_move_to(cr, calendar_popup_->width - 28.0, 20.0);
        cairo_show_text(cr, ">");

        constexpr std::array<const char*, 7> weekdays = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
        constexpr double grid_left = 12.0;
        constexpr double cell_width = 30.0;
        constexpr double grid_top = 48.0;
        constexpr double cell_height = 30.0;
        for (std::size_t column = 0; column < weekdays.size(); ++column) {
            PangoLayout* layout = pango_cairo_create_layout(cr);
            pango_layout_set_text(layout, weekdays[column], -1);
            PangoFontDescription* font = pango_font_description_from_string("Sans 8");
            pango_layout_set_font_description(layout, font);
            pango_font_description_free(font);
            int width_px = 0;
            int height_px = 0;
            pango_layout_get_pixel_size(layout, &width_px, &height_px);
            cairo_set_source_rgb(cr, 110.0 / 255.0, 110.0 / 255.0, 110.0 / 255.0);
            cairo_move_to(cr, grid_left + column * cell_width + (cell_width - width_px) / 2.0,
                          grid_top - height_px / 2.0 + 2.0);
            pango_cairo_show_layout(cr, layout);
            g_object_unref(layout);
        }

        tm first_day{};
        first_day.tm_year = calendar_popup_->year - 1900;
        first_day.tm_mon = calendar_popup_->month - 1;
        first_day.tm_mday = 1;
        mktime(&first_day);
        const int first_column = (first_day.tm_wday + 6) % 7;

        tm last_day{};
        last_day.tm_year = calendar_popup_->year - 1900;
        last_day.tm_mon = calendar_popup_->month;
        last_day.tm_mday = 0;
        mktime(&last_day);

        const time_t now = time(nullptr);
        tm today{};
        localtime_r(&now, &today);

        for (int day = 1; day <= last_day.tm_mday; ++day) {
            const int cell = first_column + day - 1;
            const int row = cell / 7;
            const int column = cell % 7;
            const double center_x = grid_left + (column + 0.5) * cell_width;
            const double top_y = grid_top + 16.0 + row * cell_height;
            const bool is_today = calendar_popup_->year == today.tm_year + 1900 &&
                calendar_popup_->month == today.tm_mon + 1 && day == today.tm_mday;
            if (is_today) {
                cairo_set_source_rgb(cr, 93.0 / 255.0, 128.0 / 255.0, 38.0 / 255.0);
                cairo_arc(cr, center_x, top_y + 8.0, 11.0, 0.0, 2.0 * 3.14159265358979323846);
                cairo_fill(cr);
            }
            PangoLayout* day_layout = pango_cairo_create_layout(cr);
            const std::string day_text = std::to_string(day);
            pango_layout_set_text(day_layout, day_text.c_str(), -1);
            PangoFontDescription* day_font = pango_font_description_from_string("Sans 8");
            pango_layout_set_font_description(day_layout, day_font);
            pango_font_description_free(day_font);
            int day_w = 0;
            int day_h = 0;
            pango_layout_get_pixel_size(day_layout, &day_w, &day_h);
            cairo_set_source_rgb(cr, is_today ? 1.0 : 37.0 / 255.0,
                                 is_today ? 1.0 : 37.0 / 255.0,
                                 is_today ? 1.0 : 37.0 / 255.0);
            cairo_move_to(cr, center_x - day_w / 2.0, top_y - day_h / 2.0 + 3.0);
            pango_cairo_show_layout(cr, day_layout);
            g_object_unref(day_layout);
        }

        cairo_set_source_rgb(cr, 119.0 / 255.0, 119.0 / 255.0, 119.0 / 255.0);
        cairo_rectangle(cr, 0.5, 0.5, calendar_popup_->width - 1.0, calendar_popup_->height - 1.0);
        cairo_stroke(cr);
        cairo_surface_flush(buffer.cairo_surface);
        wl_surface_attach(calendar_popup_->surface, buffer.buffer, 0, 0);
        wl_surface_damage(calendar_popup_->surface, 0, 0, calendar_popup_->width, calendar_popup_->height);
        buffer.busy = true;
        wl_surface_commit(calendar_popup_->surface);
        wl_display_flush(display_);
    }

    void handle_calendar_key(xkb_keysym_t symbol)
    {
        if (calendar_popup_ == nullptr) {
            return;
        }
        if (symbol == XKB_KEY_Escape) {
            close_calendar_popup();
            return;
        }
        if (symbol == XKB_KEY_Left) {
            calendar_popup_->month -= 1;
            if (calendar_popup_->month < 1) {
                calendar_popup_->month = 12;
                --calendar_popup_->year;
            }
            render_calendar_popup();
        } else if (symbol == XKB_KEY_Right) {
            calendar_popup_->month += 1;
            if (calendar_popup_->month > 12) {
                calendar_popup_->month = 1;
                ++calendar_popup_->year;
            }
            render_calendar_popup();
        }
    }

    void handle_calendar_click(int x, int y)
    {
        if (calendar_popup_ == nullptr) {
            return;
        }
        if (y < 42 && x < 52) {
            calendar_popup_->month -= 1;
            if (calendar_popup_->month < 1) {
                calendar_popup_->month = 12;
                --calendar_popup_->year;
            }
        } else if (y < 42 && x >= calendar_popup_->width - 52) {
            calendar_popup_->month += 1;
            if (calendar_popup_->month > 12) {
                calendar_popup_->month = 1;
                ++calendar_popup_->year;
            }
        }
        render_calendar_popup();
    }

    std::size_t find_free_buffer(std::vector<std::unique_ptr<Buffer>>& target)
    {
        for (std::size_t index = 0; index < target.size(); ++index) {
            if (!target[index]->busy) {
                return index;
            }
        }
        return target.size();
    }

    void close_desktop_hub_popup()
    {
        if (desktop_hub_popup_ == nullptr) {
            return;
        }
        if (desktop_hub_popup_->surface != nullptr) {
            wl_surface_attach(desktop_hub_popup_->surface, nullptr, 0, 0);
            wl_surface_commit(desktop_hub_popup_->surface);
        }
        if (desktop_hub_popup_->popup != nullptr) {
            xdg_popup_destroy(desktop_hub_popup_->popup);
        }
        if (desktop_hub_popup_->surface_role != nullptr) {
            xdg_surface_destroy(desktop_hub_popup_->surface_role);
        }
        if (desktop_hub_popup_->surface != nullptr) {
            wl_surface_destroy(desktop_hub_popup_->surface);
        }
        std::function<void()> on_close = std::move(desktop_hub_popup_->on_close);
        set_panel_keyboard_interactivity(ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        wl_display_flush(display_);
        wl_display_roundtrip(display_);
        desktop_hub_leave_pending_ = false;
        desktop_hub_focus_leave_pending_ = false;
        desktop_hub_popup_.reset();
        if (on_close) {
            on_close();
        }
    }

    void create_desktop_hub_popup(int anchor_x, int anchor_width,
        std::string icon_name, std::vector<HubDisplayItem> items,
        std::function<void(const std::string&)> on_search,
        std::function<void(std::size_t)> on_activate,
        std::function<void()> on_close)
    {
        if (desktop_hub_popup_ != nullptr) {
            close_desktop_hub_popup();
        }
        desktop_hub_popup_ = std::make_unique<DesktopHubPopup>();
        desktop_hub_popup_->owner = this;
        desktop_hub_popup_->anchor_x = anchor_x;
        desktop_hub_popup_->anchor_width = anchor_width;
        desktop_hub_popup_->icon_name = std::move(icon_name);
        desktop_hub_popup_->items = std::move(items);
        desktop_hub_popup_->on_search = std::move(on_search);
        desktop_hub_popup_->on_activate = std::move(on_activate);
        desktop_hub_popup_->on_close = std::move(on_close);
        desktop_hub_popup_->surface = wl_compositor_create_surface(compositor_);
        if (desktop_hub_popup_->surface == nullptr) {
            desktop_hub_popup_.reset();
            return;
        }
        desktop_hub_popup_->surface_role = xdg_wm_base_get_xdg_surface(wm_base_, desktop_hub_popup_->surface);
        static const xdg_surface_listener surface_listener = {desktop_hub_popup_surface_configure};
        xdg_surface_add_listener(desktop_hub_popup_->surface_role, &surface_listener, desktop_hub_popup_.get());

        xdg_positioner* positioner = xdg_wm_base_create_positioner(wm_base_);
        configure_panel_positioner(positioner, desktop_hub_popup_->anchor_x,
            desktop_hub_popup_->anchor_width, desktop_hub_popup_->width,
            desktop_hub_popup_->height);
        desktop_hub_popup_->popup = xdg_surface_get_popup(desktop_hub_popup_->surface_role,
            popup_parent_surface(), positioner);
        xdg_positioner_destroy(positioner);
        if (desktop_hub_popup_->popup == nullptr) {
            close_desktop_hub_popup();
            return;
        }
        static const xdg_popup_listener popup_listener = {
            .configure = desktop_hub_popup_configure,
            .popup_done = desktop_hub_popup_done,
            .repositioned = popup_repositioned
        };
        xdg_popup_add_listener(desktop_hub_popup_->popup, &popup_listener, desktop_hub_popup_.get());
        attach_popup_to_panel(desktop_hub_popup_->popup);
        wl_surface_commit(desktop_hub_popup_->surface);
        wl_display_flush(display_);
        set_panel_keyboard_interactivity(
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    }

    void update_desktop_hub_scroll()
    {
        if (desktop_hub_popup_ == nullptr) {
            return;
        }
        constexpr std::size_t visible_rows = 11;
        if (desktop_hub_popup_->selected < desktop_hub_popup_->scroll) {
        if (seat_ != nullptr && pointer_serial_ != 0) {
            xdg_popup_grab(desktop_hub_popup_->popup, seat_, pointer_serial_);
            pointer_serial_ = 0;
        }
            desktop_hub_popup_->scroll = desktop_hub_popup_->selected;
        } else if (desktop_hub_popup_->selected >= desktop_hub_popup_->scroll + visible_rows) {
            desktop_hub_popup_->scroll = desktop_hub_popup_->selected - visible_rows + 1;
        }
    }

    void update_desktop_hub_hover(int x, int y)
    {
        if (desktop_hub_popup_ == nullptr || y < 72 || x < 18 || x > desktop_hub_popup_->width - 20) {
            return;
        }
        const int row = (y - 72) / static_cast<int>(desktop_hub_popup_->row_height);
        if (row >= 0) {
            const std::size_t selected = desktop_hub_popup_->scroll + static_cast<std::size_t>(row);
            if (selected < desktop_hub_popup_->items.size()) {
                desktop_hub_popup_->selected = selected;
                render_desktop_hub_popup();
            }
        }
    }

    void render_desktop_hub_popup()
    {
        if (desktop_hub_popup_ == nullptr || !desktop_hub_popup_->configured) {
            return;
        }
        const std::size_t buffer_index = find_free_buffer(desktop_hub_popup_->buffers);
        if (buffer_index == desktop_hub_popup_->buffers.size()) {
            if (!create_buffer_set(desktop_hub_popup_->buffers, desktop_hub_popup_->width, desktop_hub_popup_->height)) {
                return;
            }
        }
        Buffer& buffer = *desktop_hub_popup_->buffers[buffer_index];
        cairo_t* cr = buffer.cairo;
        cairo_set_source_rgb(cr, 245.0 / 255.0, 245.0 / 255.0, 245.0 / 255.0);
        cairo_paint(cr);

        cairo_set_source_rgb(cr, 119.0 / 255.0, 119.0 / 255.0, 119.0 / 255.0);
        cairo_rectangle(cr, 0.5, 0.5, desktop_hub_popup_->width - 1.0, desktop_hub_popup_->height - 1.0);
        cairo_stroke(cr);

        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_rectangle(cr, 30.0, 24.0, desktop_hub_popup_->width - 60.0, 34.0);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 180.0 / 255.0, 180.0 / 255.0, 180.0 / 255.0);
        cairo_rectangle(cr, 30.5, 24.5, desktop_hub_popup_->width - 61.0, 33.0);
        cairo_stroke(cr);

        paint_theme_icon(cr, "system-search-symbolic", 42.0, 41.0, 18);
        const std::string search_text = desktop_hub_popup_->query.empty()
            ? "Type app, folder, file, *.deb, or command..." : desktop_hub_popup_->query;
        cairo_set_source_rgb(cr, desktop_hub_popup_->query.empty() ? 0.45 : 32.0 / 255.0,
                             desktop_hub_popup_->query.empty() ? 0.45 : 32.0 / 255.0,
                             desktop_hub_popup_->query.empty() ? 0.45 : 32.0 / 255.0);
        PangoLayout* search_layout = pango_cairo_create_layout(cr);
        pango_layout_set_text(search_layout, search_text.c_str(), -1);
        PangoFontDescription* search_font = pango_font_description_from_string("Sans 10");
        pango_layout_set_font_description(search_layout, search_font);
        pango_font_description_free(search_font);
        cairo_move_to(cr, 62.0, 31.0);
        pango_cairo_show_layout(cr, search_layout);
        g_object_unref(search_layout);

        constexpr std::size_t visible_rows = 11;
        const std::size_t visible_count = std::min<std::size_t>(visible_rows,
            std::max<std::size_t>(1, desktop_hub_popup_->items.size() - desktop_hub_popup_->scroll));
        for (std::size_t row = 0; row < visible_count; ++row) {
            const std::size_t index = desktop_hub_popup_->scroll + row;
            if (index >= desktop_hub_popup_->items.size()) {
                break;
            }
            const int y = 72 + static_cast<int>(row) * static_cast<int>(desktop_hub_popup_->row_height);
            if (index == desktop_hub_popup_->selected) {
                cairo_set_source_rgb(cr, 220.0 / 255.0, 233.0 / 255.0, 203.0 / 255.0);
                cairo_rectangle(cr, 30.0, y, desktop_hub_popup_->width - 60.0, desktop_hub_popup_->row_height);
                cairo_fill(cr);
            }
            const std::string& icon_name = desktop_hub_popup_->items[index].icon_name;
            paint_theme_icon(cr, icon_name, 42.0, y + 15.0, 18);
            PangoLayout* item_layout = pango_cairo_create_layout(cr);
            pango_layout_set_text(item_layout, desktop_hub_popup_->items[index].label.c_str(), -1);
            pango_layout_set_width(item_layout, (desktop_hub_popup_->width - 120) * PANGO_SCALE);
            pango_layout_set_ellipsize(item_layout, PANGO_ELLIPSIZE_END);
            PangoFontDescription* item_font = pango_font_description_from_string("Sans 10");
            pango_layout_set_font_description(item_layout, item_font);
            pango_font_description_free(item_font);
            cairo_set_source_rgb(cr, index == desktop_hub_popup_->selected ? 23.0 / 255.0 : 48.0 / 255.0,
                                 index == desktop_hub_popup_->selected ? 23.0 / 255.0 : 48.0 / 255.0,
                                 index == desktop_hub_popup_->selected ? 23.0 / 255.0 : 48.0 / 255.0);
            cairo_move_to(cr, 62.0, y + 7.0);
            pango_cairo_show_layout(cr, item_layout);
            g_object_unref(item_layout);
        }

        if (desktop_hub_popup_->items.size() > visible_rows) {
            const double track_y = 72.0;
            const double track_height = static_cast<double>(visible_rows) * static_cast<double>(desktop_hub_popup_->row_height);
            const double thumb_height = std::max(24.0, track_height * visible_rows /
                static_cast<double>(desktop_hub_popup_->items.size()));
            const std::size_t max_scroll = desktop_hub_popup_->items.size() - visible_rows;
            const double scroll_ratio = max_scroll == 0 ? 0.0 :
                static_cast<double>(desktop_hub_popup_->scroll) / static_cast<double>(max_scroll);
            const double thumb_y = track_y + scroll_ratio * (track_height - thumb_height);
            cairo_set_source_rgb(cr, 238.0 / 255.0, 238.0 / 255.0, 238.0 / 255.0);
            cairo_rectangle(cr, desktop_hub_popup_->width - 28.0, track_y, 8.0, track_height);
            cairo_fill(cr);
            cairo_set_source_rgb(cr, 181.0 / 255.0, 181.0 / 255.0, 181.0 / 255.0);
            cairo_rectangle(cr, desktop_hub_popup_->width - 28.0, thumb_y, 8.0, thumb_height);
            cairo_fill(cr);
        }

        cairo_surface_flush(buffer.cairo_surface);
        wl_surface_attach(desktop_hub_popup_->surface, buffer.buffer, 0, 0);
        wl_surface_damage(desktop_hub_popup_->surface, 0, 0, desktop_hub_popup_->width, desktop_hub_popup_->height);
        buffer.busy = true;
        wl_surface_commit(desktop_hub_popup_->surface);
        wl_display_flush(display_);
    }

    void handle_desktop_hub_key(xkb_keysym_t symbol, xkb_keycode_t keycode)
    {
        if (desktop_hub_popup_ == nullptr) {
            return;
        }
        if (symbol == XKB_KEY_Escape) {
            return close_desktop_hub_popup();
        }
        if (symbol == XKB_KEY_Up || symbol == XKB_KEY_Page_Up) {
            if (desktop_hub_popup_->selected > 0) {
                const std::size_t step = symbol == XKB_KEY_Page_Up ? 11 : 1;
                desktop_hub_popup_->selected = desktop_hub_popup_->selected > step
                    ? desktop_hub_popup_->selected - step : 0;
                update_desktop_hub_scroll();
                render_desktop_hub_popup();
            }
            return;
        }
        if (symbol == XKB_KEY_Down || symbol == XKB_KEY_Page_Down) {
            if (desktop_hub_popup_->selected + 1 < desktop_hub_popup_->items.size()) {
                const std::size_t step = symbol == XKB_KEY_Page_Down ? 11 : 1;
                desktop_hub_popup_->selected = std::min(
                    desktop_hub_popup_->selected + step,
                    desktop_hub_popup_->items.size() - 1);
                update_desktop_hub_scroll();
                render_desktop_hub_popup();
            }
            return;
        }
        if (symbol == XKB_KEY_Home || symbol == XKB_KEY_End) {
            if (!desktop_hub_popup_->items.empty()) {
                desktop_hub_popup_->selected = symbol == XKB_KEY_Home
                    ? 0 : desktop_hub_popup_->items.size() - 1;
                update_desktop_hub_scroll();
                render_desktop_hub_popup();
            }
            return;
        }
        if (symbol == XKB_KEY_Return || symbol == XKB_KEY_KP_Enter) {
            if (desktop_hub_popup_->on_activate &&
                desktop_hub_popup_->selected < desktop_hub_popup_->items.size()) {
                desktop_hub_popup_->on_activate(desktop_hub_popup_->selected);
            }
            return;
        }
        if (symbol == XKB_KEY_BackSpace) {
            if (!desktop_hub_popup_->query.empty()) {
                do {
                    const unsigned char character = static_cast<unsigned char>(
                        desktop_hub_popup_->query.back());
                    desktop_hub_popup_->query.pop_back();
                    if ((character & 0xc0) != 0x80) break;
                } while (!desktop_hub_popup_->query.empty());
                if (desktop_hub_popup_->on_search) {
                    desktop_hub_popup_->on_search(desktop_hub_popup_->query);
                }
                render_desktop_hub_popup();
            }
            return;
        }
        if (xkb_state_mod_name_is_active(xkb_state_, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) ||
            xkb_state_mod_name_is_active(xkb_state_, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE)) {
            return;
        }
        char text[64]{};
        const int length = xkb_state_key_get_utf8(xkb_state_, keycode, text, sizeof(text));
        if (length > 0 && text[0] >= 0x20) {
            desktop_hub_popup_->query.append(text, static_cast<std::size_t>(length));
            if (desktop_hub_popup_->on_search) {
                desktop_hub_popup_->on_search(desktop_hub_popup_->query);
            }
            render_desktop_hub_popup();
        }
    }

    void handle_desktop_hub_click([[maybe_unused]] int x, int y)
    {
        if (desktop_hub_popup_ == nullptr) {
            return;
        }
        if (y >= 72 && y < 72 + static_cast<int>(std::min<std::size_t>(11, desktop_hub_popup_->items.size())) * static_cast<int>(desktop_hub_popup_->row_height)) {
            const std::size_t row = static_cast<std::size_t>((y - 72) / static_cast<int>(desktop_hub_popup_->row_height));
            const std::size_t index = desktop_hub_popup_->scroll + row;
            if (index < desktop_hub_popup_->items.size()) {
                desktop_hub_popup_->selected = index;
                render_desktop_hub_popup();
                if (desktop_hub_popup_->on_activate) {
                    desktop_hub_popup_->on_activate(index);
                }
            }
        }
    }

public:
    void toggle_calendar(int anchor_x, int anchor_width)
    {
        if (calendar_popup_ != nullptr) {
            close_calendar_popup();
            return;
        }
        create_calendar_popup(anchor_x, anchor_width);
        if (calendar_popup_ != nullptr) {
            render_calendar_popup();
        }
    }

    void open_desktop_hub(int anchor_x, int anchor_width, std::string icon_name,
        std::vector<HubDisplayItem> items,
        std::function<void(const std::string&)> on_search,
        std::function<void(std::size_t)> on_activate,
        std::function<void()> on_close)
    {
        if (desktop_hub_popup_ != nullptr) {
            close_desktop_hub_popup();
            return;
        }
        create_desktop_hub_popup(anchor_x, anchor_width, std::move(icon_name),
            std::move(items), std::move(on_search), std::move(on_activate), std::move(on_close));
        if (desktop_hub_popup_ != nullptr) {
            render_desktop_hub_popup();
        }
    }

    void update_desktop_hub(std::string query, std::vector<HubDisplayItem> items,
        std::size_t selected, bool preserve_selection)
    {
        if (desktop_hub_popup_ == nullptr) {
            return;
        }
        const std::size_t previous_selected = desktop_hub_popup_->selected;
        desktop_hub_popup_->query = std::move(query);
        desktop_hub_popup_->items = std::move(items);
        desktop_hub_popup_->selected = desktop_hub_popup_->items.empty() ? 0 :
            std::min(preserve_selection ? previous_selected : selected,
                     desktop_hub_popup_->items.size() - 1);
        update_desktop_hub_scroll();
        render_desktop_hub_popup();
    }

    void close_desktop_hub()
    {
        close_desktop_hub_popup();
    }

    bool desktop_hub_visible() const
    {
        return desktop_hub_popup_ != nullptr;
    }

    void set_desktop_hub_countdown_active(bool active)
    {
        if (desktop_hub_countdown_active_ && !active && desktop_hub_popup_ != nullptr) {
            desktop_hub_leave_pending_ = true;
        }
        desktop_hub_countdown_active_ = active;
    }

    bool supports_task_list() const
    {
        initialize_task_backend();
        if (task_backend_ != nullptr) {
            return true;
        }
        const char* desktop_value = std::getenv("XDG_CURRENT_DESKTOP");
        std::string desktop = desktop_value != nullptr ? desktop_value : "";
        std::transform(desktop.begin(), desktop.end(), desktop.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        const char* compositor = std::getenv("MDEON_SESSION_COMPOSITOR");
        if (desktop.find("gnome") != std::string::npos ||
            (compositor != nullptr && std::string(compositor) == "gnome-shell")) {
            return true;
        }
        return !SystemCommands::find_executable("wlrctl").empty() &&
            SystemCommands::run({"wlrctl", "toplevel", "list"}).exit_code == 0;
    }

    bool supports_task_context_menu() const
    {
        initialize_task_backend();
        return task_backend_ != nullptr
            ? task_backend_->supports_context_menu() : supports_task_list();
    }

    bool supports_task_action(TaskAction action) const
    {
        initialize_task_backend();
        return task_backend_ != nullptr
            ? task_backend_->supports_action(action) : supports_task_list();
    }

    std::vector<TaskWindowInfo> list_task_windows()
    {
        initialize_task_backend();
        if (task_backend_ != nullptr) {
            task_backend_->set_monitor_filter(backend_.monitor_,
                                              backend_.task_monitor_filter_);
            return task_backend_->list_windows();
        }
        return read_wlr_task_windows();
    }

    void perform_task_action(const std::string& id, TaskAction action)
    {
        initialize_task_backend();
        if (task_backend_ != nullptr) {
            task_backend_->perform_action(id, action);
        } else {
            run_wlr_task_action(id, action);
        }
    }

    void show_task_menu(const std::string& id, int panel_x, int panel_y, bool)
    {
        const auto tasks = list_task_windows();
        const auto task = std::find_if(tasks.begin(), tasks.end(), [&](const TaskWindowInfo& item) {
            return item.id == id;
        });
        if (task == tasks.end()) {
            return;
        }
        std::vector<std::pair<std::string, TaskAction>> actions;
        if (supports_task_action(TaskAction::Activate)) {
            actions.emplace_back("Activate / Restore", TaskAction::Activate);
        }
        if (supports_task_action(TaskAction::Minimize)) {
            actions.emplace_back("Minimize", TaskAction::Minimize);
        }
        if (supports_task_action(TaskAction::ToggleMaximize)) {
            actions.emplace_back(task->maximized ? "Restore" : "Maximize",
                                 TaskAction::ToggleMaximize);
        }
        if (supports_task_action(TaskAction::Close)) {
            actions.emplace_back("Close", TaskAction::Close);
        }
        std::vector<std::string> labels;
        labels.reserve(actions.size());
        for (const auto& item : actions) labels.push_back(item.first);
        open_task_menu_popup(edge_ == Edge::Left || edge_ == Edge::Right
                ? panel_y : panel_x,
            "Window actions", std::move(labels),
            [this, id, actions = std::move(actions)](std::size_t selected) {
                if (selected < actions.size()) {
                    perform_task_action(id, actions[selected].second);
                }
            });
    }

    bool supports_workspaces() const
    {
        return workspace_tool() != WorkspaceTool::None;
    }

    WorkspaceSnapshot get_workspaces()
    {
        return read_workspaces();
    }

    bool configure_workspace_count(int count)
    {
        return ensure_workspace_count(count);
    }

    bool switch_workspace(int id)
    {
        return activate_workspace(id);
    }

    bool supports_show_desktop() const
    {
        return show_desktop_tool() != ShowDesktopTool::None;
    }

    bool get_show_desktop_state(bool& showing)
    {
        const ShowDesktopTool tool = show_desktop_tool();
        if (tool == ShowDesktopTool::None) {
            return false;
        }
        if (tool == ShowDesktopTool::Gnome) {
            initialize_task_backend();
            return task_backend_ != nullptr && task_backend_->get_show_desktop_state(showing);
        }
        if (tool == ShowDesktopTool::Kde) return kwin_show_desktop_state(showing);
        showing = showing_desktop_;
        return true;
    }

    bool toggle_show_desktop()
    {
        const ShowDesktopTool tool = show_desktop_tool();
        if (tool == ShowDesktopTool::Kde) {
            bool showing = false;
            return kwin_show_desktop_state(showing) && kwin_set_show_desktop(!showing);
        }
        if (tool == ShowDesktopTool::Gnome) {
            initialize_task_backend();
            return task_backend_ != nullptr && task_backend_->toggle_show_desktop();
        }
        if (tool == ShowDesktopTool::Wlrctl) {
            if (!showing_desktop_) {
                show_desktop_task_ids_.clear();
                for (const TaskWindowInfo& task : read_wlr_task_windows()) {
                    if (run_wlr_task_action(task.id, TaskAction::Minimize)) {
                        show_desktop_task_ids_.push_back(task.id);
                    }
                }
                showing_desktop_ = true;
                return true;
            }
            for (const std::string& id : show_desktop_task_ids_) {
                run_wlr_task_action(id, TaskAction::Activate);
            }
            show_desktop_task_ids_.clear();
            showing_desktop_ = false;
            return true;
        }

        std::vector<std::string> command;
        if (tool == ShowDesktopTool::Gnome) {
            command = {"gdbus", "call", "--session", "--dest", "org.gnome.Shell",
                "--object-path", "/org/gnome/Shell", "--method", "org.gnome.Shell.Eval",
                "Main.overview.toggle();"};
        } else if (tool == ShowDesktopTool::Ydotool) {
            command = {"ydotool", "key", "125:1", "32:1", "32:0", "125:0"};
        } else if (tool == ShowDesktopTool::Wtype) {
            command = {"wtype", "-M", "win", "-k", "d", "-m", "win"};
        }
        if (command.empty() || SystemCommands::run(command).exit_code != 0) {
            return false;
        }
        showing_desktop_ = !showing_desktop_;
        return true;
    }

    bool supports_confirmed_window_close() const
    {
        initialize_task_backend();
        if (task_backend_ != nullptr && task_backend_->supports_action(TaskAction::Close)) {
            return true;
        }
        if ((std::getenv("HYPRLAND_INSTANCE_SIGNATURE") != nullptr &&
             !SystemCommands::find_executable("hyprctl").empty()) ||
            (std::getenv("SWAYSOCK") != nullptr &&
             !SystemCommands::find_executable("swaymsg").empty())) {
            return true;
        }
        bool showing_desktop = false;
        return !kwin_show_desktop_state(showing_desktop) &&
            !SystemCommands::find_executable("wtype").empty();
    }

    bool confirm_close_active_window()
    {
        if (!supports_confirmed_window_close()) {
            panel_debug("close-tracker: close confirmation requested but unsupported");
            return false;
        }
        initialize_task_backend();
        std::string target_id;
        if (task_backend_ != nullptr &&
            task_backend_->supports_action(TaskAction::Close)) {
            const auto tasks = task_backend_->list_windows();
            const auto active = std::find_if(tasks.begin(), tasks.end(),
                [](const TaskWindowInfo& task) { return task.active; });
            if (active != tasks.end()) {
                target_id = active->id;
                panel_debug("close-tracker: captured active window before popup: '",
                    active->title, "' (id=", active->id, ")");
            } else {
                panel_debug("close-tracker: no active task captured before popup; using active-window fallback");
            }
        }
        panel_debug("close-tracker: showing close confirmation menu");
        const bool vertical = edge_ == Edge::Left || edge_ == Edge::Right;
        open_task_menu_popup(vertical ? backend_.height_ / 2 : backend_.width_ / 2,
            "Close active window?",
            {"Cancel", "Close active window"}, [this, target_id = std::move(target_id)](std::size_t selected) {
                panel_debug("close-tracker: confirmation selection index=", selected);
                if (selected != 1) {
                    return;
                }
                initialize_task_backend();
                if (task_backend_ != nullptr && !target_id.empty()) {
                    const bool sent = task_backend_->perform_action(target_id, TaskAction::Close);
                    panel_debug("close-tracker: captured-window close request ",
                        sent ? "accepted" : "rejected", " (id=", target_id, ")");
                    if (sent) return;
                }
                if (task_backend_ != nullptr && task_backend_->close_active_window()) {
                    panel_debug("close-tracker: KWin target-resolution request accepted by task backend");
                    return;
                }
                if (task_backend_ != nullptr &&
                    task_backend_->supports_action(TaskAction::Close)) {
                    const auto tasks = task_backend_->list_windows();
                    const auto active = std::find_if(tasks.begin(), tasks.end(),
                        [](const TaskWindowInfo& task) { return task.active; });
                    if (active != tasks.end()) {
                        const bool sent = task_backend_->perform_action(active->id, TaskAction::Close);
                        panel_debug("close-tracker: active task close request ",
                            sent ? "accepted" : "rejected", " (id=", active->id, ")");
                    } else {
                        panel_debug("close-tracker: task backend has no active window in its snapshot");
                    }
                    return;
                }
                std::vector<std::string> command;
                if (std::getenv("HYPRLAND_INSTANCE_SIGNATURE") != nullptr &&
                    !SystemCommands::find_executable("hyprctl").empty()) {
                    command = {"hyprctl", "dispatch", "killactive"};
                } else if (std::getenv("SWAYSOCK") != nullptr &&
                           !SystemCommands::find_executable("swaymsg").empty()) {
                    command = {"swaymsg", "kill"};
                } else {
                    command = {"wtype", "-M", "alt", "-k", "F4", "-m", "alt"};
                }
                panel_debug("close-tracker: running close fallback: ", command.front());
                auto* deferred_command = new std::vector<std::string>(std::move(command));
                g_timeout_add_full(G_PRIORITY_DEFAULT, 250, [](gpointer data) -> gboolean {
                    const auto result = SystemCommands::run(
                        *static_cast<std::vector<std::string>*>(data));
                    panel_debug("close-tracker: close fallback exited with status ", result.exit_code);
                    return G_SOURCE_REMOVE;
                }, deferred_command, [](gpointer data) {
                    delete static_cast<std::vector<std::string>*>(data);
                });
            });
        return true;
    }

    void destroy_buffers()
    {
        buffers_.clear();
        retired_buffers_.clear();
        retired_tooltip_popups_.clear();
    }
};

WaylandBackend::WaylandBackend() = default;

WaylandBackend::~WaylandBackend() = default;

bool WaylandBackend::init(int requested_width, int requested_height, Edge edge)
{
    impl_ = std::make_unique<WaylandBackendImpl>(*this);
    return impl_->connect() &&
        impl_->create_layer_surface(edge, requested_width, requested_height, monitor_);
}

bool WaylandBackend::set_monitor(const std::string& monitor)
{
    monitor_ = monitor.empty() ? "all" : monitor;
    return true;
}

void WaylandBackend::set_task_monitor_filter(bool enabled)
{
    task_monitor_filter_ = enabled;
}

std::vector<std::string> WaylandBackend::list_monitors()
{
    WaylandBackendImpl probe(*this);
    if (!probe.connect()) {
        return {};
    }
    return probe.output_names();
}

cairo_t* WaylandBackend::get_cairo()
{
    return impl_ == nullptr ? nullptr : impl_->drawing_context();
}

void WaylandBackend::commit()
{
    if (impl_ != nullptr) impl_->commit();
}

int WaylandBackend::event_fd() const
{
    return impl_ == nullptr ? -1 : impl_->event_fd();
}

void WaylandBackend::process_events()
{
    if (impl_ != nullptr) impl_->process_events();
}

void WaylandBackend::set_on_click(ClickCallback callback)
{
    click_callback_ = std::move(callback);
}

void WaylandBackend::set_on_motion(MotionCallback callback)
{
    motion_callback_ = std::move(callback);
}

void WaylandBackend::show_tooltip(const std::string& text, int x, int y)
{
    if (impl_ != nullptr) {
        impl_->show_tooltip(text, x, y);
    }
}

void WaylandBackend::hide_tooltip()
{
    if (impl_ != nullptr) {
        impl_->hide_tooltip();
    }
}

void WaylandBackend::toggle_calendar(int anchor_x, int anchor_width)
{
    if (impl_ != nullptr) {
        impl_->toggle_calendar(anchor_x, anchor_width);
    }
}

void WaylandBackend::open_desktop_hub(int anchor_x, int anchor_width, std::string icon_name,
    std::vector<HubDisplayItem> items,
    std::function<void(const std::string&)> on_search,
    std::function<void(std::size_t)> on_activate,
    std::function<void()> on_close)
{
    if (impl_ != nullptr) {
        impl_->open_desktop_hub(anchor_x, anchor_width, std::move(icon_name),
            std::move(items), std::move(on_search), std::move(on_activate), std::move(on_close));
    }
}

void WaylandBackend::update_desktop_hub(std::string query, std::vector<HubDisplayItem> items,
    std::size_t selected, bool preserve_selection)
{
    if (impl_ != nullptr) {
        impl_->update_desktop_hub(std::move(query), std::move(items), selected,
                                  preserve_selection);
    }
}

void WaylandBackend::close_desktop_hub()
{
    if (impl_ != nullptr) {
        impl_->close_desktop_hub();
    }
}

void WaylandBackend::set_desktop_hub_countdown_active(bool active)
{
    if (impl_ != nullptr) {
        impl_->set_desktop_hub_countdown_active(active);
    }
}

bool WaylandBackend::desktop_hub_visible() const
{
    return impl_ != nullptr && impl_->desktop_hub_visible();
}

bool WaylandBackend::supports_task_list() const
{
    return impl_ != nullptr && impl_->supports_task_list();
}

bool WaylandBackend::supports_task_context_menu() const
{
    return impl_ != nullptr && impl_->supports_task_context_menu();
}

bool WaylandBackend::supports_task_action(TaskAction action) const
{
    return impl_ != nullptr && impl_->supports_task_action(action);
}

std::vector<TaskWindowInfo> WaylandBackend::list_task_windows()
{
    return impl_ == nullptr ? std::vector<TaskWindowInfo>{} : impl_->list_task_windows();
}

void WaylandBackend::perform_task_action(const std::string& id, TaskAction action)
{
    if (impl_ != nullptr) {
        impl_->perform_task_action(id, action);
    }
}

void WaylandBackend::show_task_menu(const std::string& id, int panel_x, int panel_y,
                                    bool maximized)
{
    if (impl_ != nullptr) {
        impl_->show_task_menu(id, panel_x, panel_y, maximized);
    }
}

bool WaylandBackend::supports_workspaces() const
{
    return impl_ != nullptr && impl_->supports_workspaces();
}

WorkspaceSnapshot WaylandBackend::get_workspaces()
{
    return impl_ == nullptr ? WorkspaceSnapshot{} : impl_->get_workspaces();
}

bool WaylandBackend::configure_workspace_count(int count)
{
    return impl_ != nullptr && impl_->configure_workspace_count(count);
}

bool WaylandBackend::switch_workspace(int id)
{
    return impl_ != nullptr && impl_->switch_workspace(id);
}

bool WaylandBackend::supports_show_desktop() const
{
    return impl_ != nullptr && impl_->supports_show_desktop();
}

bool WaylandBackend::get_show_desktop_state(bool& showing)
{
    return impl_ != nullptr && impl_->get_show_desktop_state(showing);
}

bool WaylandBackend::toggle_show_desktop()
{
    return impl_ != nullptr && impl_->toggle_show_desktop();
}

bool WaylandBackend::supports_confirmed_window_close() const
{
    return impl_ != nullptr && impl_->supports_confirmed_window_close();
}

bool WaylandBackend::confirm_close_active_window()
{
    return impl_ != nullptr && impl_->confirm_close_active_window();
}

void WaylandBackend::open_application_menu(
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
    if (impl_ != nullptr) {
        impl_->open_application_menu(anchor_x, anchor_width, std::move(settings),
            std::move(categories), std::move(items), std::move(on_category),
            std::move(on_search), std::move(on_activate), std::move(on_shortcut),
            std::move(on_power_action), std::move(on_close));
    }
}

void WaylandBackend::update_application_menu(
    std::string query, std::vector<ApplicationMenuDisplayItem> items, std::size_t selected,
    bool preserve_selection)
{
    if (impl_ != nullptr) {
        impl_->update_application_menu(std::move(query), std::move(items), selected,
                                       preserve_selection);
    }
}

void WaylandBackend::focus_application_menu_items()
{
    if (impl_ != nullptr) {
        impl_->focus_application_menu_items();
    }
}

void WaylandBackend::set_application_menu_countdown_active(bool active)
{
    if (impl_ != nullptr) {
        impl_->set_application_menu_countdown_active(active);
    }
}

void WaylandBackend::set_application_menu_category(std::string category)
{
    if (impl_ != nullptr) impl_->set_application_menu_category(std::move(category));
}

void WaylandBackend::set_application_menu_categories(std::vector<std::string> categories)
{
    if (impl_ != nullptr) impl_->set_application_menu_categories(std::move(categories));
}

void WaylandBackend::toggle_application_power_menu(
    int anchor_x, int anchor_width,
    std::function<void(const std::string&, const std::string&)> on_power_action)
{
    if (impl_ != nullptr) {
        impl_->toggle_application_power_menu(anchor_x, anchor_width,
                                             std::move(on_power_action));
    }
}

void WaylandBackend::close_application_menu()
{
    if (impl_ != nullptr) impl_->close_application_menu();
}

bool WaylandBackend::application_menu_visible() const
{
    return impl_ != nullptr && impl_->application_menu_visible();
}

int WaylandBackend::width() const { return width_; }
int WaylandBackend::height() const { return height_; }
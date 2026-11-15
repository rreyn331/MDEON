#include "panel/modules/tray.hpp"

#include "panel/icon_theme.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace {

constexpr char watcher_xml[] =
    "<node><interface name='org.kde.StatusNotifierWatcher'>"
    "<method name='RegisterStatusNotifierItem'><arg type='s' direction='in'/></method>"
    "<method name='RegisterStatusNotifierHost'><arg type='s' direction='in'/></method>"
    "<property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
    "<property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
    "<property name='ProtocolVersion' type='i' access='read'/>"
    "</interface></node>";

bool contains_any(const std::string& text, const std::vector<std::string_view>& terms)
{
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return std::any_of(terms.begin(), terms.end(), [&lower](std::string_view term) {
        return lower.find(term) != std::string::npos;
    });
}

void watcher_method_call(GDBusConnection*, const gchar* sender, const gchar*, const gchar*,
                         const gchar* method, GVariant* parameters,
                         GDBusMethodInvocation* invocation, gpointer data)
{
    auto* self = static_cast<TrayModule*>(data);
    if (g_strcmp0(method, "RegisterStatusNotifierItem") == 0) {
        const gchar* service = nullptr;
        g_variant_get(parameters, "(&s)", &service);
        self->register_item(sender, service);
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

GVariant* watcher_get_property(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                               const gchar* property, GError**, gpointer data)
{
    const auto* self = static_cast<const TrayModule*>(data);
    if (g_strcmp0(property, "IsStatusNotifierHostRegistered") == 0) {
        return g_variant_new_boolean(TRUE);
    }
    if (g_strcmp0(property, "ProtocolVersion") == 0) {
        return g_variant_new_int32(0);
    }
    if (g_strcmp0(property, "RegisteredStatusNotifierItems") == 0) {
        return self->registered_items_variant();
    }
    return nullptr;
}

const GDBusInterfaceVTable watcher_vtable = {
    watcher_method_call, watcher_get_property, nullptr, {nullptr}
};

}

TrayModule::Item::~Item()
{
    if (proxy != nullptr) {
        g_object_unref(proxy);
    }
}

TrayModule::TrayModule(bool vertical)
    : vertical_(vertical)
{
    setup_watcher();
}

TrayModule::~TrayModule()
{
    if (owner_id_ != 0) {
        g_bus_unown_name(owner_id_);
    }
    if (signal_id_ != 0 && connection_ != nullptr) {
        g_dbus_connection_signal_unsubscribe(connection_, signal_id_);
    }
    if (registration_id_ != 0 && connection_ != nullptr) {
        g_dbus_connection_unregister_object(connection_, registration_id_);
    }
    if (connection_ != nullptr) {
        g_object_unref(connection_);
    }
    if (node_info_ != nullptr) {
        g_dbus_node_info_unref(node_info_);
    }
}

bool TrayModule::update()
{
    const bool changed = pending_change_;
    pending_change_ = false;
    return changed;
}

int TrayModule::measure_width(cairo_t*, int)
{
    const auto visible = std::count_if(items_.begin(), items_.end(), [](const auto& item) {
        return item->visible;
    });
    return static_cast<int>(visible) * 24;
}

void TrayModule::render(cairo_t* cr, const Geometry& geometry)
{
    int index = 0;
    for (const auto& item : items_) {
        if (!item->visible) {
            continue;
        }
        const std::string& icon = item->icon_name.empty()
            ? std::string("application-x-executable") : item->icon_name;
        paint_theme_icon(cr, icon,
            vertical_ ? geometry.x + geometry.width / 2.0 : geometry.x + index * 24 + 12.0,
            vertical_ ? geometry.y + index * 24 + 12.0 : geometry.y + geometry.height / 2.0,
            22);
        ++index;
    }
}

void TrayModule::on_click(int button, int rel_x, int rel_y)
{
    if (button < 1 || button > 3) {
        return;
    }
    const int target = (vertical_ ? rel_y : rel_x) / 24;
    int index = 0;
    for (const auto& item : items_) {
        if (!item->visible) {
            continue;
        }
        if (index++ != target || item->proxy == nullptr) {
            continue;
        }
        const char* method = button == 1 ? "Activate" :
            (button == 2 ? "SecondaryActivate" : "ContextMenu");
        g_dbus_proxy_call(item->proxy, method, g_variant_new("(ii)", rel_x % 24, rel_y),
                          G_DBUS_CALL_FLAGS_NONE, 500, nullptr, nullptr, nullptr);
        return;
    }
}

std::string TrayModule::tooltip_text() const
{
    const auto visible = std::count_if(items_.begin(), items_.end(), [](const auto& item) {
        return item->visible;
    });
    return "System tray - " + std::to_string(visible) + " items";
}

void TrayModule::setup_watcher()
{
    GError* error = nullptr;
    connection_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (error != nullptr) {
        g_error_free(error);
    }
    if (connection_ == nullptr) {
        return;
    }
    node_info_ = g_dbus_node_info_new_for_xml(watcher_xml, &error);
    if (error != nullptr) {
        g_error_free(error);
    }
    if (node_info_ == nullptr) {
        return;
    }

    signal_id_ = g_dbus_connection_signal_subscribe(
        connection_, "org.kde.StatusNotifierWatcher", "org.kde.StatusNotifierWatcher", nullptr,
        "/StatusNotifierWatcher", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar* sender, const gchar*, const gchar*, const gchar* signal,
           GVariant* parameters, gpointer data) {
            auto* self = static_cast<TrayModule*>(data);
            const gchar* service = nullptr;
            g_variant_get(parameters, "(&s)", &service);
            if (g_strcmp0(signal, "StatusNotifierItemRegistered") == 0) {
                self->register_item(sender, service);
            } else if (g_strcmp0(signal, "StatusNotifierItemUnregistered") == 0) {
                self->unregister_item(sender, service);
            }
        }, this, nullptr);

    load_registered_items();
    owner_id_ = g_bus_own_name(
        G_BUS_TYPE_SESSION, "org.kde.StatusNotifierWatcher", G_BUS_NAME_OWNER_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, gpointer) {},
        [](GDBusConnection* connection, const gchar*, gpointer data) {
            auto* self = static_cast<TrayModule*>(data);
            self->watcher_owned_ = true;
            self->registration_id_ = g_dbus_connection_register_object(
                connection, "/StatusNotifierWatcher", self->node_info_->interfaces[0],
                &watcher_vtable, self, nullptr, nullptr);
            self->load_registered_items();
            self->pending_change_ = true;
        },
        [](GDBusConnection*, const gchar*, gpointer data) {
            static_cast<TrayModule*>(data)->watcher_owned_ = false;
        }, this, nullptr);
}

void TrayModule::register_item(const std::string& sender, const std::string& service)
{
    std::string item_service = sender;
    std::string object_path = "/StatusNotifierItem";
    const std::size_t slash = service.find('/');
    if (slash != std::string::npos) {
        item_service = service.substr(0, slash);
        object_path = service.substr(slash);
    } else if (!service.empty() && service.front() != ':') {
        item_service = service;
    }
    if (!g_dbus_is_name(item_service.c_str())) {
        if (!g_dbus_is_name(sender.c_str())) {
            g_warning("Ignoring StatusNotifierItem with invalid service and sender names");
            return;
        }
        item_service = sender;
    }
    const std::string id = item_service + object_path;
    const auto exists = std::find_if(items_.begin(), items_.end(), [&id](const auto& item) {
        return item->id == id;
    });
    if (exists != items_.end() || connection_ == nullptr) {
        return;
    }

    auto item = std::make_unique<Item>();
    item->id = id;
    item->service = std::move(item_service);
    item->path = std::move(object_path);
    GError* error = nullptr;
    item->proxy = g_dbus_proxy_new_sync(
        connection_, G_DBUS_PROXY_FLAGS_NONE, nullptr, item->service.c_str(), item->path.c_str(),
        "org.kde.StatusNotifierItem", nullptr, &error);
    if (error != nullptr) {
        g_error_free(error);
    }
    if (item->proxy == nullptr) {
        return;
    }
    Item* item_pointer = item.get();
    g_signal_connect(item->proxy, "g-properties-changed",
                     G_CALLBACK(+[](GDBusProxy*, GVariant*, const gchar* const*, gpointer data) {
                         auto* changed_item = static_cast<Item*>(data);
                         changed_item->owner->refresh_item(*changed_item);
                     }), item_pointer);
    item->owner = this;
    items_.push_back(std::move(item));
    refresh_item(*items_.back());
    pending_change_ = true;
}

GVariant* TrayModule::registered_items_variant() const
{
    std::vector<const gchar*> ids;
    ids.reserve(items_.size());
    for (const auto& item : items_) {
        ids.push_back(item->id.c_str());
    }
    return g_variant_new_strv(ids.data(), ids.size());
}

void TrayModule::unregister_item(const std::string& sender, const std::string& service)
{
    std::string id = service;
    if (id.find('/') == std::string::npos) {
        id = sender + "/StatusNotifierItem";
    }
    const std::size_t slash = id.find('/');
    if (slash != std::string::npos && id.front() != ':') {
        id = sender + id.substr(slash);
    }
    const auto old_size = items_.size();
    std::erase_if(items_, [&id](const auto& item) { return item->id == id; });
    pending_change_ = pending_change_ || items_.size() != old_size;
}

void TrayModule::load_registered_items()
{
    if (connection_ == nullptr) {
        return;
    }
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        connection_, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.kde.StatusNotifierWatcher", "RegisteredStatusNotifierItems"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 500, nullptr, &error);
    if (error != nullptr) {
        g_error_free(error);
    }
    if (reply == nullptr) {
        return;
    }
    GVariant* boxed = nullptr;
    g_variant_get(reply, "(v)", &boxed);
    GVariant* registered = boxed;
    gsize count = 0;
    const gchar** names = g_variant_get_strv(registered, &count);
    for (gsize index = 0; index < count; ++index) {
        const std::string name(names[index]);
        const std::size_t slash = name.find('/');
        if (slash != std::string::npos) {
            register_item(name.substr(0, slash), name);
        }
    }
    g_free(names);
    g_variant_unref(registered);
    g_variant_unref(reply);
}

void TrayModule::refresh_item(Item& item)
{
    GVariant* icon = g_dbus_proxy_get_cached_property(item.proxy, "IconName");
    if (icon != nullptr) {
        if (g_variant_is_of_type(icon, G_VARIANT_TYPE_STRING)) {
            item.icon_name = g_variant_get_string(icon, nullptr);
        }
        g_variant_unref(icon);
    }
    GVariant* title = g_dbus_proxy_get_cached_property(item.proxy, "Title");
    if (title != nullptr) {
        if (g_variant_is_of_type(title, G_VARIANT_TYPE_STRING)) {
            item.title = g_variant_get_string(title, nullptr);
        }
        g_variant_unref(title);
    }
    GVariant* tooltip = g_dbus_proxy_get_cached_property(item.proxy, "ToolTip");
    if (tooltip != nullptr) {
        gchar* rendered = g_variant_print(tooltip, TRUE);
        item.tooltip = rendered != nullptr ? rendered : "";
        g_free(rendered);
        g_variant_unref(tooltip);
    }
    item.visible = !should_hide(item);
    pending_change_ = true;
}

bool TrayModule::should_hide(const Item& item) const
{
    const std::string metadata = item.title + " " + item.icon_name + " " +
        item.service + " " + item.id + " " + item.tooltip;
    static const std::vector<std::string_view> volume_terms = {
        "volume", "audio", "pulseaudio", "pavucontrol", "pnmixer", "pasystray", "volctl", "volumeicon"
    };
    static const std::vector<std::string_view> network_terms = {
        "network", "nm-applet", "networkmanager", "network-manager", "nm-tray", "connman", "wicd", "wifi"
    };
    static const std::vector<std::string_view> bluetooth_terms = {
        "bluetooth", "bluez", "blueman", "bt-applet"
    };
    return contains_any(metadata, volume_terms) || contains_any(metadata, network_terms) ||
        contains_any(metadata, bluetooth_terms);
}
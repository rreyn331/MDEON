#include "panel/modules/notifications.hpp"

#include "panel/icon_theme.hpp"
#include "panel/modules/system_commands.hpp"
#include "panel/theme.hpp"

#include <algorithm>
#include <map>
#include <memory>

#include <pango/pangocairo.h>

struct NotificationProxyState {
    GDBusConnection* connection{nullptr};
    GDBusConnection* monitor_connection{nullptr};
    GMainContext* main_context{nullptr};
    guint monitor_filter_id{0};
    guint closed_subscription{0};
    guint action_subscription{0};
    std::map<std::uint32_t, std::uint32_t> remote_by_local;
    std::map<std::uint32_t, std::uint32_t> local_by_remote;
    std::map<std::uint32_t, std::uint32_t> local_by_call_serial;
    std::map<std::uint32_t, std::uint32_t> observed_local_by_remote;
    std::function<void(std::uint32_t)> on_remote_closed;
    std::function<void(std::uint32_t, const std::string&, const std::string&,
                       const std::string&, std::uint32_t)> on_observed_notify;
    std::function<void(std::uint32_t, std::uint32_t)> on_observed_reply;

    ~NotificationProxyState()
    {
        if (monitor_connection != nullptr) {
            if (monitor_filter_id != 0) {
                g_dbus_connection_remove_filter(monitor_connection, monitor_filter_id);
            }
            g_object_unref(monitor_connection);
        }
        if (connection != nullptr) {
            g_object_unref(connection);
        }
        if (main_context != nullptr) {
            g_main_context_unref(main_context);
        }
    }
};

namespace {

constexpr char notification_xml[] =
    "<node>"
    "<interface name='org.freedesktop.Notifications'>"
    "<method name='GetServerInformation'>"
    "<arg type='s' direction='out'/><arg type='s' direction='out'/>"
    "<arg type='s' direction='out'/><arg type='s' direction='out'/>"
    "</method>"
    "<method name='GetCapabilities'><arg type='as' direction='out'/></method>"
    "<method name='Notify'>"
    "<arg type='s' direction='in'/><arg type='u' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='as' direction='in'/>"
    "<arg type='a{sv}' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='u' direction='out'/>"
    "</method>"
    "<method name='CloseNotification'><arg type='u' direction='in'/></method>"
    "<signal name='NotificationClosed'><arg type='u'/><arg type='u'/></signal>"
    "<signal name='ActionInvoked'><arg type='u'/><arg type='s'/></signal>"
    "</interface></node>";

void notification_method_call(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                              const gchar* method, GVariant* parameters,
                              GDBusMethodInvocation* invocation, gpointer data)
{
    static_cast<NotificationsModule*>(data)->handle_dbus_method_call(
        method, parameters, invocation);
}

const GDBusInterfaceVTable notification_vtable = {
    notification_method_call, nullptr, nullptr, {nullptr}
};

struct ForwardNotifyCall {
    std::shared_ptr<NotificationProxyState> state;
    GDBusMethodInvocation* invocation;
    std::uint32_t local_id;
};

struct ForwardSimpleCall {
    GDBusMethodInvocation* invocation;
    bool capabilities;
};

struct PassiveMonitorEvent {
    std::shared_ptr<NotificationProxyState> state;
    bool is_reply;
    bool is_closed;
    std::uint32_t serial;
    std::uint32_t remote_id;
    std::uint32_t replaces_id;
    std::string app;
    std::string summary;
    std::string body;
};

gboolean dispatch_passive_monitor_event(gpointer data)
{
    std::unique_ptr<PassiveMonitorEvent> event(
        static_cast<PassiveMonitorEvent*>(data));
    if (event->is_closed) {
        const auto local = event->state->observed_local_by_remote.find(event->remote_id);
        if (local != event->state->observed_local_by_remote.end()) {
            const std::uint32_t local_id = local->second;
            event->state->observed_local_by_remote.erase(local);
            if (event->state->on_remote_closed) {
                event->state->on_remote_closed(local_id);
            }
        }
    } else if (event->is_reply) {
        if (event->state->on_observed_reply) {
            event->state->on_observed_reply(event->serial, event->remote_id);
        }
    } else if (event->state->on_observed_notify) {
        event->state->on_observed_notify(event->serial, event->app, event->summary,
                                         event->body, event->replaces_id);
    }
    return G_SOURCE_REMOVE;
}

GDBusMessage* passive_monitor_filter(GDBusConnection*, GDBusMessage* message,
                                     gboolean incoming, gpointer data)
{
    if (!incoming) {
        return message;
    }
    const GDBusMessageType type = g_dbus_message_get_message_type(message);
    auto* state = static_cast<std::shared_ptr<NotificationProxyState>*>(data);
    if (type == G_DBUS_MESSAGE_TYPE_METHOD_CALL &&
        g_strcmp0(g_dbus_message_get_path(message),
                  "/org/freedesktop/Notifications") == 0 &&
        g_strcmp0(g_dbus_message_get_interface(message),
                  "org.freedesktop.Notifications") == 0 &&
        g_strcmp0(g_dbus_message_get_member(message), "Notify") == 0) {
        GVariant* body = g_dbus_message_get_body(message);
        if (body == nullptr || !g_variant_is_of_type(
                body, G_VARIANT_TYPE("(susssasa{sv}i)"))) {
            return message;
        }
        const gchar* app;
        const gchar* app_icon;
        const gchar* summary;
        const gchar* text;
        guint32 replaces_id;
        gint32 timeout;
        GVariant* actions;
        GVariant* hints;
        g_variant_get(body, "(&su&s&s&s@as@a{sv}i)", &app, &replaces_id,
                      &app_icon, &summary, &text, &actions, &hints, &timeout);
        auto* event = new PassiveMonitorEvent{
            *state, false, false, g_dbus_message_get_serial(message), 0, replaces_id,
            app, summary, text};
        g_variant_unref(actions);
        g_variant_unref(hints);
        g_main_context_invoke_full((*state)->main_context, G_PRIORITY_DEFAULT,
                                   dispatch_passive_monitor_event, event, nullptr);
    } else if (type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN) {
        GVariant* body = g_dbus_message_get_body(message);
        if (body != nullptr && g_variant_is_of_type(body, G_VARIANT_TYPE("(u)"))) {
            guint32 remote_id = 0;
            g_variant_get(body, "(u)", &remote_id);
            auto* event = new PassiveMonitorEvent{
                *state, true, false,
                g_dbus_message_get_reply_serial(message), remote_id,
                0, {}, {}, {}};
            g_main_context_invoke_full((*state)->main_context, G_PRIORITY_DEFAULT,
                                       dispatch_passive_monitor_event, event, nullptr);
        }
    } else if (type == G_DBUS_MESSAGE_TYPE_ERROR) {
        auto* event = new PassiveMonitorEvent{
            *state, true, false, g_dbus_message_get_reply_serial(message), 0,
            0, {}, {}, {}};
        g_main_context_invoke_full((*state)->main_context, G_PRIORITY_DEFAULT,
                                   dispatch_passive_monitor_event, event, nullptr);
    } else if (type == G_DBUS_MESSAGE_TYPE_SIGNAL &&
               g_strcmp0(g_dbus_message_get_path(message),
                         "/org/freedesktop/Notifications") == 0 &&
               g_strcmp0(g_dbus_message_get_interface(message),
                         "org.freedesktop.Notifications") == 0 &&
               g_strcmp0(g_dbus_message_get_member(message),
                         "NotificationClosed") == 0) {
        GVariant* body = g_dbus_message_get_body(message);
        if (body != nullptr && g_variant_is_of_type(body, G_VARIANT_TYPE("(uu)"))) {
            guint32 remote_id = 0;
            guint32 reason = 0;
            g_variant_get(body, "(uu)", &remote_id, &reason);
            auto* event = new PassiveMonitorEvent{
                *state, false, true, 0, remote_id, reason, {}, {}, {}};
            g_main_context_invoke_full((*state)->main_context, G_PRIORITY_DEFAULT,
                                       dispatch_passive_monitor_event, event, nullptr);
        }
    }
    return message;
}

void return_notification_closed(const std::shared_ptr<NotificationProxyState>& state,
                                std::uint32_t id, std::uint32_t reason)
{
    if (state->connection == nullptr) {
        return;
    }
    GError* error = nullptr;
    g_dbus_connection_emit_signal(
        state->connection, nullptr, "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "NotificationClosed",
        g_variant_new("(uu)", id, reason), &error);
    if (error != nullptr) {
        g_warning("Could not relay NotificationClosed: %s", error->message);
        g_error_free(error);
    }
}

void forward_notify_finished(GObject* source, GAsyncResult* result, gpointer data)
{
    std::unique_ptr<ForwardNotifyCall> call(static_cast<ForwardNotifyCall*>(data));
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_finish(
        G_DBUS_CONNECTION(source), result, &error);
    if (reply != nullptr) {
        guint32 remote_id = 0;
        g_variant_get(reply, "(u)", &remote_id);
        const auto old_remote = call->state->remote_by_local.find(call->local_id);
        if (old_remote != call->state->remote_by_local.end()) {
            call->state->local_by_remote.erase(old_remote->second);
        }
        call->state->remote_by_local.insert_or_assign(call->local_id, remote_id);
        call->state->local_by_remote.insert_or_assign(remote_id, call->local_id);
        g_variant_unref(reply);
    } else if (error != nullptr) {
        g_warning("Could not forward notification: %s", error->message);
        g_error_free(error);
    }
    g_dbus_method_invocation_return_value(
        call->invocation, g_variant_new("(u)", call->local_id));
    g_object_unref(call->invocation);
}

void forward_simple_finished(GObject* source, GAsyncResult* result, gpointer data)
{
    std::unique_ptr<ForwardSimpleCall> call(static_cast<ForwardSimpleCall*>(data));
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_finish(
        G_DBUS_CONNECTION(source), result, &error);
    if (reply != nullptr) {
        g_dbus_method_invocation_return_value(call->invocation, reply);
        g_variant_unref(reply);
    } else {
        if (error != nullptr) {
            g_warning("Could not query notification daemon: %s", error->message);
            g_error_free(error);
        }
        if (call->capabilities) {
            const gchar* capabilities[] = {"body", "actions", nullptr};
            g_dbus_method_invocation_return_value(
                call->invocation, g_variant_new("(^as)", capabilities));
        } else {
            g_dbus_method_invocation_return_value(
                call->invocation,
                g_variant_new("(ssss)", "mdeon-panel", "mdeon-panel", "1.0", "1.2"));
        }
    }
    g_object_unref(call->invocation);
}

void destroy_proxy_state(gpointer data)
{
    delete static_cast<std::shared_ptr<NotificationProxyState>*>(data);
}

void notification_signal_received(GDBusConnection*, const gchar*, const gchar*,
                                  const gchar*, const gchar* signal, GVariant* parameters,
                                  gpointer data)
{
    const auto& state = *static_cast<std::shared_ptr<NotificationProxyState>*>(data);
    if (g_strcmp0(signal, "NotificationClosed") == 0) {
        guint32 remote_id = 0;
        guint32 reason = 0;
        g_variant_get(parameters, "(uu)", &remote_id, &reason);
        const auto local = state->local_by_remote.find(remote_id);
        if (local == state->local_by_remote.end()) {
            return;
        }
        const std::uint32_t local_id = local->second;
        state->local_by_remote.erase(local);
        state->remote_by_local.erase(local_id);
        if (state->on_remote_closed) {
            state->on_remote_closed(local_id);
        }
        return_notification_closed(state, local_id, reason);
    } else if (g_strcmp0(signal, "ActionInvoked") == 0) {
        guint32 remote_id = 0;
        const gchar* action = nullptr;
        g_variant_get(parameters, "(u&s)", &remote_id, &action);
        const auto local = state->local_by_remote.find(remote_id);
        if (local == state->local_by_remote.end() || state->connection == nullptr) {
            return;
        }
        GError* error = nullptr;
        g_dbus_connection_emit_signal(
            state->connection, nullptr, "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications", "ActionInvoked",
            g_variant_new("(us)", local->second, action), &error);
        if (error != nullptr) {
            g_warning("Could not relay ActionInvoked: %s", error->message);
            g_error_free(error);
        }
    }
}

}

NotificationsModule::NotificationsModule(PopupCallback open_history,
                             std::string proxy_destination, bool vertical)
    : open_history_(std::move(open_history)),
            proxy_destination_(std::move(proxy_destination)),
            proxy_state_(std::make_shared<NotificationProxyState>()), vertical_(vertical)
{
    proxy_state_->main_context = g_main_context_ref_thread_default();
    if (!proxy_destination_.empty() &&
        (!g_dbus_is_name(proxy_destination_.c_str()) ||
         proxy_destination_ == "org.freedesktop.Notifications")) {
        g_warning("Ignoring invalid notification proxy destination: %s",
                  proxy_destination_.c_str());
        proxy_destination_.clear();
    }
    setup_service();
}

NotificationsModule::~NotificationsModule()
{
    stop_passive_monitor();
    if (owner_id_ != 0) {
        g_bus_unown_name(owner_id_);
    }
    proxy_state_->on_remote_closed = {};
    proxy_state_->on_observed_notify = {};
    proxy_state_->on_observed_reply = {};
    if (proxy_state_->connection != nullptr) {
        if (proxy_state_->closed_subscription != 0) {
            g_dbus_connection_signal_unsubscribe(
                proxy_state_->connection, proxy_state_->closed_subscription);
        }
        if (proxy_state_->action_subscription != 0) {
            g_dbus_connection_signal_unsubscribe(
                proxy_state_->connection, proxy_state_->action_subscription);
        }
        if (registration_id_ != 0) {
            g_dbus_connection_unregister_object(
                proxy_state_->connection, registration_id_);
        }
    }
    if (node_info_ != nullptr) {
        g_dbus_node_info_unref(node_info_);
    }
}

void NotificationsModule::handle_dbus_method_call(
    const gchar* method, GVariant* parameters, GDBusMethodInvocation* invocation)
{
    if (g_strcmp0(method, "GetServerInformation") == 0 ||
        g_strcmp0(method, "GetCapabilities") == 0) {
        const bool capabilities = g_strcmp0(method, "GetCapabilities") == 0;
        if (!proxy_destination_.empty() && proxy_state_->connection != nullptr) {
            g_dbus_connection_call(
                proxy_state_->connection, proxy_destination_.c_str(),
                "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                capabilities ? "GetCapabilities" : "GetServerInformation", nullptr,
                capabilities ? G_VARIANT_TYPE("(as)") : G_VARIANT_TYPE("(ssss)"),
                G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, forward_simple_finished,
                new ForwardSimpleCall{G_DBUS_METHOD_INVOCATION(g_object_ref(invocation)),
                                      capabilities});
            return;
        }
        if (capabilities) {
            const gchar* values[] = {"body", "actions", nullptr};
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(^as)", values));
        } else {
            g_dbus_method_invocation_return_value(
                invocation,
                g_variant_new("(ssss)", "mdeon-panel", "mdeon-panel", "1.0", "1.2"));
        }
    } else if (g_strcmp0(method, "Notify") == 0) {
        const gchar* app;
        const gchar* app_icon;
        const gchar* summary;
        const gchar* body;
        guint32 replaces_id;
        gint32 timeout;
        GVariant* actions;
        GVariant* hints;
        g_variant_get(parameters, "(&su&s&s&s@as@a{sv}i)",
                      &app, &replaces_id, &app_icon, &summary, &body,
                      &actions, &hints, &timeout);
        const std::uint32_t local_id = add_notification(app, summary, body, replaces_id);
        if (proxy_destination_.empty() || proxy_state_->connection == nullptr) {
            g_variant_unref(actions);
            g_variant_unref(hints);
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(u)", local_id));
            return;
        }
        guint32 remote_replaces_id = 0;
        const auto mapped = proxy_state_->remote_by_local.find(replaces_id);
        if (replaces_id != 0 && mapped != proxy_state_->remote_by_local.end()) {
            remote_replaces_id = mapped->second;
        }
        GVariant* forwarded_parameters = g_variant_new(
            "(susss@as@a{sv}i)", app, remote_replaces_id, app_icon, summary, body,
            actions, hints, timeout);
        g_dbus_connection_call(
            proxy_state_->connection, proxy_destination_.c_str(),
            "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify",
            forwarded_parameters, G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE,
            5000, nullptr, forward_notify_finished,
            new ForwardNotifyCall{proxy_state_,
                G_DBUS_METHOD_INVOCATION(g_object_ref(invocation)), local_id});
    } else if (g_strcmp0(method, "CloseNotification") == 0) {
        guint32 local_id = 0;
        g_variant_get(parameters, "(u)", &local_id);
        const auto mapped = proxy_state_->remote_by_local.find(local_id);
        if (!proxy_destination_.empty() && proxy_state_->connection != nullptr &&
            mapped != proxy_state_->remote_by_local.end()) {
            g_dbus_connection_call(
                proxy_state_->connection, proxy_destination_.c_str(),
                "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                "CloseNotification", g_variant_new("(u)", mapped->second), nullptr,
                G_DBUS_CALL_FLAGS_NONE, 3000, nullptr,
                [](GObject* source, GAsyncResult* result, gpointer data) {
                    std::unique_ptr<ForwardNotifyCall> call(
                        static_cast<ForwardNotifyCall*>(data));
                    GError* error = nullptr;
                    GVariant* reply = g_dbus_connection_call_finish(
                        G_DBUS_CONNECTION(source), result, &error);
                    if (reply != nullptr) {
                        g_variant_unref(reply);
                    } else {
                        if (error != nullptr) {
                            g_warning("Could not close forwarded notification: %s",
                                      error->message);
                            g_error_free(error);
                        }
                        call->state->remote_by_local.erase(call->local_id);
                        for (auto iter = call->state->local_by_remote.begin();
                             iter != call->state->local_by_remote.end(); ++iter) {
                            if (iter->second == call->local_id) {
                                call->state->local_by_remote.erase(iter);
                                break;
                            }
                        }
                        if (call->state->on_remote_closed) {
                            call->state->on_remote_closed(call->local_id);
                        }
                        return_notification_closed(call->state, call->local_id, 3);
                    }
                    g_object_unref(call->invocation);
                },
                new ForwardNotifyCall{proxy_state_,
                    G_DBUS_METHOD_INVOCATION(g_object_ref(invocation)), local_id});
            g_dbus_method_invocation_return_value(invocation, nullptr);
        } else {
            close_notification(local_id);
            return_notification_closed(proxy_state_, local_id, 3);
            g_dbus_method_invocation_return_value(invocation, nullptr);
        }
    }
}

bool NotificationsModule::update()
{
    const bool changed = pending_change_;
    pending_change_ = false;
    return changed;
}

int NotificationsModule::measure_width(cairo_t*, int)
{
    if (vertical_) return 30;
    const std::string count = unread_ > 0 ? std::to_string(std::min(unread_, 99)) : "";
    return count.empty() ? 30 : 34 + static_cast<int>(count.size()) * 3;
}

void NotificationsModule::render(cairo_t* cr, const Geometry& geometry)
{
    geometry_ = geometry;
    paint_theme_icon(cr, "preferences-system-notifications-symbolic",
        vertical_ ? geometry.x + geometry.width / 2.0 : geometry.x + 15.0,
        geometry.y + geometry.height / 2.0, 28);
    if (unread_ <= 0) {
        return;
    }

    const std::string count = std::to_string(std::min(unread_, 99));
    PangoLayout* layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, count.c_str(), -1);
    PangoFontDescription* font = pango_font_description_from_string("Sans Bold 7");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    int text_width = 0;
    int text_height = 0;
    pango_layout_get_pixel_size(layout, &text_width, &text_height);
    const double badge_x = vertical_ ? geometry.x + geometry.width / 2.0 + 9.0
                                      : geometry.x + 24.0;
    const double badge_y = vertical_ ? geometry.y + 7.0 : geometry.y + 5.0;
    const double radius = std::max(5.0, text_width / 2.0 + 2.0);
    cairo_set_source_rgb(cr, 0.75, 0.12, 0.12);
    cairo_arc(cr, badge_x, badge_y, radius, 0.0, 6.283185307179586);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_move_to(cr, badge_x - text_width / 2.0,
                  badge_y - text_height / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

void NotificationsModule::on_click(int button, int, int)
{
    if (button == 1) {
        unread_ = 0;
        pending_change_ = true;
        if (open_history_) {
            open_history_(history_text(), geometry_.x + geometry_.width / 2,
                          geometry_.y + geometry_.height / 2);
        }
    } else if (button == 3 && !entries_.empty()) {
        entries_.clear();
        unread_ = 0;
        pending_change_ = true;
    }
}

std::string NotificationsModule::tooltip_text() const
{
    if (!service_owned_ && !passive_monitoring_) {
        return "Notifications handled by the desktop daemon";
    }
    return std::string(passive_monitoring_ ? "Notifications (passive) - " : "Notifications - ") +
        std::to_string(unread_) + " unread\n" +
        "Left-click: view history\nRight-click: clear history";
}

void NotificationsModule::setup_service()
{
    GError* error = nullptr;
    node_info_ = g_dbus_node_info_new_for_xml(notification_xml, &error);
    if (error != nullptr) {
        g_error_free(error);
        node_info_ = nullptr;
        return;
    }
    const GBusNameOwnerFlags owner_flags = proxy_destination_.empty()
        ? G_BUS_NAME_OWNER_FLAGS_NONE : G_BUS_NAME_OWNER_FLAGS_REPLACE;
    owner_id_ = g_bus_own_name(
        G_BUS_TYPE_SESSION, "org.freedesktop.Notifications", owner_flags,
        [](GDBusConnection*, const gchar*, gpointer) {},
        [](GDBusConnection* connection, const gchar*, gpointer data) {
            auto* self = static_cast<NotificationsModule*>(data);
            self->stop_passive_monitor();
            if (self->proxy_state_->connection != nullptr) {
                if (self->proxy_state_->closed_subscription != 0) {
                    g_dbus_connection_signal_unsubscribe(
                        self->proxy_state_->connection,
                        self->proxy_state_->closed_subscription);
                    self->proxy_state_->closed_subscription = 0;
                }
                if (self->proxy_state_->action_subscription != 0) {
                    g_dbus_connection_signal_unsubscribe(
                        self->proxy_state_->connection,
                        self->proxy_state_->action_subscription);
                    self->proxy_state_->action_subscription = 0;
                }
                g_object_unref(self->proxy_state_->connection);
            }
            self->proxy_state_->connection = G_DBUS_CONNECTION(g_object_ref(connection));
            GError* registration_error = nullptr;
            self->registration_id_ = g_dbus_connection_register_object(
                connection, "/org/freedesktop/Notifications", self->node_info_->interfaces[0],
                &notification_vtable, self, nullptr, &registration_error);
            self->service_owned_ = self->registration_id_ != 0;
            if (self->service_owned_ && !self->proxy_destination_.empty()) {
                self->proxy_state_->on_remote_closed = [self](std::uint32_t id) {
                    self->close_notification(id);
                };
                auto* closed_state = new std::shared_ptr<NotificationProxyState>(
                    self->proxy_state_);
                self->proxy_state_->closed_subscription = g_dbus_connection_signal_subscribe(
                    connection, self->proxy_destination_.c_str(),
                    "org.freedesktop.Notifications", "NotificationClosed",
                    "/org/freedesktop/Notifications", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                    notification_signal_received, closed_state, destroy_proxy_state);
                auto* action_state = new std::shared_ptr<NotificationProxyState>(
                    self->proxy_state_);
                self->proxy_state_->action_subscription = g_dbus_connection_signal_subscribe(
                    connection, self->proxy_destination_.c_str(),
                    "org.freedesktop.Notifications", "ActionInvoked",
                    "/org/freedesktop/Notifications", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                    notification_signal_received, action_state, destroy_proxy_state);
            }
            self->pending_change_ = true;
            if (registration_error != nullptr) {
                g_warning("Could not register notification service: %s",
                          registration_error->message);
                g_error_free(registration_error);
            }
        },
        [](GDBusConnection*, const gchar*, gpointer data) {
            auto* self = static_cast<NotificationsModule*>(data);
            self->service_owned_ = false;
            self->pending_change_ = true;
            self->start_passive_monitor();
        }, this, nullptr);
}

void NotificationsModule::start_passive_monitor()
{
    if (proxy_state_->monitor_connection != nullptr) {
        return;
    }
    GError* error = nullptr;
    gchar* address = g_dbus_address_get_for_bus_sync(
        G_BUS_TYPE_SESSION, nullptr, &error);
    if (address == nullptr) {
        if (error != nullptr) {
            g_warning("Could not locate session bus for passive notifications: %s",
                      error->message);
            g_error_free(error);
        }
        return;
    }
    GDBusConnection* monitor = g_dbus_connection_new_for_address_sync(
        address,
        static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, &error);
    g_free(address);
    if (monitor == nullptr) {
        if (error != nullptr) {
            g_warning("Could not connect passive notification monitor: %s",
                      error->message);
            g_error_free(error);
        }
        return;
    }

    auto* filter_state = new std::shared_ptr<NotificationProxyState>(proxy_state_);
    const guint filter_id = g_dbus_connection_add_filter(
        monitor, passive_monitor_filter, filter_state, destroy_proxy_state);
    GVariantBuilder rules;
    g_variant_builder_init(&rules, G_VARIANT_TYPE("as"));
    g_variant_builder_add(&rules, "s",
        "type='method_call',interface='org.freedesktop.Notifications',"
        "member='Notify',path='/org/freedesktop/Notifications'");
    g_variant_builder_add(&rules, "s",
        "type='method_return',sender='org.freedesktop.Notifications'");
    g_variant_builder_add(&rules, "s",
        "type='error',sender='org.freedesktop.Notifications'");
    g_variant_builder_add(&rules, "s",
        "type='signal',interface='org.freedesktop.Notifications',"
        "member='NotificationClosed',path='/org/freedesktop/Notifications'");
    GVariant* reply = g_dbus_connection_call_sync(
        monitor, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus.Monitoring", "BecomeMonitor",
        g_variant_new("(@asu)", g_variant_builder_end(&rules), 0),
        G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &error);
    if (reply == nullptr) {
        g_dbus_connection_remove_filter(monitor, filter_id);
        g_object_unref(monitor);
        if (error != nullptr) {
            g_warning("Session bus denied passive notification monitoring: %s",
                      error->message);
            g_error_free(error);
        }
        return;
    }
    g_variant_unref(reply);
    proxy_state_->monitor_connection = monitor;
    proxy_state_->monitor_filter_id = filter_id;
    proxy_state_->on_remote_closed = [this](std::uint32_t id) {
        close_notification(id);
    };
    proxy_state_->on_observed_notify = [this](std::uint32_t serial,
                                               const std::string& app,
                                               const std::string& summary,
                                               const std::string& body,
                                               std::uint32_t replaces_id) {
        std::uint32_t local_replaces_id = 0;
        const auto existing = proxy_state_->observed_local_by_remote.find(replaces_id);
        if (replaces_id != 0 && existing != proxy_state_->observed_local_by_remote.end()) {
            local_replaces_id = existing->second;
        }
        const std::uint32_t local_id = add_notification(
            app, summary, body, local_replaces_id);
        proxy_state_->local_by_call_serial.insert_or_assign(serial, local_id);
    };
    proxy_state_->on_observed_reply = [this](std::uint32_t reply_serial,
                                              std::uint32_t remote_id) {
        const auto pending = proxy_state_->local_by_call_serial.find(reply_serial);
        if (pending == proxy_state_->local_by_call_serial.end()) {
            return;
        }
        if (remote_id != 0) {
            proxy_state_->observed_local_by_remote.insert_or_assign(
                remote_id, pending->second);
        }
        proxy_state_->local_by_call_serial.erase(pending);
    };
    passive_monitoring_ = true;
    pending_change_ = true;
}

void NotificationsModule::stop_passive_monitor()
{
    if (proxy_state_->monitor_connection != nullptr) {
        if (proxy_state_->monitor_filter_id != 0) {
            g_dbus_connection_remove_filter(proxy_state_->monitor_connection,
                                            proxy_state_->monitor_filter_id);
            proxy_state_->monitor_filter_id = 0;
        }
        g_dbus_connection_close_sync(proxy_state_->monitor_connection, nullptr, nullptr);
        g_object_unref(proxy_state_->monitor_connection);
        proxy_state_->monitor_connection = nullptr;
    }
    passive_monitoring_ = false;
    proxy_state_->local_by_call_serial.clear();
    proxy_state_->on_remote_closed = {};
    proxy_state_->on_observed_notify = {};
    proxy_state_->on_observed_reply = {};
}

std::uint32_t NotificationsModule::add_notification(const std::string& app, const std::string& summary,
                                                    const std::string& body, std::uint32_t replaces_id)
{
    std::string text = (app.empty() ? "Notification" : app) + ": " + summary;
    if (!body.empty()) {
        text += "\n" + body;
    }
    auto existing = std::find_if(entries_.begin(), entries_.end(), [replaces_id](const Entry& entry) {
        return replaces_id != 0 && entry.id == replaces_id;
    });
    if (existing != entries_.end()) {
        existing->text = std::move(text);
        pending_change_ = true;
        return existing->id;
    } else {
        const std::uint32_t id = next_id_++;
        entries_.push_back({id, std::move(text)});
        ++unread_;
        if (entries_.size() > 50) {
            entries_.erase(entries_.begin());
        }
        pending_change_ = true;
        return id;
    }
}

void NotificationsModule::close_notification(std::uint32_t id)
{
    const auto old_size = entries_.size();
    std::erase_if(entries_, [id](const Entry& entry) { return entry.id == id; });
    if (entries_.size() != old_size) {
        pending_change_ = true;
    }
}

std::string NotificationsModule::history_text() const
{
    if (entries_.empty()) {
        return "No notifications";
    }
    std::string text = "Recent notifications";
    const std::size_t start = entries_.size() > 10 ? entries_.size() - 10 : 0;
    for (std::size_t index = entries_.size(); index > start; --index) {
        text += "\n" + entries_[index - 1].text;
    }
    return text;
}
#pragma once

#include "panel/module.hpp"

#include <gio/gio.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct NotificationProxyState;

class NotificationsModule final : public IModule {
public:
    using PopupCallback = std::function<void(const std::string&, int, int)>;

    explicit NotificationsModule(PopupCallback open_history,
                                 std::string proxy_destination = {}, bool vertical = false);
    ~NotificationsModule() override;
    void handle_dbus_method_call(const gchar* method, GVariant* parameters,
                                 GDBusMethodInvocation* invocation);
    std::uint32_t add_notification(const std::string& app, const std::string& summary,
                                   const std::string& body, std::uint32_t replaces_id);
    void close_notification(std::uint32_t id);
    [[nodiscard]] bool update() override;
    int measure_width(cairo_t* cr, int panel_height) override;
    void render(cairo_t* cr, const Geometry& geometry) override;
    void on_click(int button, int rel_x, int rel_y) override;
    [[nodiscard]] std::string tooltip_text() const override;

private:
    struct Entry {
        std::uint32_t id;
        std::string text;
    };

    PopupCallback open_history_;
    std::string proxy_destination_;
    std::shared_ptr<NotificationProxyState> proxy_state_;
    std::vector<Entry> entries_;
    std::uint32_t next_id_{1};
    int unread_{0};
    guint owner_id_{0};
    guint registration_id_{0};
    GDBusNodeInfo* node_info_{nullptr};
    bool service_owned_{false};
    bool passive_monitoring_{false};
    bool pending_change_{false};
    bool vertical_{false};
    Geometry geometry_;

    void setup_service();
    void start_passive_monitor();
    void stop_passive_monitor();
    [[nodiscard]] std::string history_text() const;
};
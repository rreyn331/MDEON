#pragma once

#include "panel/backend.hpp"
#include "panel/module.hpp"

#include <glib.h>

#include <map>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class DesktopHubModule final : public IModule {
public:
    DesktopHubModule(IBackend& backend, std::map<std::string, std::string> config);
    ~DesktopHubModule() override;
    [[nodiscard]] bool update() override;
    int measure_width(cairo_t* cr, int panel_height) override;
    void render(cairo_t* cr, const Geometry& geometry) override;
    void on_click(int button, int rel_x, int rel_y) override;
    [[nodiscard]] std::string tooltip_text() const override;

private:
    struct Item {
        enum class Kind { Application, Place, Path, Uri, Terminal, Power };
                Item() = default;
                Item(std::string label_value, std::string target_value, std::string icon_value,
                         Kind item_kind, std::string searchable_value = {},
                         std::string categories_value = {})
                        : label(std::move(label_value)), target(std::move(target_value)),
                            icon_name(std::move(icon_value)), kind(item_kind),
                            searchable(std::move(searchable_value)), categories(std::move(categories_value))
                {
                }

        std::string label;
        std::string target;
        std::string icon_name;
        Kind kind{Kind::Application};
        std::string searchable;
        std::string categories;
    };

    IBackend& backend_;
    std::map<std::string, std::string> config_;
    std::string icon_name_;
    Geometry geometry_;
    std::vector<Item> applications_;
    std::vector<Item> places_;
    std::vector<Item> visible_items_;
    std::vector<Item> base_results_;
    std::vector<Item> file_results_;
    std::vector<std::string> application_commands_;
    std::shared_ptr<std::atomic<bool>> alive_{std::make_shared<std::atomic<bool>>(true)};
    std::shared_ptr<std::atomic<std::uint64_t>> search_generation_{
        std::make_shared<std::atomic<std::uint64_t>>(0)};
    guint file_search_source_id_{0};
    std::string current_query_;
    bool confirming_power_{false};
    std::string pending_power_action_;
    std::string pending_power_label_;
    int confirmation_seconds_{0};
    std::chrono::steady_clock::time_point next_confirmation_tick_{};

    void load_applications();
    void load_places();
    void search(const std::string& query);
    void activate(std::size_t index);
    void clear_power_confirmation();
    void refresh_display(std::size_t selected = 0, bool preserve_selection = false);
    void schedule_file_search(const std::string& query, std::uint64_t generation);
    void start_file_search(const std::string& query, std::uint64_t generation);
    [[nodiscard]] bool execute_power_action(const std::string& action);
    [[nodiscard]] std::vector<HubDisplayItem> display_items() const;
    [[nodiscard]] std::string setting(const std::string& key,
                                     const std::string& fallback = {}) const;
};
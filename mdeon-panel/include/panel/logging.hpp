#pragma once

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <utility>

inline std::chrono::steady_clock::time_point& panel_debug_start_time()
{
    static auto started = std::chrono::steady_clock::now();
    return started;
}

inline std::mutex& panel_debug_output_mutex()
{
    static std::mutex mutex;
    return mutex;
}

inline bool panel_debug_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MDEON_PANEL_DEBUG");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

template <typename... Values>
inline void panel_debug(Values&&... values)
{
    if (!panel_debug_enabled()) {
        return;
    }
    std::lock_guard lock(panel_debug_output_mutex());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - panel_debug_start_time()).count();
    std::clog << "[mdeon-panel +" << elapsed << "ms] ";
    (std::clog << ... << std::forward<Values>(values));
    std::clog << '\n';
}
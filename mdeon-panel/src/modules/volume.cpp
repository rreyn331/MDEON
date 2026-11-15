#include "panel/modules/volume.hpp"

#include "panel/icon_theme.hpp"
#include "panel/modules/system_commands.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <regex>
#include <system_error>

namespace {

std::filesystem::path audio_state_path()
{
    return std::filesystem::path(g_get_user_state_dir()) / "mdeon-panel/audio-state";
}

bool load_audio_state(int& volume, bool& muted)
{
    std::ifstream input(audio_state_path());
    int stored_muted = 0;
    if (!(input >> volume >> stored_muted) || volume < 0 || volume > 150 ||
        (stored_muted != 0 && stored_muted != 1)) {
        return false;
    }
    muted = stored_muted != 0;
    return true;
}

void save_audio_state(int volume, bool muted)
{
    const auto path = audio_state_path();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return;
    }

    auto temporary_path = path;
    temporary_path += ".tmp";
    std::ofstream output(temporary_path, std::ios::trunc);
    if (!output) {
        return;
    }
    output << std::clamp(volume, 0, 150) << ' ' << (muted ? 1 : 0) << '\n';
    output.close();
    if (!output) {
        std::filesystem::remove(temporary_path, error);
        return;
    }
    std::filesystem::rename(temporary_path, path, error);
    if (error) {
        std::filesystem::remove(temporary_path, error);
    }
}

}

bool VolumeModule::update()
{
    const auto now = std::chrono::steady_clock::now();
    if (now < next_refresh_) {
        return false;
    }
    next_refresh_ = now + std::chrono::seconds(5);
    const Backend previous_backend = backend_;
    const int previous_volume = volume_;
    const bool previous_muted = muted_;
    if (backend_ == Backend::None) {
        detect_backend();
    }
    if (backend_ != Backend::None && !restore_attempted_) {
        restore_attempted_ = true;
        int saved_volume = 0;
        bool saved_muted = false;
        if (load_audio_state(saved_volume, saved_muted)) {
            SystemCommands::Result volume_result;
            SystemCommands::Result mute_result;
            const std::string volume_value = std::to_string(saved_volume) + "%";
            switch (backend_) {
            case Backend::PulseAudio:
                volume_result = SystemCommands::run(
                    {"pactl", "set-sink-volume", "@DEFAULT_SINK@", volume_value});
                mute_result = SystemCommands::run(
                    {"pactl", "set-sink-mute", "@DEFAULT_SINK@", saved_muted ? "true" : "false"});
                break;
            case Backend::PipeWire:
                volume_result = SystemCommands::run(
                    {"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", volume_value});
                mute_result = SystemCommands::run(
                    {"wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", saved_muted ? "1" : "0"});
                break;
            case Backend::Alsa:
                volume_result = SystemCommands::run(
                    {"amixer", "-q", "set", "Master", volume_value});
                mute_result = SystemCommands::run(
                    {"amixer", "-q", "set", "Master", saved_muted ? "mute" : "unmute"});
                break;
            case Backend::None:
                break;
            }
            restore_pending_ = volume_result.exit_code != 0 || mute_result.exit_code != 0;
        }
    }

    SystemCommands::Result volume;
    SystemCommands::Result mute;
    switch (backend_) {
    case Backend::PulseAudio:
        volume = SystemCommands::run({"pactl", "get-sink-volume", "@DEFAULT_SINK@"});
        mute = SystemCommands::run({"pactl", "get-sink-mute", "@DEFAULT_SINK@"});
        muted_ = mute.output.find("yes") != std::string::npos;
        break;
    case Backend::PipeWire:
        volume = SystemCommands::run({"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"});
        muted_ = volume.output.find("MUTED") != std::string::npos;
        break;
    case Backend::Alsa:
        volume = SystemCommands::run({"amixer", "get", "Master"});
        muted_ = volume.output.find("[off]") != std::string::npos;
        break;
    case Backend::None:
        return previous_backend != backend_ || previous_volume != volume_ ||
            previous_muted != muted_;
    }

    std::smatch match;
    if (backend_ == Backend::PipeWire &&
        std::regex_search(volume.output, match, std::regex(R"(Volume:\s*([0-9.]+))"))) {
        volume_ = static_cast<int>(std::round(std::stod(match[1].str()) * 100.0));
    } else if (backend_ == Backend::Alsa &&
               std::regex_search(volume.output, match, std::regex(R"(\[(\d+)%\])"))) {
        volume_ = std::stoi(match[1].str());
    } else if (backend_ == Backend::PulseAudio &&
               std::regex_search(volume.output, match, std::regex(R"((\d+)%)"))) {
        volume_ = std::stoi(match[1].str());
    }
    volume_ = std::clamp(volume_, 0, 150);
    if (!restore_pending_) {
        persist_state();
    }
    return previous_backend != backend_ || previous_volume != volume_ ||
        previous_muted != muted_;
}

int VolumeModule::measure_width(cairo_t*, int)
{
    return 30;
}

void VolumeModule::render(cairo_t* cr, const Geometry& geometry)
{
    const char* icon_name = "dialog-warning-symbolic";
    if (backend_ != Backend::None) {
        if (muted_ || volume_ <= 0) {
            icon_name = "audio-volume-muted-symbolic";
        } else if (volume_ < 33) {
            icon_name = "audio-volume-low-symbolic";
        } else if (volume_ < 66) {
            icon_name = "audio-volume-medium-symbolic";
        } else {
            icon_name = "audio-volume-high-symbolic";
        }
    }
    paint_theme_icon(cr, icon_name, geometry.x + geometry.width / 2.0,
                     geometry.y + geometry.height / 2.0, 28);
}

void VolumeModule::on_click(int button, int, int)
{
    if (button == 1) {
        SystemCommands::launch_first({
            {"pavucontrol"},
            {"pwvucontrol"},
            {"pavucontrol-qt"},
            {"gnome-control-center", "sound"},
            {"mate-volume-control"},
            {"xfce4-mixer"},
            {"systemsettings", "kcm_audio"}
        });
    } else if (button == 2 || button == 3) {
        toggle_mute();
        next_refresh_ = {};
    } else if (button == 4) {
        adjust(5);
        next_refresh_ = {};
    } else if (button == 5) {
        adjust(-5);
        next_refresh_ = {};
    }
}

std::string VolumeModule::tooltip_text() const
{
    if (backend_ == Backend::None) {
        return "Audio controls unavailable";
    }
    return std::string(muted_ ? "Muted" : "Volume") + " - " +
        std::to_string(volume_) + "%\nLeft-click: volume controls\nRight-click: mute\nScroll: adjust volume";
}

void VolumeModule::detect_backend()
{
    if (SystemCommands::run({"pactl", "get-sink-volume", "@DEFAULT_SINK@"}).exit_code == 0) {
        backend_ = Backend::PulseAudio;
    } else if (SystemCommands::run(
                   {"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"}).exit_code == 0) {
        backend_ = Backend::PipeWire;
    } else if (SystemCommands::run({"amixer", "get", "Master"}).exit_code == 0) {
        backend_ = Backend::Alsa;
    }
}

void VolumeModule::adjust(int delta)
{
    SystemCommands::Result result;
    switch (backend_) {
    case Backend::PulseAudio:
        result = SystemCommands::run({"pactl", "set-sink-volume", "@DEFAULT_SINK@",
                                      (delta > 0 ? "+" : "") + std::to_string(delta) + "%"});
        break;
    case Backend::PipeWire:
        result = SystemCommands::run({"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@",
                                      std::to_string(std::abs(delta)) + "%" + (delta > 0 ? "+" : "-")});
        break;
    case Backend::Alsa:
        result = SystemCommands::run({"amixer", "-q", "set", "Master",
                                      std::to_string(std::abs(delta)) + "%" + (delta > 0 ? "+" : "-")});
        break;
    case Backend::None:
        break;
    }
    if (result.exit_code == 0) {
        volume_ = std::clamp(volume_ + delta, 0, 150);
        restore_pending_ = false;
        persist_state();
    }
}

void VolumeModule::toggle_mute()
{
    const bool muted = !muted_;
    SystemCommands::Result result;
    switch (backend_) {
    case Backend::PulseAudio:
        result = SystemCommands::run(
            {"pactl", "set-sink-mute", "@DEFAULT_SINK@", muted ? "true" : "false"});
        break;
    case Backend::PipeWire:
        result = SystemCommands::run(
            {"wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", muted ? "1" : "0"});
        break;
    case Backend::Alsa:
        result = SystemCommands::run(
            {"amixer", "-q", "set", "Master", muted ? "mute" : "unmute"});
        break;
    case Backend::None:
        break;
    }
    if (result.exit_code == 0) {
        muted_ = muted;
        restore_pending_ = false;
        persist_state();
    }
}

void VolumeModule::persist_state() const
{
    save_audio_state(volume_, muted_);
}
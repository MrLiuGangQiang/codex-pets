#pragma once

#include "types.h"

#include <filesystem>
#include <optional>
#include <string_view>

namespace codexpets {

struct XiaoAiSettings {
    bool enabled{};
    // Saved in the OS credential store, never in settings.json.
    std::string auth_cookies;
    std::string device_id;
    bool notify_started{true};
    bool notify_completed{true};
    bool notify_error{true};
    bool notify_interrupted{true};
};

struct AppSettings {
    int dock_hover_height{240};
    int dock_idle_hide_seconds{10};
    int dock_reveal_seconds{3};
    int dock_notification_seconds{5};
    bool sound_enabled{true};
    bool pet_visible{true};
    std::filesystem::path sessions_root;
    std::optional<PetPositionState> pet_position;
    XiaoAiSettings xiaoai;

    AppSettings();
    void normalize();
};

class JsonSettingsStore {
public:
    explicit JsonSettingsStore(std::filesystem::path settings_file = {});

    [[nodiscard]] const std::filesystem::path& settings_file_path() const noexcept {
        return settings_file_;
    }
    [[nodiscard]] AppSettings load() const noexcept;
    bool save(AppSettings settings, std::string* error = nullptr) const noexcept;

private:
    std::filesystem::path settings_file_;
};

std::optional<PetPositionState> deserialize_legacy_macos_position(std::string_view json) noexcept;

} // namespace codexpets

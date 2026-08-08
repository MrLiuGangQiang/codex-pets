#include "settings.h"

#include "json.h"
#include "paths.h"
#include "platform_text.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace codexpets {
namespace {

const JsonValue* property(const JsonValue& object, std::string_view name) noexcept {
    return object.get_ascii_case_insensitive(name);
}

bool read_bool(const JsonValue& object, std::string_view name, bool fallback) noexcept {
    const auto* value = property(object, name);
    return value && value->is_boolean() ? value->boolean() : fallback;
}

int read_int(const JsonValue& object, std::string_view name, int fallback) noexcept {
    const auto* value = property(object, name);
    return value ? value->int_or(fallback) : fallback;
}

std::string read_string(const JsonValue& object, std::string_view name,
                        std::string fallback = {}) {
    const auto* value = property(object, name);
    return value && value->is_string() ? value->string() : std::move(fallback);
}

std::vector<std::string> read_string_array(const JsonValue& object, std::string_view name) {
    const auto* value = property(object, name);
    if (!value || !value->is_array()) return {};
    std::vector<std::string> result;
    result.reserve(value->array().size());
    for (const auto& item : value->array()) {
        if (item.is_string()) result.push_back(item.string());
    }
    return result;
}

std::optional<DockEdge> parse_dock_edge(const JsonValue* value) noexcept {
    if (!value) return std::nullopt;
    if (value->is_number()) {
        const auto raw = value->int_or(-1);
        if (raw >= 0 && raw <= 2) return static_cast<DockEdge>(raw);
    }
    if (value->is_string()) {
        const auto text = lowercase_ascii(value->string());
        if (text == "none") return DockEdge::None;
        if (text == "left") return DockEdge::Left;
        if (text == "right") return DockEdge::Right;
    }
    return std::nullopt;
}

std::optional<PetPositionState> parse_position(const JsonValue* value,
                                                bool mirror_macos_y) noexcept {
    if (!value || !value->is_object()) return std::nullopt;
    const auto edge = parse_dock_edge(property(*value, "DockEdge"));
    const auto* x = property(*value, "RelativeX");
    const auto* y = property(*value, "RelativeY");
    const auto* screen = property(*value, "ScreenIdentifier");
    if (!edge || !x || !y || !screen || !x->is_number() || !y->is_number() ||
        !screen->is_string() || !std::isfinite(x->number()) || !std::isfinite(y->number())) {
        return std::nullopt;
    }
    PetPositionState result{*edge, screen->string(), x->number(),
                            mirror_macos_y ? 1.0 - y->number() : y->number()};
    result.normalize();
    return result;
}

std::string serialize_settings(const AppSettings& settings) {
    const auto& xiaoai = settings.xiaoai;
    const auto& telegram = settings.telegram;
    std::ostringstream stream;
    stream << "{\n"
           << "  \"DockHoverHeight\": " << settings.dock_hover_height << ",\n"
           << "  \"DockIdleHideSeconds\": " << settings.dock_idle_hide_seconds << ",\n"
           << "  \"DockRevealSeconds\": " << settings.dock_reveal_seconds << ",\n"
           << "  \"DockNotificationSeconds\": " << settings.dock_notification_seconds << ",\n"
           << "  \"SoundEnabled\": " << (settings.sound_enabled ? "true" : "false") << ",\n"
           << "  \"XiaoAi\": {\n"
           << "    \"Enabled\": " << (xiaoai.enabled ? "true" : "false") << ",\n"
           << "    \"DeviceId\": \"" << json_escape(xiaoai.device_id) << "\",\n"
           << "    \"DeviceIds\": [";
    for (std::size_t index = 0; index < xiaoai.device_ids.size(); ++index) {
        if (index != 0) stream << ", ";
        stream << "\"" << json_escape(xiaoai.device_ids[index]) << "\"";
    }
    stream << "],\n"
           << "    \"MaxParallelRequests\": " << xiaoai.max_parallel_requests << ",\n"
           << "    \"NotifyStarted\": " << (xiaoai.notify_started ? "true" : "false") << ",\n"
           << "    \"NotifyCompleted\": " << (xiaoai.notify_completed ? "true" : "false") << ",\n"
           << "    \"NotifyError\": " << (xiaoai.notify_error ? "true" : "false") << ",\n"
           << "    \"NotifyInterrupted\": " << (xiaoai.notify_interrupted ? "true" : "false") << "\n"
           << "  },\n"
           << "  \"Telegram\": {\n"
           << "    \"Enabled\": " << (telegram.enabled ? "true" : "false") << ",\n"
           << "    \"ChatId\": \"" << json_escape(telegram.chat_id) << "\",\n"
           << "    \"NotifyStarted\": " << (telegram.notify_started ? "true" : "false") << ",\n"
           << "    \"NotifyCompleted\": " << (telegram.notify_completed ? "true" : "false") << ",\n"
           << "    \"NotifyError\": " << (telegram.notify_error ? "true" : "false") << ",\n"
           << "    \"NotifyInterrupted\": " << (telegram.notify_interrupted ? "true" : "false") << "\n"
           << "  },\n"
           << "  \"PetVisible\": " << (settings.pet_visible ? "true" : "false") << ",\n"
           << "  \"SessionsRoot\": \"" << json_escape(path_to_utf8(settings.sessions_root)) << "\",\n"
           << "  \"PetPosition\": ";
    if (!settings.pet_position) {
        stream << "null\n";
    } else {
        const auto& position = *settings.pet_position;
        stream << "{\n"
               << "    \"DockEdge\": " << static_cast<int>(position.dock_edge) << ",\n"
               << "    \"ScreenIdentifier\": \"" << json_escape(position.screen_identifier) << "\",\n"
               << "    \"RelativeX\": " << position.relative_x << ",\n"
               << "    \"RelativeY\": " << position.relative_y << "\n"
               << "  }\n";
    }
    stream << "}\n";
    return stream.str();
}

bool replace_file(const std::filesystem::path& temporary,
                  const std::filesystem::path& destination,
                  std::string* error) {
#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    if (error) *error = "MoveFileExW failed: " + std::to_string(GetLastError());
    return false;
#else
    std::error_code ec;
    std::filesystem::rename(temporary, destination, ec);
    if (!ec) return true;
    if (error) *error = ec.message();
    return false;
#endif
}

} // namespace

AppSettings::AppSettings() : sessions_root(paths::default_sessions_root()) {}

void AppSettings::normalize() {
    dock_hover_height = std::clamp(dock_hover_height, 40, 1000);
    dock_idle_hide_seconds = std::clamp(dock_idle_hide_seconds, 0, 3600);
    dock_reveal_seconds = std::clamp(dock_reveal_seconds, 1, 60);
    dock_notification_seconds = std::clamp(dock_notification_seconds, 1, 120);
    sessions_root = paths::normalize_sessions_root(path_to_utf8(sessions_root));
    std::vector<std::string> normalized_devices;
    normalized_devices.reserve(xiaoai.device_ids.size() + (xiaoai.device_id.empty() ? 0 : 1));
    std::unordered_set<std::string> seen;
    const auto append_device = [&](const std::string& value) {
        if (!value.empty() && seen.insert(value).second) normalized_devices.push_back(value);
    };
    for (const auto& value : xiaoai.device_ids) append_device(value);
    if (normalized_devices.empty()) append_device(xiaoai.device_id);
    xiaoai.device_ids = std::move(normalized_devices);
    xiaoai.device_id = xiaoai.device_ids.empty() ? std::string{} : xiaoai.device_ids.front();
    xiaoai.max_parallel_requests = std::clamp(xiaoai.max_parallel_requests, 1, 8);
    telegram.chat_id = trim_ascii(telegram.chat_id);
    if (pet_position) pet_position->normalize();
}

JsonSettingsStore::JsonSettingsStore(std::filesystem::path settings_file)
    : settings_file_(settings_file.empty()
        ? paths::application_data_directory() / "settings.json"
        : std::filesystem::absolute(std::move(settings_file)).lexically_normal()) {}

AppSettings JsonSettingsStore::load() const noexcept {
    AppSettings result;
    try {
        std::ifstream stream(settings_file_, std::ios::binary);
        if (!stream) return result;
        std::string contents((std::istreambuf_iterator<char>(stream)),
                             std::istreambuf_iterator<char>());
        const auto root = parse_json(contents);
        if (!root.is_object()) return result;
        result.dock_hover_height = read_int(root, "DockHoverHeight", result.dock_hover_height);
        result.dock_idle_hide_seconds = read_int(root, "DockIdleHideSeconds", result.dock_idle_hide_seconds);
        result.dock_reveal_seconds = read_int(root, "DockRevealSeconds", result.dock_reveal_seconds);
        result.dock_notification_seconds = read_int(root, "DockNotificationSeconds", result.dock_notification_seconds);
        result.sound_enabled = read_bool(root, "SoundEnabled", result.sound_enabled);
        result.pet_visible = read_bool(root, "PetVisible", result.pet_visible);
        if (const auto* xiaoai = property(root, "XiaoAi"); xiaoai && xiaoai->is_object()) {
            result.xiaoai.enabled = read_bool(*xiaoai, "Enabled", result.xiaoai.enabled);
            result.xiaoai.device_id = read_string(*xiaoai, "DeviceId");
            result.xiaoai.device_ids = read_string_array(*xiaoai, "DeviceIds");
            result.xiaoai.max_parallel_requests = read_int(
                *xiaoai, "MaxParallelRequests", result.xiaoai.max_parallel_requests);
            result.xiaoai.notify_started = read_bool(*xiaoai, "NotifyStarted", result.xiaoai.notify_started);
            result.xiaoai.notify_completed = read_bool(*xiaoai, "NotifyCompleted", result.xiaoai.notify_completed);
            result.xiaoai.notify_error = read_bool(*xiaoai, "NotifyError", result.xiaoai.notify_error);
            result.xiaoai.notify_interrupted = read_bool(*xiaoai, "NotifyInterrupted", result.xiaoai.notify_interrupted);
        }
        if (const auto* telegram = property(root, "Telegram"); telegram && telegram->is_object()) {
            result.telegram.enabled = read_bool(*telegram, "Enabled", result.telegram.enabled);
            result.telegram.chat_id = read_string(*telegram, "ChatId");
            result.telegram.notify_started = read_bool(
                *telegram, "NotifyStarted", result.telegram.notify_started);
            result.telegram.notify_completed = read_bool(
                *telegram, "NotifyCompleted", result.telegram.notify_completed);
            result.telegram.notify_error = read_bool(
                *telegram, "NotifyError", result.telegram.notify_error);
            result.telegram.notify_interrupted = read_bool(
                *telegram, "NotifyInterrupted", result.telegram.notify_interrupted);
        }
        const auto sessions = read_string(root, "SessionsRoot");
        if (!sessions.empty()) result.sessions_root = path_from_utf8(sessions);
        result.pet_position = parse_position(property(root, "PetPosition"), false);
        result.normalize();
    } catch (...) {
        return AppSettings{};
    }
    return result;
}

bool JsonSettingsStore::save(AppSettings settings, std::string* error) const noexcept {
    try {
        settings.normalize();
        std::filesystem::create_directories(settings_file_.parent_path());
        auto temporary = settings_file_;
        temporary += ".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) {
                if (error) *error = "Unable to create settings temporary file";
                return false;
            }
            const auto serialized = serialize_settings(settings);
            stream.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
            stream.flush();
            if (!stream) {
                if (error) *error = "Unable to write settings temporary file";
                return false;
            }
        }
        if (!replace_file(temporary, settings_file_, error)) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    } catch (...) {
        if (error) *error = "Unknown settings error";
        return false;
    }
}

std::optional<PetPositionState> deserialize_legacy_macos_position(std::string_view json) noexcept {
    try {
        const auto root = parse_json(json);
        return parse_position(&root, true);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace codexpets

#include "paths.h"
#include "platform_text.h"

#include <cctype>
#include <system_error>

namespace codexpets::paths {
namespace {

std::filesystem::path absolute_normalized(const std::filesystem::path& input) {
    std::error_code error;
    auto result = std::filesystem::absolute(input, error);
    if (error) return input.lexically_normal();
    return result.lexically_normal();
}

#ifndef _WIN32
bool looks_like_windows_drive_path(std::string_view value) noexcept {
    return value.size() >= 3 && std::isalpha(static_cast<unsigned char>(value[0])) &&
           value[1] == ':' && (value[2] == '\\' || value[2] == '/');
}
#endif

} // namespace

std::filesystem::path default_home() {
    if (const auto configured = environment_utf8(kHomeEnvironmentVariable);
        configured && !trim_ascii(*configured).empty()) {
        return absolute_normalized(path_from_utf8(trim_ascii(*configured)));
    }
    return absolute_normalized(user_home_directory() / ".codex");
}

std::filesystem::path default_sessions_root() {
    return default_home() / "sessions";
}

std::filesystem::path default_config_file() {
    return default_home() / "config.toml";
}

std::filesystem::path normalize_sessions_root(std::string_view value) {
    const auto trimmed = trim_ascii(value);
    if (trimmed.empty()) return default_sessions_root();
#ifdef _WIN32
    if (trimmed.front() == '/' || trimmed.rfind("~/", 0) == 0 || trimmed.rfind("~\\", 0) == 0) {
        return default_sessions_root();
    }
#else
    if (looks_like_windows_drive_path(trimmed)) return default_sessions_root();
#endif
    try {
        return absolute_normalized(path_from_utf8(trimmed));
    } catch (...) {
        return default_sessions_root();
    }
}

std::filesystem::path application_data_directory() {
#ifdef __APPLE__
    return user_home_directory() / "Library" / "Application Support" / "CodeXPets";
#elif defined(_WIN32)
    if (const auto local = environment_utf8("LOCALAPPDATA"); local && !local->empty()) {
        return path_from_utf8(*local) / "CodeXPets";
    }
    return user_home_directory() / "AppData" / "Local" / "CodeXPets";
#else
    return user_home_directory() / ".local" / "share" / "CodeXPets";
#endif
}

} // namespace codexpets::paths

#include "platform_text.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace codexpets {

#ifdef _WIN32
std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) return {};
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("Invalid UTF-8 text");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (count <= 0) throw std::runtime_error("Invalid UTF-16 text");
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count,
                        nullptr, nullptr);
    return result;
}
#endif

std::filesystem::path path_from_utf8(std::string_view value) {
#ifdef _WIN32
    return std::filesystem::path(utf8_to_wide(value));
#else
    return std::filesystem::path(value);
#endif
}

std::string path_to_utf8(const std::filesystem::path& value) {
#ifdef _WIN32
    return wide_to_utf8(value.native());
#else
    return value.string();
#endif
}

std::optional<std::string> environment_utf8(std::string_view name) {
#ifdef _WIN32
    const auto wide_name = utf8_to_wide(name);
    const auto required = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (required == 0) return std::nullopt;
    std::wstring buffer(required, L'\0');
    const auto written = GetEnvironmentVariableW(wide_name.c_str(), buffer.data(), required);
    if (written == 0 || written >= required) return std::nullopt;
    buffer.resize(written);
    return wide_to_utf8(buffer);
#else
    const std::string key(name);
    if (const auto* value = std::getenv(key.c_str())) return std::string(value);
    return std::nullopt;
#endif
}

std::filesystem::path user_home_directory() {
#ifdef _WIN32
    if (const auto value = environment_utf8("USERPROFILE"); value && !value->empty()) {
        return path_from_utf8(*value);
    }
    const auto drive = environment_utf8("HOMEDRIVE").value_or("");
    const auto path = environment_utf8("HOMEPATH").value_or("");
    if (!drive.empty() || !path.empty()) return path_from_utf8(drive + path);
#else
    if (const auto value = environment_utf8("HOME"); value && !value->empty()) {
        return path_from_utf8(*value);
    }
#endif
    return std::filesystem::current_path();
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + 32) : static_cast<char>(ch);
    });
    return value;
}

std::string trim_ascii(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

} // namespace codexpets

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace codexpets {

std::filesystem::path path_from_utf8(std::string_view value);
std::string path_to_utf8(const std::filesystem::path& value);
std::optional<std::string> environment_utf8(std::string_view name);
std::filesystem::path user_home_directory();
std::string lowercase_ascii(std::string value);
std::string trim_ascii(std::string_view value);

#ifdef _WIN32
std::wstring utf8_to_wide(std::string_view value);
std::string wide_to_utf8(std::wstring_view value);
#endif

} // namespace codexpets

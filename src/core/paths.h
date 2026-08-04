#pragma once

#include <filesystem>
#include <string_view>

namespace codexpets::paths {

inline constexpr std::string_view kHomeEnvironmentVariable = "CODEX_HOME";

std::filesystem::path default_home();
std::filesystem::path default_sessions_root();
std::filesystem::path default_config_file();
std::filesystem::path normalize_sessions_root(std::string_view value);
std::filesystem::path application_data_directory();

} // namespace codexpets::paths

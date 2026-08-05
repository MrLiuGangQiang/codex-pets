#pragma once

#include <string>
#include <string_view>

namespace codexpets::windows {

[[nodiscard]] std::string load_xiaoai_authorization();
[[nodiscard]] bool save_xiaoai_authorization(std::string_view cookies, std::string* error);
void remove_legacy_xiaoai_authorization() noexcept;

} // namespace codexpets::windows

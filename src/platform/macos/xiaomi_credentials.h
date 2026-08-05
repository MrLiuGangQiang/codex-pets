#pragma once

#include <string>
#include <string_view>

namespace codexpets::macos {

[[nodiscard]] std::string load_xiaoai_authorization();
[[nodiscard]] bool save_xiaoai_authorization(std::string_view cookies, std::string* error);

} // namespace codexpets::macos

#pragma once

#include <string>
#include <string_view>

namespace codexpets::macos {

[[nodiscard]] std::string load_telegram_bot_token();
[[nodiscard]] bool save_telegram_bot_token(std::string_view token, std::string* error);

} // namespace codexpets::macos

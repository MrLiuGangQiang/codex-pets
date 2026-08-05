#pragma once

#include <functional>
#include <string>

#ifdef __OBJC__
@class NSWindow;
#else
class NSWindow;
#endif

namespace codexpets::macos {

void start_xiaomi_browser_login(
    NSWindow* owner, std::string user_data_folder,
    std::function<void(std::string cookies, std::string error)> completed);

} // namespace codexpets::macos

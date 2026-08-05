#pragma once

#include <functional>
#include <string>

#include <windows.h>

namespace codexpets::windows {

// Opens an embedded Edge WebView2 sign-in window and returns the Xiaomi cookies
// required by MiNA once a serviceToken is issued.
void start_xiaomi_browser_login(HWND owner, std::wstring user_data_folder,
                                std::function<void(std::string cookies, std::string error)> completed);

} // namespace codexpets::windows

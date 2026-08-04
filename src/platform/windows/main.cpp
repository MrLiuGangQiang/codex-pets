#include "native_app.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <string_view>
#include <vector>

using codexpets::windows::NativeApp;

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int count{};
    auto** raw = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::wstring> arguments;
    if (raw) {
        for (int i = 1; i < count; ++i) arguments.emplace_back(raw[i]);
        LocalFree(raw);
    }
    const auto is_utility = std::any_of(arguments.begin(), arguments.end(), [](const std::wstring& value) {
        return value == L"--version" || value == L"--preview" ||
               value == L"--validate-resources" || value == L"--smoke-test" ||
               value == L"--startup-smoke-test" || value == L"--test-sound";
    });
    const auto result = is_utility
        ? NativeApp::run_utility(instance, arguments)
        : NativeApp(instance, std::move(arguments)).run();
    CoUninitialize();
    return result;
}

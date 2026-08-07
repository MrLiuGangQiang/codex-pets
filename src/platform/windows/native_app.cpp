#include "native_app.h"
#include "xiaomi_transport.h"
#include "xiaomi_browser_login.h"
#include "xiaomi_credentials.h"

#include "../../../src/core/app_logic.h"
#include "../../../src/core/monitor_policy.h"
#include "../../../src/core/platform_text.h"
#include "../../../src/core/paths.h"
#include "../../../src/core/presentation.h"
#include "../../../src/core/render_layout.h"
#include "resource_ids.h"

#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <wincrypt.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <unordered_set>


namespace codexpets::windows {
namespace {
constexpr wchar_t kPetClassName[] = L"CodeXPets.NativePet";
constexpr wchar_t kMessageClassName[] = L"CodeXPets.NativeMessage";
constexpr wchar_t kSettingsClassName[] = L"CodeXPets.NativeSettings";
constexpr wchar_t kMutexName[] = L"Local\\CodeXPets.Native.SingleInstance";
constexpr wchar_t kReleaseUrl[] = L"https://github.com/MrLiuGangQiang/codex-pets/releases/latest";
constexpr UINT kMonitorMessage = WM_APP + 20;
constexpr UINT kTrayCallback = WM_APP + 21;
constexpr UINT kXiaoAiResultMessage = WM_APP + 22;
constexpr UINT kSettingsApply = 3001;
constexpr UINT kSettingsCancel = 3002;
constexpr UINT kSettingsDefaults = 3003;
constexpr UINT kSettingsBrowse = 3004;
constexpr UINT kMenuStatus = 4000;
constexpr UINT kMenuPet = 4001;
constexpr UINT kMenuSound = 4002;
constexpr UINT kMenuStartup = 4003;
constexpr UINT kMenuFolder = 4004;
constexpr UINT kMenuSettings = 4005;
constexpr UINT kMenuVersion = 4006;
constexpr UINT kMenuUpdate = 4007;
constexpr UINT kMenuExit = 4008;
constexpr UINT kSettingsHover = 4101;
constexpr UINT kSettingsIdle = 4102;
constexpr UINT kSettingsReveal = 4103;
constexpr UINT kSettingsNotification = 4104;
constexpr UINT kSettingsRoot = 4105;
constexpr UINT kSettingsSound = 4106;
constexpr UINT kSettingsHeader = 4107;
constexpr UINT kSettingsHint = 4108;
constexpr UINT kSettingsXiaoAiEnabled = 4109;
constexpr UINT kSettingsXiaoAiDevice = 4113;
constexpr UINT kSettingsXiaoAiTest = 4114;
constexpr UINT kSettingsXiaoAiLogin = 4115;
constexpr UINT kSettingsXiaoAiScan = 4116;
constexpr UINT kSettingsXiaoAiSelectAll = 4117;
constexpr UINT kSettingsXiaoAiParallel = 4118;

enum class XiaoAiUiOperation : unsigned char { ValidateLogin, ScanDevices, TestNotification };

struct XiaoAiUiResult {
    XiaoAiUiOperation operation;
    HWND owner{};
    XiaoAiSettings settings;
    std::vector<XiaoAiDeviceInfo> devices;
    std::string error;
    bool login_follow_up{};
};

void post_xiaoai_ui_result(HWND message_window, XiaoAiUiResult result) {
    auto* payload = new XiaoAiUiResult(std::move(result));
    if (!message_window || !PostMessageW(message_window, kXiaoAiResultMessage, 0,
                                        reinterpret_cast<LPARAM>(payload))) {
        delete payload;
    }
}

int read_edit_int(HWND parent, int control, int fallback) {
    wchar_t buffer[64]{};
    GetDlgItemTextW(parent, control, buffer, static_cast<int>(std::size(buffer)));
    wchar_t* end = nullptr;
    const auto value = wcstol(buffer, &end, 10);
    return end != buffer ? static_cast<int>(value) : fallback;
}

void set_edit_int(HWND parent, int control, int value) {
    SetDlgItemTextW(parent, control, std::to_wstring(value).c_str());
}

void set_control_font(HWND control) {
    SendMessageW(control, WM_SETFONT,
                 reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

void add_menu_item(HMENU menu, UINT id, std::wstring_view text, bool enabled = true) {
    MENUITEMINFOW info{sizeof(info)};
    info.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE;
    info.wID = id;
    info.dwTypeData = const_cast<wchar_t*>(text.data());
    info.fState = enabled ? MFS_ENABLED : MFS_DISABLED;
    InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &info);
}

void add_menu_separator(HMENU menu) {
    MENUITEMINFOW info{sizeof(info)};
    info.fMask = MIIM_FTYPE;
    info.fType = MFT_SEPARATOR;
    InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &info);
}

std::string decode_base64_utf8(std::wstring_view encoded) {
    DWORD size{};
    if (!CryptStringToBinaryW(encoded.data(), static_cast<DWORD>(encoded.size()),
                              CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr) || size == 0) return {};
    std::string result(size, '\0');
    if (!CryptStringToBinaryW(encoded.data(), static_cast<DWORD>(encoded.size()),
                              CRYPT_STRING_BASE64, reinterpret_cast<BYTE*>(result.data()),
                              &size, nullptr, nullptr)) return {};
    result.resize(size);
    return result;
}

std::string win32_error(std::string_view action, DWORD code);

std::string win32_error(std::string_view action, DWORD code) {
    std::string result(action);
    result += "（Win32 错误 ";
    result += std::to_string(code);
    result += "）";
    return result;
}

bool register_window_class(const WNDCLASSEXW& window_class,
                           std::string_view action,
                           std::string* error) {
    if (RegisterClassExW(&window_class)) return true;
    const auto code = GetLastError();
    if (code == ERROR_CLASS_ALREADY_EXISTS) return true;
    if (error) *error = win32_error(action, code);
    return false;
}

} // namespace

NativeApp::NativeApp(HINSTANCE instance, std::vector<std::wstring> arguments)
    : instance_(instance), arguments_(std::move(arguments)), settings_store_() {}

NativeApp::~NativeApp() { shutdown(); }

std::wstring NativeApp::to_wide(std::string_view value) { return utf8_to_wide(value); }
std::string NativeApp::to_utf8(std::wstring_view value) { return wide_to_utf8(value); }

void NativeApp::write_stdout(std::string_view value) {
    const auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!handle || handle == INVALID_HANDLE_VALUE) return;
    DWORD written{};
    WriteFile(handle, value.data(), static_cast<DWORD>(value.size()), &written, nullptr);
}

int NativeApp::run() {
    std::string error;
    if (!initialize(&error)) {
        if (duplicate_instance_) return 0;
        if (!error.empty()) MessageBoxW(nullptr, to_wide(error).c_str(), L"CodeXPets", MB_ICONERROR | MB_OK);
        return 1;
    }
    MSG message{};
    while (!shutting_down_) {
        const auto result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) break;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    shutdown();
    return 0;
}

bool NativeApp::initialize(std::string* error) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool startup_smoke_test = std::find(arguments_.begin(), arguments_.end(),
                                              L"--startup-smoke-test") != arguments_.end();
    expression_demo_ = std::find(arguments_.begin(), arguments_.end(),
                                 L"--expression-demo") != arguments_.end();
    instance_mutex_ = CreateMutexW(nullptr, FALSE, startup_smoke_test ? nullptr : kMutexName);
    const auto mutex_error = GetLastError();
    if (!instance_mutex_) {
        if (error) *error = win32_error("无法创建单实例锁", mutex_error);
        return false;
    }
    if (!startup_smoke_test && mutex_error == ERROR_ALREADY_EXISTS) {
        duplicate_instance_ = true;
        MessageBoxW(nullptr, L"CodeXPets 已经在运行。", L"CodeXPets", MB_ICONINFORMATION | MB_OK);
        return false;
    }

    if (!std::filesystem::exists(settings_store_.settings_file_path())) {
        AppSettings migrated;
        for (const wchar_t* key_path : {L"Software\\CodeXPets", L"Software\\CodeXPets\\Windows"}) {
            HKEY key{};
            if (RegOpenKeyExW(HKEY_CURRENT_USER, key_path, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
            auto read_int_value = [&](const wchar_t* name, int fallback) {
                DWORD value{}, size = sizeof(value), type{};
                return RegQueryValueExW(key, name, nullptr, &type,
                                        reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
                       (type == REG_DWORD || type == REG_QWORD) ? static_cast<int>(value) : fallback;
            };
            migrated.dock_hover_height = read_int_value(L"DockHoverHeight", migrated.dock_hover_height);
            migrated.dock_idle_hide_seconds = read_int_value(L"DockIdleHideSeconds", migrated.dock_idle_hide_seconds);
            migrated.dock_reveal_seconds = read_int_value(L"DockRevealSeconds", migrated.dock_reveal_seconds);
            migrated.dock_notification_seconds = read_int_value(L"DockNotificationSeconds", migrated.dock_notification_seconds);
            migrated.sound_enabled = read_int_value(L"SoundEnabled", migrated.sound_enabled ? 1 : 0) != 0;
            wchar_t root[4096]{}; DWORD root_size = sizeof(root); DWORD type{};
            if (RegQueryValueExW(key, L"SessionsRoot", nullptr, &type,
                                 reinterpret_cast<BYTE*>(root), &root_size) == ERROR_SUCCESS &&
                (type == REG_SZ || type == REG_EXPAND_SZ) && root[0] != L'\0') {
                migrated.sessions_root = std::filesystem::path(root);
            }
            wchar_t position[4096]{}; DWORD position_size = sizeof(position); type = 0;
            if (RegQueryValueExW(key, L"PetPositionV1", nullptr, &type,
                                 reinterpret_cast<BYTE*>(position), &position_size) == ERROR_SUCCESS &&
                type == REG_SZ && position[0] != L'\0') {
                std::wstring value(position);
                std::vector<std::wstring> parts;
                std::size_t start{};
                while (start <= value.size()) {
                    const auto end = value.find(L';', start);
                    parts.push_back(value.substr(start, end == std::wstring::npos ? end : end - start));
                    if (end == std::wstring::npos) break;
                    start = end + 1;
                }
                if (parts.size() == 5 && parts[0] == L"1") {
                    PetPositionState p;
                    p.dock_edge = parts[1] == L"L" ? DockEdge::Left : parts[1] == L"R" ? DockEdge::Right : DockEdge::None;
                    wchar_t* end = nullptr;
                    p.relative_x = wcstod(parts[3].c_str(), &end);
                    p.relative_y = wcstod(parts[4].c_str(), &end);
                    p.screen_identifier = decode_base64_utf8(parts[2]);
                    if (p.screen_identifier.empty()) p.screen_identifier = to_utf8(parts[2]);
                    p.normalize();
                    migrated.pet_position = p;
                }
            }
            RegCloseKey(key);
        }
        migrated.normalize();
        settings_ = migrated;
        save_settings();
    } else settings_ = settings_store_.load();
    settings_.normalize();
    remove_legacy_xiaoai_authorization();
    settings_.xiaoai.auth_cookies = load_xiaoai_authorization();
    xiaoai_notifier_ = std::make_unique<XiaoAiNotifier>(make_xiaoai_http_transport());
    xiaoai_notifier_->configure(settings_.xiaoai);

    if (!renderer_.initialize(instance_, error)) return false;
    if (!renderer_.validate(error)) return false;
    if (!create_message_window(error) || !create_pet_window(error)) return false;
    create_tray_icon();
    place_default();
    restore_saved_position();
    if (!expression_demo_) start_monitor();
    initialized_ = true;
    ShowWindow(pet_window_, expression_demo_ || settings_.pet_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    SetTimer(pet_window_, timer_id_, 50, nullptr);
    if (expression_demo_) show_expression_demo_state(0, Clock::now());
    else {
        refresh_visual(true);
        render_and_present();
    }
    return true;
}

bool NativeApp::create_message_window(std::string* error) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = message_window_proc;
    wc.hInstance = instance_;
    wc.lpszClassName = kMessageClassName;
    if (!register_window_class(wc, "注册后台消息窗口类失败", error)) return false;
    message_window_ = CreateWindowExW(0, kMessageClassName, L"CodeXPets message", 0,
                                      0, 0, 0, 0, HWND_MESSAGE, nullptr, instance_, this);
    if (!message_window_) {
        const auto code = GetLastError();
        if (error) *error = win32_error("创建后台消息窗口失败", code);
        return false;
    }
    return true;
}

bool NativeApp::create_pet_window(std::string* error) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = pet_window_proc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = renderer_.application_icon();
    wc.lpszClassName = kPetClassName;
    if (!register_window_class(wc, "注册桌宠窗口类失败", error)) return false;
    pet_window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                  kPetClassName, L"CodeXPets", WS_POPUP,
                                  0, 0, Renderer::LogicalWidth, Renderer::LogicalHeight,
                                  nullptr, nullptr, instance_, this);
    if (!pet_window_) {
        const auto code = GetLastError();
        if (error) *error = win32_error("创建桌宠窗口失败", code);
        return false;
    }
    SendMessageW(pet_window_, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(renderer_.application_icon()));
    SendMessageW(pet_window_, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(renderer_.application_icon()));
    return true;
}

void NativeApp::start_monitor() {
    monitor_message_posted_.store(false, std::memory_order_release);
    const auto generation = ++monitor_generation_;
    monitor_worker_ = std::make_unique<MonitorWorker>(settings_.sessions_root,
        [this, generation](std::vector<MonitorEventKind> events, MonitorSnapshot snapshot) {
            if (!pending_updates_.push(PendingMonitorUpdate{
                    generation, std::move(events), std::move(snapshot)})) return;
            const bool post_message =
                !monitor_message_posted_.exchange(true, std::memory_order_acq_rel);
            if (post_message && message_window_) {
                PostMessageW(message_window_, kMonitorMessage, 0, 0);
            }
        });
    monitor_worker_->start();
}

void NativeApp::create_tray_icon() {
    tray_ = NOTIFYICONDATAW{};
    tray_.cbSize = sizeof(tray_);
    tray_.hWnd = message_window_;
    tray_.uID = 1;
    tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray_.uCallbackMessage = kTrayCallback;
    tray_.hIcon = renderer_.tray_icon(ReminderState::Idle, 0);
    lstrcpynW(tray_.szTip, L"CodeXPets · 空闲", ARRAYSIZE(tray_.szTip));
    tray_added_ = Shell_NotifyIconW(NIM_ADD, &tray_) == TRUE;
    if (tray_added_) {
        tray_state_ = ReminderState::Idle;
        tray_frame_ = 0;
        tray_tip_ = tray_.szTip;
    }
}

void NativeApp::remove_tray_icon() noexcept {
    if (tray_added_) Shell_NotifyIconW(NIM_DELETE, &tray_);
    tray_added_ = false;
}

void NativeApp::shutdown() noexcept {
    if (shutdown_complete_) return;
    shutdown_complete_ = true;
    shutting_down_ = true;
    if (pet_window_) KillTimer(pet_window_, timer_id_);
    if (monitor_worker_) monitor_worker_->stop();
    if (xiaoai_notifier_) xiaoai_notifier_->stop();
    if (message_window_) {
        MSG queued{};
        while (PeekMessageW(&queued, message_window_, kXiaoAiResultMessage, kXiaoAiResultMessage, PM_REMOVE)) {
            delete reinterpret_cast<XiaoAiUiResult*>(queued.lParam);
        }
    }
    save_position();
    remove_tray_icon();
    if (settings_window_) DestroyWindow(settings_window_);
    if (pet_window_) DestroyWindow(pet_window_);
    if (message_window_) DestroyWindow(message_window_);
    renderer_.shutdown();
    if (instance_mutex_) CloseHandle(instance_mutex_);
    instance_mutex_ = nullptr;
}

NativeApp::ScreenInfo NativeApp::screen_from_point(POINT point) const {
    ScreenInfo result;
    result.monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info{sizeof(info)};
    if (result.monitor && GetMonitorInfoW(result.monitor, &info)) {
        result.work = info.rcWork;
        result.identifier = screen_identifier(result.monitor, info);
    } else {
        result.work = RECT{0, 0, 1920, 1080};
    }
    result.dpi = pet_window_ ? GetDpiForWindow(pet_window_) : 96;
    if (result.dpi == 0) result.dpi = 96;
    return result;
}

NativeApp::ScreenInfo NativeApp::screen_for_identifier(std::wstring_view identifier) const {
    struct Context { std::wstring wanted; ScreenInfo result; } context{std::wstring(identifier), {}};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        auto& context = *reinterpret_cast<Context*>(data);
        if (context.result.monitor) return FALSE;
        MONITORINFOEXW info{sizeof(info)};
        if (!GetMonitorInfoW(monitor, &info)) return TRUE;
        const auto exact = std::wstring(context.wanted);
        const auto generated = std::wstring(info.szDevice) + L"|" +
            std::to_wstring(info.rcMonitor.left) + L"," + std::to_wstring(info.rcMonitor.top) + L"," +
            std::to_wstring(info.rcMonitor.right - info.rcMonitor.left) + L"," +
            std::to_wstring(info.rcMonitor.bottom - info.rcMonitor.top);
        const auto separator = exact.find(L'|');
        const auto display_name = separator == std::wstring::npos ? exact : exact.substr(0, separator);
        if (exact == generated || display_name == info.szDevice) {
            context.result.monitor = monitor;
            context.result.work = info.rcWork;
            context.result.identifier = generated;
            UINT dpi_x{}, dpi_y{};
            if (GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y) != S_OK) dpi_x = 96;
            context.result.dpi = dpi_x == 0 ? 96 : dpi_x;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    if (!context.result.monitor) {
        POINT point{0, 0};
        return screen_from_point(point);
    }
    return context.result;
}

NativeApp::ScreenInfo NativeApp::dock_screen() const {
    if (dock_screen_cache_key_ != dock_screen_identifier_) {
        dock_screen_cache_key_ = dock_screen_identifier_;
        dock_screen_cache_ = screen_for_identifier(dock_screen_identifier_);
    }
    return dock_screen_cache_;
}

std::wstring NativeApp::screen_identifier(HMONITOR, const MONITORINFOEXW& info) const {
    return std::wstring(info.szDevice) + L"|" +
        std::to_wstring(info.rcMonitor.left) + L"," + std::to_wstring(info.rcMonitor.top) + L"," +
        std::to_wstring(info.rcMonitor.right - info.rcMonitor.left) + L"," +
        std::to_wstring(info.rcMonitor.bottom - info.rcMonitor.top);
}

POINT NativeApp::cursor_position() const {
    POINT point{};
    GetCursorPos(&point);
    return point;
}

void NativeApp::place_default() {
    POINT origin{0, 0};
    const auto screen = screen_from_point(origin);
    const auto dpi = static_cast<double>(screen.dpi) / 96.0;
    const auto width = static_cast<int>(std::lround(Renderer::LogicalWidth * dpi));
    const auto height = static_cast<int>(std::lround(Renderer::LogicalHeight * dpi));
    SetWindowPos(pet_window_, HWND_TOPMOST,
                 std::max(screen.work.left, screen.work.right - width - static_cast<int>(24 * dpi)),
                 std::max(screen.work.top, screen.work.bottom - height - static_cast<int>(24 * dpi)),
                 width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void NativeApp::restore_saved_position() {
    if (!settings_.pet_position) return;
    const auto saved = *settings_.pet_position;
    const auto screen = screen_for_identifier(to_wide(saved.screen_identifier));
    const auto dpi = static_cast<double>(screen.dpi) / 96.0;
    const auto width = static_cast<int>(std::lround(Renderer::LogicalWidth * dpi));
    const auto height = static_cast<int>(std::lround(Renderer::LogicalHeight * dpi));
    dock_edge_ = saved.dock_edge;
    dock_screen_identifier_ = screen.identifier;
    if (dock_edge_ != DockEdge::None) {
        dock_coordinate_ = screen.work.top + static_cast<int>(std::lround(saved.relative_y *
                                                                           (screen.work.bottom - screen.work.top)));
        dock_visibility_ = 1.0;
        update_window_position();
    } else {
        const auto x = screen.work.left + static_cast<int>(std::lround(saved.relative_x *
                                                                         (screen.work.right - screen.work.left))) - width / 2;
        const auto y = screen.work.top + static_cast<int>(std::lround(saved.relative_y *
                                                                         (screen.work.bottom - screen.work.top))) - height;
        SetWindowPos(pet_window_, HWND_TOPMOST,
                     std::clamp(x, screen.work.left, std::max(screen.work.left, screen.work.right - width)),
                     std::clamp(y, screen.work.top, std::max(screen.work.top, screen.work.bottom - height)),
                     width, height, SWP_NOACTIVATE);
    }
}

void NativeApp::update_window_position() {
    if (dock_edge_ == DockEdge::None) return;
    const auto screen = dock_screen();
    const auto scale = static_cast<double>(screen.dpi) / 96.0;
    const auto width = static_cast<int>(std::lround(Renderer::LogicalWidth * scale));
    const auto height = static_cast<int>(std::lround(Renderer::LogicalHeight * scale));
    const bool bubble_below = dock_coordinate_ < screen.work.top + static_cast<int>(std::lround(
        render_layout::dock_bubble_switch_margin * scale));
    const render_layout::State layout{last_visual_state_, dock_edge_, true, bubble_below,
                                      false, dock_visibility_, animation_tick_};
    const auto visible_center = static_cast<int>(std::lround(
        render_layout::dock_pet_center_y(layout) * scale));
    auto y = dock_coordinate_ - visible_center;
    const auto x = dock_edge_ == DockEdge::Left ? screen.work.left : screen.work.right - width;
    y = std::clamp(y, static_cast<int>(screen.work.top),
                   std::max(static_cast<int>(screen.work.top), static_cast<int>(screen.work.bottom) - height));
    SetWindowPos(pet_window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
}

void NativeApp::clamp_to_work_area() {
    RECT rect{};
    GetWindowRect(pet_window_, &rect);
    auto screen = screen_from_point(POINT{rect.left + (rect.right - rect.left) / 2,
                                          rect.top + (rect.bottom - rect.top) / 2});
    const auto width = rect.right - rect.left;
    const auto height = rect.bottom - rect.top;
    const auto x = std::clamp(rect.left, screen.work.left, std::max(screen.work.left, screen.work.right - width));
    const auto y = std::clamp(rect.top, screen.work.top, std::max(screen.work.top, screen.work.bottom - height));
    SetWindowPos(pet_window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
}

void NativeApp::save_position() {
    if (!pet_window_ || !IsWindow(pet_window_)) return;
    RECT rect{};
    GetWindowRect(pet_window_, &rect);
    const POINT center{rect.left + (rect.right - rect.left) / 2, rect.top + (rect.bottom - rect.top) / 2};
    const auto screen = dock_edge_ == DockEdge::None
        ? screen_from_point(center) : dock_screen();
    const auto work_width = std::max(1, static_cast<int>(screen.work.right - screen.work.left));
    const auto work_height = std::max(1, static_cast<int>(screen.work.bottom - screen.work.top));
    PetPositionState position;
    position.dock_edge = dock_edge_;
    position.screen_identifier = to_utf8(screen.identifier);
    if (dock_edge_ == DockEdge::None) {
        position.relative_x = static_cast<double>(center.x - screen.work.left) / work_width;
        position.relative_y = static_cast<double>(rect.bottom - screen.work.top) / work_height;
    } else {
        position.relative_x = dock_edge_ == DockEdge::Left ? 0.0 : 1.0;
        position.relative_y = static_cast<double>(dock_coordinate_ - screen.work.top) / work_height;
    }
    position.normalize();
    settings_.pet_position = position;
    save_settings();
}

void NativeApp::save_settings() {
    std::string ignored;
    settings_store_.save(settings_, &ignored);
}

void NativeApp::handle_monitor_update(const PendingMonitorUpdate& update) {
    if (update.generation != monitor_generation_) return;
    const bool first = !has_snapshot_;
    snapshot_ = update.snapshot;
    has_snapshot_ = true;
    const auto effects = apply_monitor_event_policy(
        visual_coordinator_, snapshot_, update.events, settings_);
    for (const auto& effect : effects) {
        if (effect.reveal_pet) show_pet(settings_.pet_visible);
        if (settings_.sound_enabled && effect.sound) {
            switch (*effect.sound) {
                case SoundCue::Started: play_sound(NotificationSound::Started); break;
                case SoundCue::Completed: play_sound(NotificationSound::Completed); break;
                case SoundCue::Error: play_sound(NotificationSound::Error); break;
                case SoundCue::Interrupted: play_sound(NotificationSound::Interrupted); break;
            }
        }
        if (effect.xiaoai_event) {
            notify_xiaoai(*effect.xiaoai_event, effect.xiaoai_context);
        }
    }
    if (first || !update.events.empty()) refresh_visual(true);
}

void NativeApp::process_monitor_updates() {
    // Clear the posted bit before taking the queue. A worker that races with
    // this method may post an extra wake-up, but can never strand an update.
    monitor_message_posted_.store(false, std::memory_order_release);
    auto updates = pending_updates_.take();
    for (const auto& update : updates) handle_monitor_update(update);
}

void NativeApp::show_expression_demo_state(int index, Clock::time_point now) {
    static constexpr std::array<ReminderState, 5> states{{
        ReminderState::Idle, ReminderState::Busy, ReminderState::Completed,
        ReminderState::Error, ReminderState::Interrupted
    }};
    expression_demo_index_ = std::clamp(index, 0, static_cast<int>(states.size()) - 1);
    expression_demo_next_ = now + std::chrono::seconds(4);
    visual_coordinator_ = VisualStateCoordinator{};
    snapshot_ = MonitorSnapshot{};
    snapshot_.active_titles.clear();
    snapshot_.active_plan_progress_labels.clear();
    snapshot_.active_count = 0;
    const auto state = states[static_cast<std::size_t>(expression_demo_index_)];
    switch (state) {
        case ReminderState::Busy:
            snapshot_.active_count = 1;
            snapshot_.active_titles = {"表情测试：正在认真工作"};
            snapshot_.active_plan_progress_labels = {std::optional<std::string>("2/5")};
            snapshot_.total_plan_step_count = 5;
            snapshot_.completed_plan_step_count = 2;
            visual_coordinator_.record_started();
            break;
        case ReminderState::Completed:
            snapshot_.last_completed_title = "表情测试：任务顺利完成";
            visual_coordinator_.record_completed(now, std::chrono::hours(1));
            break;
        case ReminderState::Error:
            snapshot_.last_aborted_title = "表情测试：任务失败";
            visual_coordinator_.record_aborted(now, std::chrono::hours(1));
            break;
        case ReminderState::Interrupted:
            snapshot_.last_interrupted_title = "表情测试：任务被中断";
            visual_coordinator_.record_interrupted(now, std::chrono::hours(1));
            break;
        case ReminderState::Idle:
        default:
            break;
    }
    dock_last_content_change_ = now;
    dock_hover_reveal_until_ = now + std::chrono::hours(1);
    dock_visibility_ = 1.0;
    refresh_visual(true);
    update_tray_icon();
    render_and_present();
}

void NativeApp::refresh_visual(bool force_text) {
    const auto now = Clock::now();
    const auto visual_state = visual_coordinator_.select(snapshot_.active_count, now);
    if (force_text && dock_edge_ != DockEdge::None && visual_state != ReminderState::Idle) {
        dock_last_content_change_ = now;
        dock_thought_until_ = now + std::chrono::seconds(
            app_logic::cloud_notification_seconds(visual_state, settings_.dock_notification_seconds));
        dock_visibility_ = 1.0;
    }
    std::string previously_selected_title;
    if (selected_task_index_ >= 0 &&
        selected_task_index_ < static_cast<int>(displayed_task_titles_.size())) {
        previously_selected_title = displayed_task_titles_[static_cast<std::size_t>(selected_task_index_)];
    }
    if (visual_state != last_visual_state_) {
        animation_tick_ = 0;
        animation_accumulator_ = 0;
        scroll_offset_ = 0;
        scroll_hold_seconds_ = 1.9;
        scroll_at_end_ = false;
        session_rotation_seconds_ = 0;
        last_visual_state_ = visual_state;
    }

    const auto content = make_visual_content(visual_state, snapshot_);
    const bool select_newest = visual_coordinator_.show_newest_task_on_next_refresh() &&
                               visual_state == ReminderState::Busy;
    const auto preferred = app_logic::select_preferred_task_index(
        select_newest, visual_coordinator_.preferred_task_index());
    selected_task_index_ = app_logic::reconcile_task_selection(
        visual_state, content.task_titles, selected_task_index_, previously_selected_title,
        select_newest, preferred);
    displayed_task_titles_ = content.task_titles;
    displayed_progress_labels_ = content.progress_labels;
    displayed_thought_text_ = content.thought_text;
    if (select_newest) visual_coordinator_.consume_newest_task_focus();

    const auto new_signature = content.status_text + "\x1f" + content.thought_text +
        std::to_string(static_cast<int>(visual_state)) + std::to_string(selected_task_index_);
    if (force_text || new_signature != last_status_signature_) {
        last_status_signature_ = new_signature;
        last_status_text_ = content.status_text;
        last_status_lines_ = content.status_lines;
        update_tray_icon();
    }
    render_and_present();
}

void NativeApp::render_and_present() {
    if (!pet_window_ || !IsWindow(pet_window_)) return;
    const auto dpi = GetDpiForWindow(pet_window_);
    const auto scale = dpi == 0 ? 1.0 : static_cast<double>(dpi) / 96.0;
    RenderState state;
    state.state = last_visual_state_;
    state.docked = dock_edge_ != DockEdge::None;
    state.bubble_visible = app_logic::should_show_thought_bubble(
        state.docked, state.state, Clock::now(), dock_thought_until_);
    state.dock_edge = dock_edge_;
    state.dock_visibility = dock_visibility_;
    state.animation_tick = animation_tick_;
    state.selected_task_index = selected_task_index_;
    state.scroll_offset = scroll_offset_;
    state.status_text = last_status_text_;
    state.thought_text = displayed_thought_text_;
    state.task_titles = displayed_task_titles_;
    state.progress_labels = displayed_progress_labels_;
    RECT rect{};
    GetWindowRect(pet_window_, &rect);
    const auto screen = state.docked
        ? dock_screen()
        : screen_from_point(POINT{rect.left + (rect.right - rect.left) / 2,
                                  rect.top + (rect.bottom - rect.top) / 2});
    state.bubble_below = state.docked && dock_coordinate_ < screen.work.top +
        static_cast<int>(std::lround(render_layout::dock_bubble_switch_margin * scale));
    state.mirror = !state.docked && ((rect.left + rect.right) / 2 <
                                     (screen.work.left + screen.work.right) / 2);
    std::string error;
    if (!renderer_.render(state, scale, &error)) return;
    SIZE size{renderer_.pixel_width(), renderer_.pixel_height()};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(pet_window_, nullptr, nullptr, &size, renderer_.dc(), &source,
                        0, &blend, ULW_ALPHA);
}

void NativeApp::update_tray_icon() {
    if (!tray_added_) return;
    const int frame = last_visual_state_ == ReminderState::Busy
        ? std::abs(animation_tick_ / 2) % 8 : 0;
    const auto tip = to_wide(last_status_text_);
    if (tray_state_ == last_visual_state_ && tray_frame_ == frame && tray_tip_ == tip) return;
    tray_.hIcon = renderer_.tray_icon(last_visual_state_, frame);
    tray_.uFlags = NIF_ICON | NIF_TIP;
    lstrcpynW(tray_.szTip, tip.c_str(), ARRAYSIZE(tray_.szTip));
    if (Shell_NotifyIconW(NIM_MODIFY, &tray_)) {
        tray_state_ = last_visual_state_;
        tray_frame_ = frame;
        tray_tip_ = tip;
    }
}

void NativeApp::on_timer() {
    if (!initialized_) return;
    if (!expression_demo_) process_monitor_updates();
    const auto now = Clock::now();
    if (expression_demo_ && now >= expression_demo_next_) {
        show_expression_demo_state((expression_demo_index_ + 1) % 5, now);
    }
    auto elapsed = std::chrono::duration<double>(now - last_tick_).count();
    last_tick_ = now;
    elapsed = std::clamp(elapsed, 0.001, 0.25);
    animation_accumulator_ += elapsed;
    bool changed = false;
    while (animation_accumulator_ >= 0.12) {
        animation_accumulator_ -= 0.12;
        animation_tick_ = (animation_tick_ + 1) % 6400;
        changed = true;
    }

    bool dock_changed = false;
    if (dock_edge_ != DockEdge::None) {
        const auto cursor = cursor_position();
        const bool hovered_now = dock_hovering(cursor);
        const bool hovered_path = !hovered_now && has_last_hover_cursor_ &&
            dock_hover_path_crosses(last_hover_cursor_, cursor);
        if (hovered_now || hovered_path) {
            dock_hover_reveal_until_ = now + std::chrono::seconds(settings_.dock_reveal_seconds);
        }
        has_last_hover_cursor_ = true;
        last_hover_cursor_ = cursor;
        dock_changed = update_dock_visibility(elapsed, hovered_now || hovered_path);
    }

    const bool bubble_visible = app_logic::should_show_thought_bubble(
        dock_edge_ != DockEdge::None, last_visual_state_, now, dock_thought_until_);
    if (bubble_visible) {
        if (displayed_task_titles_.size() > 1) {
            session_rotation_seconds_ += elapsed;
            if (session_rotation_seconds_ >= 6.0) {
                const auto steps = std::max(1, static_cast<int>(session_rotation_seconds_ / 6.0));
                session_rotation_seconds_ -= steps * 6.0;
                selected_task_index_ = (selected_task_index_ + steps) %
                    static_cast<int>(displayed_task_titles_.size());
                scroll_offset_ = 0;
                scroll_hold_seconds_ = 1.9;
                scroll_at_end_ = false;
                changed = true;
            }
        } else session_rotation_seconds_ = 0;
        const auto text = displayed_task_titles_.empty() ? displayed_thought_text_ :
            displayed_task_titles_[static_cast<std::size_t>(std::clamp(selected_task_index_, 0,
                                      static_cast<int>(displayed_task_titles_.size()) - 1))];
        const auto approximate_lines = std::max(1, static_cast<int>(text.size() / 24) +
                                                   static_cast<int>(std::count(text.begin(), text.end(), '\n')));
        const auto max_scroll = std::max(0.0, (approximate_lines - 3) * 15.0);
        if (max_scroll > 0.0) {
            if (scroll_hold_seconds_ > 0) scroll_hold_seconds_ = std::max(0.0, scroll_hold_seconds_ - elapsed);
            else if (!scroll_at_end_) {
                scroll_offset_ = std::min(max_scroll, scroll_offset_ + 15.0 * elapsed);
                if (scroll_offset_ >= max_scroll) { scroll_at_end_ = true; scroll_hold_seconds_ = 1.7; }
                changed = true;
            } else {
                scroll_offset_ = 0; scroll_hold_seconds_ = 1.9; scroll_at_end_ = false; changed = true;
            }
        } else if (scroll_offset_ != 0.0) { scroll_offset_ = 0; changed = true; }
    } else {
        session_rotation_seconds_ = 0;
        if (scroll_offset_ != 0.0) { scroll_offset_ = 0.0; changed = true; }
    }

    const auto next_state = visual_coordinator_.select(snapshot_.active_count, now);
    if (next_state != last_visual_state_) { refresh_visual(true); return; }
    if (changed || dock_changed) {
        update_tray_icon();
        render_and_present();
    }
}

bool NativeApp::update_dock_visibility(double elapsed_seconds, bool hovering) {
    if (dock_edge_ == DockEdge::None) return false;
    const auto now = Clock::now();
    const auto show = app_logic::should_show_dock(
        dock_last_content_change_, now, dragging_ || drag_pending_, hovering,
        dock_hover_reveal_until_, settings_.dock_idle_hide_seconds);
    const auto target = show ? 1.0 : 0.0;
    const auto duration = target > dock_visibility_ ? 0.30 : 0.55;
    if (std::abs(target - dock_visibility_) < 0.001) {
        const bool changed = dock_visibility_ != target;
        dock_visibility_ = target;
        return changed;
    }
    const auto before = dock_visibility_;
    const auto step = elapsed_seconds / duration;
    dock_visibility_ += (target > dock_visibility_ ? step : -step);
    dock_visibility_ = std::clamp(dock_visibility_, 0.0, 1.0);
    return std::abs(before - dock_visibility_) > 0.0001;
}

std::optional<RectD> NativeApp::dock_hover_rect() const {
    if (dock_edge_ == DockEdge::None) return std::nullopt;
    const auto screen = dock_screen();
    const auto dpi = GetDpiForWindow(pet_window_);
    const auto scale = dpi == 0 ? 1.0 : static_cast<double>(dpi) / 96.0;
    const RectD work{static_cast<double>(screen.work.left),
                     static_cast<double>(screen.work.top),
                     static_cast<double>(screen.work.right - screen.work.left),
                     static_cast<double>(screen.work.bottom - screen.work.top)};
    return app_logic::dock_hover_bounds(
        dock_edge_, work, static_cast<double>(dock_coordinate_), scale,
        dock_visibility_ <= 0.01, settings_.dock_hover_height);
}

bool NativeApp::dock_hovering(POINT cursor) const {
    if (dock_edge_ == DockEdge::None) return false;
    const auto bounds = dock_hover_rect();
    if (bounds && bounds->contains(PointD{static_cast<double>(cursor.x), static_cast<double>(cursor.y)})) {
        return true;
    }
    if (!pet_window_ || !IsWindow(pet_window_)) return false;
    POINT local = cursor;
    if (ScreenToClient(pet_window_, &local)) {
        return renderer_.hit_test_alpha(local.x, local.y);
    }
    return false;
}

bool NativeApp::dock_hover_path_crosses(POINT from, POINT to) const {
    if (dock_edge_ == DockEdge::None) return false;
    const auto bounds = dock_hover_rect();
    if (bounds) {
        const PointD from_point{static_cast<double>(from.x), static_cast<double>(from.y)};
        const PointD to_point{static_cast<double>(to.x), static_cast<double>(to.y)};
        if (app_logic::segment_intersects_rect(from_point, to_point, *bounds)) return true;
    }
    return dock_hovering(from) || dock_hovering(to);
}

void NativeApp::reveal_dock_for_interaction() {
    if (dock_edge_ == DockEdge::None) return;
    const auto now = Clock::now();
    dock_visibility_ = 1.0;
    dock_last_content_change_ = now;
    dock_thought_until_ = last_visual_state_ == ReminderState::Idle
        ? Clock::time_point::min()
        : now + std::chrono::seconds(app_logic::cloud_notification_seconds(
            last_visual_state_, settings_.dock_notification_seconds));
    dock_hover_reveal_until_ = now + std::chrono::seconds(settings_.dock_reveal_seconds);
    render_and_present();
}

void NativeApp::begin_drag(POINT cursor) {
    POINT local = cursor;
    ScreenToClient(pet_window_, &local);
    if (!renderer_.hit_test_alpha(local.x, local.y)) return;
    drag_pending_ = true;
    dragging_ = false;
    drag_started_docked_ = dock_edge_ != DockEdge::None;
    drag_start_cursor_ = cursor;
    last_cursor_ = cursor;
    RECT rect{}; GetWindowRect(pet_window_, &rect);
    drag_start_position_ = POINT{rect.left, rect.top};
    if (drag_started_docked_) { dock_visibility_ = 1.0; render_and_present(); }
    SetCapture(pet_window_);
}

void NativeApp::move_drag(POINT cursor) {
    if (!drag_pending_) return;
    last_cursor_ = cursor;
    auto dx = cursor.x - drag_start_cursor_.x;
    auto dy = cursor.y - drag_start_cursor_.y;
    if (!dragging_ && std::abs(dx) + std::abs(dy) < 4) return;
    if (!dragging_ && dock_edge_ != DockEdge::None) {
        const auto dpi = GetDpiForWindow(pet_window_);
        const auto scale = dpi == 0 ? 1.0 : static_cast<double>(dpi) / 96.0;
        const auto width = static_cast<int>(std::lround(Renderer::LogicalWidth * scale));
        const auto height = static_cast<int>(std::lround(Renderer::LogicalHeight * scale));
        const auto y_offset = std::max(24, static_cast<int>(std::lround(70.0 * scale)));
        const auto new_x = cursor.x - width / 2;
        const auto new_y = cursor.y - height + y_offset;
        SetWindowPos(pet_window_, HWND_TOPMOST, new_x, new_y, width, height, SWP_NOACTIVATE);
        drag_start_cursor_ = cursor;
        drag_start_position_ = POINT{new_x, new_y};
        dx = 0;
        dy = 0;
        dock_edge_ = DockEdge::None;
        dock_screen_identifier_.clear();
        dock_visibility_ = 1.0;
        dock_thought_until_ = Clock::time_point::min();
        dock_hover_reveal_until_ = Clock::time_point::min();
        has_last_hover_cursor_ = false;
    }
    dragging_ = true;
    RECT rect{}; GetWindowRect(pet_window_, &rect);
    SetWindowPos(pet_window_, HWND_TOPMOST, drag_start_position_.x + dx, drag_start_position_.y + dy,
                 rect.right - rect.left, rect.bottom - rect.top, SWP_NOACTIVATE);
    render_and_present();
}

void NativeApp::finish_drag(POINT cursor) {
    if (!drag_pending_) return;
    const auto moved = dragging_;
    const auto started_docked = drag_started_docked_;
    drag_pending_ = false;
    dragging_ = false;
    drag_started_docked_ = false;
    ReleaseCapture();
    if (!moved) {
        if (started_docked) reveal_dock_for_interaction();
        return;
    }
    try_snap_or_clamp(cursor);
    save_position();
}

void NativeApp::try_snap_or_clamp(POINT cursor) {
    const auto screen = screen_from_point(cursor);
    const auto dpi = static_cast<double>(screen.dpi) / 96.0;
    const auto distance = std::max(24.0, 36.0 * dpi);
    const auto edge = app_logic::select_snap_edge(
        PointD{static_cast<double>(cursor.x), static_cast<double>(cursor.y)},
        RectD{static_cast<double>(screen.work.left), static_cast<double>(screen.work.top),
               static_cast<double>(screen.work.right - screen.work.left),
               static_cast<double>(screen.work.bottom - screen.work.top)}, distance);
    if (edge != DockEdge::None) {
        dock_edge_ = edge;
        dock_screen_identifier_ = screen.identifier;
        dock_coordinate_ = cursor.y;
        dock_visibility_ = 1.0;
        dock_last_content_change_ = Clock::now();
        has_last_hover_cursor_ = false;
        update_window_position();
    } else {
        dock_edge_ = DockEdge::None;
        dock_screen_identifier_.clear();
        dock_thought_until_ = Clock::time_point::min();
        dock_hover_reveal_until_ = Clock::time_point::min();
        has_last_hover_cursor_ = false;
        clamp_to_work_area();
    }
    refresh_visual(true);
}

void NativeApp::show_pet(bool visible) {
    const bool changed = settings_.pet_visible != visible;
    settings_.pet_visible = visible;
    if (visible) {
        ShowWindow(pet_window_, SW_SHOWNOACTIVATE);
        SetWindowPos(pet_window_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else ShowWindow(pet_window_, SW_HIDE);
    if (changed) save_settings();
}

void NativeApp::toggle_sound() {
    settings_.sound_enabled = !settings_.sound_enabled;
    save_settings();
}

bool NativeApp::is_autostart_enabled() const {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    wchar_t value[4096]{}; DWORD size = sizeof(value); DWORD type{};
    const auto result = RegQueryValueExW(key, L"CodeXPets", nullptr, &type,
                                         reinterpret_cast<BYTE*>(value), &size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && value[0] != L'\0';
}

void NativeApp::set_autostart_enabled(bool enabled) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    if (enabled) {
        wchar_t path[32768]{};
        const auto length = GetModuleFileNameW(instance_, path, ARRAYSIZE(path));
        std::wstring command = L"\"" + std::wstring(path, length) + L"\"";
        RegSetValueExW(key, L"CodeXPets", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(command.c_str()),
                       static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(key, L"CodeXPets");
    RegCloseKey(key);
}

void NativeApp::toggle_startup() {
    set_autostart_enabled(!is_autostart_enabled());
}

void NativeApp::open_sessions_folder() {
    ShellExecuteW(pet_window_, L"open", settings_.sessions_root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void NativeApp::open_latest_release() {
    ShellExecuteW(pet_window_, L"open", kReleaseUrl, nullptr, nullptr, SW_SHOWNORMAL);
}

std::wstring NativeApp::audio_path(NotificationSound sound) {
    std::wstring name = sound == NotificationSound::Started ? L"voice-start.mp3" :
                        sound == NotificationSound::Completed ? L"voice-complete.mp3" :
                        sound == NotificationSound::Error ? L"voice-error.mp3" : L"voice-interrupted.mp3";
    auto directory = settings_store_.settings_file_path().parent_path() / L"audio";
    const auto path = directory / name;
    if (!std::filesystem::exists(path)) {
        std::string ignored;
        renderer_.extract_audio(sound, path, &ignored);
    }
    return path.native();
}

void NativeApp::notify_xiaoai(XiaoAiEvent event, std::string_view title) {
    if (xiaoai_notifier_) xiaoai_notifier_->notify(event, title);
}

void NativeApp::set_xiaoai_controls_enabled(bool enabled) {
    if (!settings_window_ || !IsWindow(settings_window_)) return;
    for (const UINT id : {kSettingsXiaoAiEnabled, kSettingsXiaoAiDevice, kSettingsXiaoAiTest,
                          kSettingsXiaoAiLogin, kSettingsXiaoAiScan, kSettingsXiaoAiSelectAll,
                          kSettingsXiaoAiParallel}) {
        if (const auto control = GetDlgItem(settings_window_, static_cast<int>(id))) {
            EnableWindow(control, enabled);
        }
    }
}

void NativeApp::open_xiaomi_login() {
    if (xiaoai_operation_in_flight_ || !xiaoai_notifier_) return;
    const auto owner = settings_window_ && IsWindow(settings_window_) ? settings_window_ : pet_window_;
    const auto message_window = message_window_;
    xiaoai_operation_in_flight_ = true;
    set_xiaoai_controls_enabled(false);
    start_xiaomi_browser_login(owner, [this, owner, message_window](std::string cookies, std::string error) {
        if (!error.empty()) {
            post_xiaoai_ui_result(message_window, {XiaoAiUiOperation::ValidateLogin, owner, {}, {},
                std::string("小米登录失败：") + error});
            return;
        }
        if (!xiaoai_notifier_) {
            post_xiaoai_ui_result(message_window, {XiaoAiUiOperation::ValidateLogin, owner, {}, {},
                "小爱播报服务未初始化。"});
            return;
        }
        XiaoAiSettings candidate = settings_.xiaoai;
        candidate.auth_cookies = std::move(cookies);
        xiaoai_notifier_->validate_async(std::move(candidate), [message_window, owner](XiaoAiSettings settings, std::string validation_error) {
            post_xiaoai_ui_result(message_window, {XiaoAiUiOperation::ValidateLogin, owner, std::move(settings), {},
                std::move(validation_error)});
        });
    });
}

void NativeApp::populate_xiaoai_device_selector(HWND hwnd) {
    auto* selector = GetDlgItem(hwnd, kSettingsXiaoAiDevice);
    if (!selector) return;
    SendMessageW(selector, LB_RESETCONTENT, 0, 0);
    std::unordered_set<std::string> selected(settings_.xiaoai.device_ids.begin(),
                                             settings_.xiaoai.device_ids.end());
    if (selected.empty() && !settings_.xiaoai.device_id.empty()) {
        selected.insert(settings_.xiaoai.device_id);
    }
    for (std::size_t i = 0; i < xiaoai_devices_.size(); ++i) {
        const auto& device = xiaoai_devices_[i];
        const auto label = !device.alias.empty() ? device.alias :
            !device.name.empty() ? device.name : device.hardware;
        if (label.empty()) continue;
        const auto index = static_cast<int>(SendMessageW(selector, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(to_wide(label).c_str())));
        if (index == LB_ERR || index == LB_ERRSPACE) continue;
        SendMessageW(selector, LB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(i));
        if (selected.contains(device.id)) {
            SendMessageW(selector, LB_SETSEL, TRUE, static_cast<LPARAM>(index));
        }
    }
    update_xiaoai_device_summary(hwnd);
}

std::vector<std::string> NativeApp::selected_xiaoai_device_ids(HWND hwnd) const {
    std::vector<std::string> result;
    const auto selector = GetDlgItem(hwnd, kSettingsXiaoAiDevice);
    if (!selector) return result;
    const auto selected_count = static_cast<int>(SendMessageW(selector, LB_GETSELCOUNT, 0, 0));
    if (selected_count <= 0) return result;
    std::vector<int> indices(static_cast<std::size_t>(selected_count));
    const auto copied = static_cast<int>(SendMessageW(selector, LB_GETSELITEMS,
        static_cast<WPARAM>(indices.size()), reinterpret_cast<LPARAM>(indices.data())));
    for (int index = 0; index < copied; ++index) {
        const auto data = static_cast<std::size_t>(SendMessageW(selector, LB_GETITEMDATA,
            static_cast<WPARAM>(indices[static_cast<std::size_t>(index)]), 0));
        if (data < xiaoai_devices_.size()) result.push_back(xiaoai_devices_[data].id);
    }
    return result;
}


void NativeApp::update_xiaoai_device_summary(HWND hwnd) {
    const auto selected = selected_xiaoai_device_ids(hwnd);
    std::wstring text;
    if (selected.empty()) text = L"请选择音箱  ▼";
    else if (selected.size() == xiaoai_devices_.size()) text = L"已选择全部音箱  ▼";
    else text = L"已选择 " + std::to_wstring(selected.size()) + L" 台音箱  ▼";
    SetDlgItemTextW(hwnd, kSettingsXiaoAiSelectAll, text.c_str());
}

void NativeApp::show_xiaoai_device_menu(HWND hwnd) {
    const auto button = GetDlgItem(hwnd, kSettingsXiaoAiSelectAll);
    if (!button) return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"全选");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const auto selected = selected_xiaoai_device_ids(hwnd);
    for (std::size_t index = 0; index < xiaoai_devices_.size(); ++index) {
        const auto& device = xiaoai_devices_[index];
        const auto label = !device.alias.empty() ? device.alias : !device.name.empty() ? device.name : device.id;
        const bool checked = std::find(selected.begin(), selected.end(), device.id) != selected.end();
        AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED), 5000 + static_cast<UINT>(index), to_wide(label).c_str());
    }
    RECT rect{}; GetWindowRect(button, &rect);
    const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                        rect.left, rect.bottom, 0, hwnd, nullptr);
    if (command == 1) SendDlgItemMessageW(hwnd, kSettingsXiaoAiDevice, LB_SETSEL, TRUE, -1);
    else if (command >= 5000 && command < 5000 + static_cast<int>(xiaoai_devices_.size())) {
        const auto item = static_cast<int>(command - 5000);
        const auto is_selected = SendDlgItemMessageW(hwnd, kSettingsXiaoAiDevice, LB_GETSEL, item, 0) > 0;
        SendDlgItemMessageW(hwnd, kSettingsXiaoAiDevice, LB_SETSEL, is_selected ? FALSE : TRUE, item);
    }
    DestroyMenu(menu);
    update_xiaoai_device_summary(hwnd);
}

void NativeApp::scan_xiaoai_devices() {
    if (xiaoai_operation_in_flight_ || !xiaoai_notifier_) return;
    const auto owner = settings_window_ && IsWindow(settings_window_) ? settings_window_ : pet_window_;
    xiaoai_operation_in_flight_ = true;
    set_xiaoai_controls_enabled(false);
    xiaoai_notifier_->discover_devices_async(settings_.xiaoai, [message_window = message_window_, owner](
                                                        std::vector<XiaoAiDeviceInfo> devices, std::string error) {
        post_xiaoai_ui_result(message_window, {XiaoAiUiOperation::ScanDevices, owner, {}, std::move(devices),
            std::move(error)});
    });
}

void NativeApp::test_xiaoai() {
    if (xiaoai_operation_in_flight_ || !xiaoai_notifier_) return;
    auto candidate = settings_.xiaoai;
    const auto owner = settings_window_ && IsWindow(settings_window_) ? settings_window_ : pet_window_;
    if (settings_window_ && IsWindow(settings_window_)) {
        candidate.device_ids = selected_xiaoai_device_ids(settings_window_);
        candidate.device_id = candidate.device_ids.empty() ? std::string{} : candidate.device_ids.front();
    }
    xiaoai_operation_in_flight_ = true;
    set_xiaoai_controls_enabled(false);
    xiaoai_notifier_->test_async(std::move(candidate), [message_window = message_window_, owner](std::string error) {
        post_xiaoai_ui_result(message_window, {XiaoAiUiOperation::TestNotification, owner, {}, {}, std::move(error)});
    });
}

void NativeApp::play_sound(NotificationSound sound) {
    const auto path = audio_path(sound);
    mciSendStringW(L"close codexpets_voice", nullptr, 0, nullptr);
    const auto open = std::wstring(L"open \"") + path + L"\" type mpegvideo alias codexpets_voice";
    if (mciSendStringW(open.c_str(), nullptr, 0, nullptr) == 0) {
        mciSendStringW(L"play codexpets_voice", nullptr, 0, nullptr);
    }
}

void NativeApp::show_error(std::wstring title, std::wstring text) {
    MessageBoxW(pet_window_, text.c_str(), title.c_str(), MB_ICONERROR | MB_OK);
}

void NativeApp::show_settings() {
    if (settings_window_ && IsWindow(settings_window_)) {
        ShowWindow(settings_window_, SW_SHOWNORMAL);
        SetForegroundWindow(settings_window_);
        return;
    }
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = settings_window_proc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kSettingsClassName;
    RegisterClassExW(&wc);
    settings_window_ = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW,
                                       kSettingsClassName, L"CodeXPets 设置",
                                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                       CW_USEDEFAULT, CW_USEDEFAULT, 560, 650,
                                       pet_window_, nullptr, instance_, this);
    if (!settings_window_) return;
    ShowWindow(settings_window_, SW_SHOWNORMAL);
    UpdateWindow(settings_window_);
}

HMENU NativeApp::build_menu(bool /*context_menu*/) {
    const auto menu = CreatePopupMenu();
    if (last_status_lines_.empty()) {
        add_menu_item(menu, kMenuStatus, to_wide(last_status_text_), false);
    } else {
        for (const auto& line : last_status_lines_) {
            add_menu_item(menu, kMenuStatus, to_wide(line), false);
        }
    }
    add_menu_separator(menu);
    add_menu_item(menu, kMenuPet, L"显示桌面宠物");
    add_menu_item(menu, kMenuSound, L"播放语音提醒");
    add_menu_item(menu, kMenuStartup, L"开机自动运行");
    add_menu_item(menu, kMenuFolder, L"打开 Codex 会话目录");
    add_menu_item(menu, kMenuSettings, L"设置…");
    add_menu_item(menu, kMenuUpdate, L"查看更新…");
    add_menu_separator(menu);
    add_menu_item(menu, kMenuVersion,
                  std::wstring(L"版本：v") + to_wide(CODEXPETS_VERSION), false);
    add_menu_item(menu, kMenuExit, L"退出");
    update_menu_checks(menu);
    return menu;
}

void NativeApp::update_menu_checks(HMENU menu) {
    CheckMenuItem(menu, kMenuPet, MF_BYCOMMAND | (settings_.pet_visible ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, kMenuSound, MF_BYCOMMAND | (settings_.sound_enabled ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, kMenuStartup, MF_BYCOMMAND | (is_autostart_enabled() ? MF_CHECKED : MF_UNCHECKED));
}

LRESULT NativeApp::settings_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE: {
            auto* header = CreateWindowExW(0, L"STATIC", L"桌面宠物与 Codex 会话监听",
                                           WS_CHILD | WS_VISIBLE, 20, 18, 500, 28, hwnd,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsHeader)), instance_, nullptr);
            set_control_font(header);
            const std::array<std::pair<int, const wchar_t*>, 4> labels{{
                {kSettingsHover, L"边缘触发高度（像素）"}, {kSettingsIdle, L"吸附自动隐藏（秒，0=关闭）"},
                {kSettingsReveal, L"鼠标唤出保持（秒）"}, {kSettingsNotification, L"任务状态云朵保持（秒）"}}};
            for (int i = 0; i < 4; ++i) {
                auto* label = CreateWindowExW(0, L"STATIC", labels[static_cast<std::size_t>(i)].second,
                    WS_CHILD | WS_VISIBLE, 24, 66 + i * 38, 230, 24, hwnd, nullptr, instance_, nullptr);
                set_control_font(label);
                auto* edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL, 270, 63 + i * 38, 100, 26, hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(labels[static_cast<std::size_t>(i)].first)), instance_, nullptr);
                set_control_font(edit);
            }
            set_edit_int(hwnd, kSettingsHover, settings_.dock_hover_height);
            set_edit_int(hwnd, kSettingsIdle, settings_.dock_idle_hide_seconds);
            set_edit_int(hwnd, kSettingsReveal, settings_.dock_reveal_seconds);
            set_edit_int(hwnd, kSettingsNotification, settings_.dock_notification_seconds);
            auto* root_label = CreateWindowExW(0, L"STATIC", L"Codex sessions 目录",
                WS_CHILD | WS_VISIBLE, 24, 224, 230, 24, hwnd, nullptr, instance_, nullptr);
            set_control_font(root_label);
            auto* root = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", settings_.sessions_root.c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 24, 250, 430, 26, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsRoot)), instance_, nullptr);
            set_control_font(root);
            auto* browse = CreateWindowExW(0, L"BUTTON", L"选择…", WS_CHILD | WS_VISIBLE,
                462, 249, 70, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsBrowse)), instance_, nullptr);
            set_control_font(browse);
            auto* sound = CreateWindowExW(0, L"BUTTON", L"播放语音提醒", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                24, 292, 180, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsSound)), instance_, nullptr);
            set_control_font(sound);
            SendMessageW(sound, BM_SETCHECK, settings_.sound_enabled ? BST_CHECKED : BST_UNCHECKED, 0);

            auto* xiaoai = CreateWindowExW(0, L"BUTTON", L"启用小爱音箱主动播报",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 24, 330, 230, 24, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsXiaoAiEnabled)), instance_, nullptr);
            set_control_font(xiaoai);
            SendMessageW(xiaoai, BM_SETCHECK, settings_.xiaoai.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
            auto* parallel_label = CreateWindowExW(0, L"STATIC", L"并发播报数（1-8）",
                WS_CHILD | WS_VISIBLE, 270, 332, 155, 24, hwnd, nullptr, instance_, nullptr);
            set_control_font(parallel_label);
            auto* parallel = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL, 440, 327, 80, 26, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsXiaoAiParallel)), instance_, nullptr);
            set_control_font(parallel);
            set_edit_int(hwnd, kSettingsXiaoAiParallel, settings_.xiaoai.max_parallel_requests);
            auto* target_label = CreateWindowExW(0, L"STATIC", L"目标音箱（可多选）",
                WS_CHILD | WS_VISIBLE, 24, 362, 160, 24, hwnd, nullptr, instance_, nullptr);
            set_control_font(target_label);
            auto* target = CreateWindowExW(0, L"LISTBOX", L"",
                WS_CHILD | LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT,
                0, 0, 1, 1, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsXiaoAiDevice)), instance_, nullptr);
            set_control_font(target);
            populate_xiaoai_device_selector(hwnd);
            auto* select = CreateWindowExW(WS_EX_CLIENTEDGE, L"BUTTON", L"请选择音箱  ▼",
                WS_CHILD | WS_VISIBLE, 160, 359, 365, 28, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsXiaoAiSelectAll)), instance_, nullptr);
            set_control_font(select);
            auto* scan = CreateWindowExW(0, L"BUTTON", L"扫描音箱",
                WS_CHILD | WS_VISIBLE, 240, 402, 100, 28, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsXiaoAiScan)), instance_, nullptr);
            set_control_font(scan);
            auto* login = CreateWindowExW(0, L"BUTTON", L"浏览器登录",
                WS_CHILD | WS_VISIBLE, 345, 402, 105, 28, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsXiaoAiLogin)), instance_, nullptr);
            set_control_font(login);
            auto* test = CreateWindowExW(0, L"BUTTON", L"测试播报",
                WS_CHILD | WS_VISIBLE, 455, 402, 90, 28, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsXiaoAiTest)), instance_, nullptr);
            set_control_font(test);
            auto* xiaoai_events = CreateWindowExW(0, L"STATIC",
                L"播报事件：开始、完成、错误、中断（保存后生效）",
                WS_CHILD | WS_VISIBLE, 24, 442, 430, 24, hwnd, nullptr, instance_, nullptr);
            set_control_font(xiaoai_events);
            auto* hint = CreateWindowExW(0, L"STATIC",
                L"扫描后可多选目标音箱，点击“全选”可对全部在线设备播报；授权信息仅保存在 Windows 凭据管理器中。",
                WS_CHILD | WS_VISIBLE, 24, 472, 510, 34, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsHint)), instance_, nullptr);
            set_control_font(hint);
            auto* defaults = CreateWindowExW(0, L"BUTTON", L"恢复默认", WS_CHILD | WS_VISIBLE,
                24, 535, 90, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsDefaults)), instance_, nullptr);
            auto* cancel = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE,
                370, 535, 75, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsCancel)), instance_, nullptr);
            auto* apply = CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                455, 535, 75, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsApply)), instance_, nullptr);
            set_control_font(defaults); set_control_font(cancel); set_control_font(apply);
            set_xiaoai_controls_enabled(!xiaoai_operation_in_flight_);
            return 0;
        }
        case WM_COMMAND: {
            const auto id = LOWORD(wparam);
            if (id == kSettingsDefaults) {
                AppSettings defaults;
                set_edit_int(hwnd, kSettingsHover, defaults.dock_hover_height);
                set_edit_int(hwnd, kSettingsIdle, defaults.dock_idle_hide_seconds);
                set_edit_int(hwnd, kSettingsReveal, defaults.dock_reveal_seconds);
                set_edit_int(hwnd, kSettingsNotification, defaults.dock_notification_seconds);
                SetDlgItemTextW(hwnd, kSettingsRoot, defaults.sessions_root.c_str());
                SendDlgItemMessageW(hwnd, kSettingsSound, BM_SETCHECK, BST_CHECKED, 0);
                SendDlgItemMessageW(hwnd, kSettingsXiaoAiEnabled, BM_SETCHECK, BST_UNCHECKED, 0);
                set_edit_int(hwnd, kSettingsXiaoAiParallel, defaults.xiaoai.max_parallel_requests);
                SendDlgItemMessageW(hwnd, kSettingsXiaoAiDevice, LB_SETSEL, FALSE, -1);
                update_xiaoai_device_summary(hwnd);
                return 0;
            }
            if (id == kSettingsBrowse) {
                BROWSEINFOW info{}; info.hwndOwner = hwnd; info.lpszTitle = L"选择 Codex sessions 目录";
                info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                const auto item = SHBrowseForFolderW(&info);
                if (item) {
                    wchar_t path[MAX_PATH * 8]{};
                    if (SHGetPathFromIDListW(item, path)) SetDlgItemTextW(hwnd, kSettingsRoot, path);
                    CoTaskMemFree(item);
                }
                return 0;
            }
            if (id == kSettingsXiaoAiSelectAll) { show_xiaoai_device_menu(hwnd); return 0; }
            if (id == kSettingsXiaoAiScan) { scan_xiaoai_devices(); return 0; }
            if (id == kSettingsXiaoAiLogin) { open_xiaomi_login(); return 0; }
            if (id == kSettingsXiaoAiTest) { test_xiaoai(); return 0; }
            if (id == kSettingsCancel) { DestroyWindow(hwnd); return 0; }
            if (id == kSettingsApply) {
                AppSettings next = settings_;
                next.dock_hover_height = read_edit_int(hwnd, kSettingsHover, next.dock_hover_height);
                next.dock_idle_hide_seconds = read_edit_int(hwnd, kSettingsIdle, next.dock_idle_hide_seconds);
                next.dock_reveal_seconds = read_edit_int(hwnd, kSettingsReveal, next.dock_reveal_seconds);
                next.dock_notification_seconds = read_edit_int(hwnd, kSettingsNotification, next.dock_notification_seconds);
                wchar_t root[32768]{}; GetDlgItemTextW(hwnd, kSettingsRoot, root, ARRAYSIZE(root));
                next.sessions_root = std::filesystem::path(root);
                next.sound_enabled = SendDlgItemMessageW(hwnd, kSettingsSound, BM_GETCHECK, 0, 0) == BST_CHECKED;
                next.xiaoai.enabled = SendDlgItemMessageW(hwnd, kSettingsXiaoAiEnabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
                next.xiaoai.max_parallel_requests = read_edit_int(
                    hwnd, kSettingsXiaoAiParallel, next.xiaoai.max_parallel_requests);
                next.xiaoai.device_ids = selected_xiaoai_device_ids(hwnd);
                next.xiaoai.device_id = next.xiaoai.device_ids.empty()
                    ? std::string{} : next.xiaoai.device_ids.front();
                next.normalize();
                const auto root_changed = next.sessions_root != settings_.sessions_root;
                settings_ = next;
                if (xiaoai_notifier_) xiaoai_notifier_->configure(settings_.xiaoai);
                save_settings();
                if (root_changed) {
                    if (monitor_worker_) monitor_worker_->stop();
                    pending_updates_.clear();
                    monitor_message_posted_.store(false, std::memory_order_release);
                    snapshot_ = MonitorSnapshot{};
                    has_snapshot_ = false;
                    visual_coordinator_ = VisualStateCoordinator{};
                    displayed_task_titles_.clear();
                    selected_task_index_ = 0;
                    dock_thought_until_ = Clock::time_point::min();
                    start_monitor();
                }
                DestroyWindow(hwnd);
                refresh_visual(true);
                return 0;
            }
            break;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: settings_window_ = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void NativeApp::exit_application() {
    shutting_down_ = true;
    PostQuitMessage(0);
}

LRESULT CALLBACK NativeApp::pet_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    NativeApp* app = reinterpret_cast<NativeApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = static_cast<NativeApp*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->pet_proc(hwnd, message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK NativeApp::message_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    NativeApp* app = reinterpret_cast<NativeApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = static_cast<NativeApp*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->message_proc(hwnd, message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK NativeApp::settings_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    NativeApp* app = reinterpret_cast<NativeApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = static_cast<NativeApp*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->settings_proc(hwnd, message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT NativeApp::pet_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &point);
            return renderer_.hit_test_alpha(point.x, point.y) ? HTCLIENT : HTTRANSPARENT;
        }
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONDOWN: {
            POINT cursor = cursor_position();
            POINT local = cursor; ScreenToClient(hwnd, &local);
            if (!renderer_.hit_test_alpha(local.x, local.y)) return 0;
            const auto dpi = GetDpiForWindow(hwnd);
            const auto scale = dpi == 0 ? 1.0 : static_cast<double>(dpi) / 96.0;
            const PointD logical{local.x / scale, local.y / scale};
            RECT window_rect{};
            GetWindowRect(hwnd, &window_rect);
            const bool docked = dock_edge_ != DockEdge::None;
            const auto screen = docked
                ? dock_screen()
                : screen_from_point(POINT{
                    window_rect.left + (window_rect.right - window_rect.left) / 2,
                    window_rect.top + (window_rect.bottom - window_rect.top) / 2});
            const bool bubble_below = docked && dock_coordinate_ < screen.work.top +
                static_cast<int>(std::lround(render_layout::dock_bubble_switch_margin * scale));
            const render_layout::State layout{last_visual_state_, dock_edge_, docked, bubble_below,
                                              false, dock_visibility_, animation_tick_};
            const RectD bubble = render_layout::bubble_bounds(layout);
            const RectD content = render_layout::body_bounds(layout);
            const bool bubble_visible = app_logic::should_show_thought_bubble(
                dock_edge_ != DockEdge::None, last_visual_state_, Clock::now(), dock_thought_until_);
            if (app_logic::is_task_switch_point(dock_edge_ != DockEdge::None, bubble_visible,
                    last_visual_state_, static_cast<int>(displayed_task_titles_.size()),
                    bubble, content, logical)) {
                if (displayed_task_titles_.size() > 1) {
                    selected_task_index_ = (selected_task_index_ + 1) %
                        static_cast<int>(displayed_task_titles_.size());
                    scroll_offset_ = 0; scroll_hold_seconds_ = 1.9; scroll_at_end_ = false;
                    reveal_dock_for_interaction(); refresh_visual(true);
                }
                return 0;
            }
            begin_drag(cursor);
            return 0;
        }
        case WM_MOUSEMOVE:
            move_drag(cursor_position());
            return 0;
        case WM_LBUTTONUP:
            finish_drag(cursor_position());
            return 0;
        case WM_RBUTTONUP: {
            const auto menu = build_menu(true);
            const auto point = cursor_position();
            SetForegroundWindow(hwnd);
            const auto command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD,
                                                point.x, point.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (command) SendMessageW(message_window_, WM_COMMAND, command, 0);
            return 0;
        }
        case WM_TIMER:
            if (wparam == timer_id_) { on_timer(); return 0; }
            break;
        case WM_CLOSE:
            exit_application(); return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT NativeApp::message_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kXiaoAiResultMessage) {
        std::unique_ptr<XiaoAiUiResult> result(reinterpret_cast<XiaoAiUiResult*>(lparam));
        if (!result) return 0;
        const auto owner = result->owner && IsWindow(result->owner) ? result->owner : pet_window_;
        const auto complete = [this] {
            xiaoai_operation_in_flight_ = false;
            set_xiaoai_controls_enabled(true);
        };
        switch (result->operation) {
            case XiaoAiUiOperation::ValidateLogin: {
                if (!result->error.empty()) {
                    complete();
                    MessageBoxW(owner, to_wide(result->error).c_str(), L"小米账号登录", MB_ICONERROR | MB_OK);
                    return 0;
                }
                auto authorization = compact_xiaoai_authorization(result->settings.auth_cookies);
                std::string save_error;
                if (!save_xiaoai_authorization(authorization, &save_error)) {
                    complete();
                    MessageBoxW(owner, to_wide(save_error).c_str(), L"小米账号登录", MB_ICONERROR | MB_OK);
                    return 0;
                }
                settings_.xiaoai.enabled = true;
                settings_.xiaoai.auth_cookies = std::move(authorization);
                if (settings_window_ && IsWindow(settings_window_)) {
                    SendDlgItemMessageW(settings_window_, kSettingsXiaoAiEnabled, BM_SETCHECK, BST_CHECKED, 0);
                }
                if (!xiaoai_notifier_) {
                    complete();
                    MessageBoxW(owner, L"小爱播报服务未初始化。", L"小米账号登录", MB_ICONERROR | MB_OK);
                    return 0;
                }
                xiaoai_notifier_->configure(settings_.xiaoai);
                save_settings();
                xiaoai_notifier_->discover_devices_async(settings_.xiaoai, [message_window = hwnd, owner](
                                                              std::vector<XiaoAiDeviceInfo> devices, std::string error) {
                    post_xiaoai_ui_result(message_window, {XiaoAiUiOperation::ScanDevices, owner, {}, std::move(devices),
                        std::move(error), true});
                });
                return 0;
            }
            case XiaoAiUiOperation::ScanDevices: {
                complete();
                if (!result->error.empty()) {
                    MessageBoxW(owner, to_wide(result->error).c_str(), L"小爱音箱", MB_ICONERROR | MB_OK);
                    return 0;
                }
                xiaoai_devices_ = std::move(result->devices);
                if (settings_window_ && IsWindow(settings_window_)) populate_xiaoai_device_selector(settings_window_);
                const std::wstring text = result->login_follow_up
                    ? std::wstring(L"小米账号授权已验证。已扫描音箱，请勾选目标音箱后测试播报。")
                    : L"已扫描到 " + std::to_wstring(xiaoai_devices_.size()) + L" 台在线小爱音箱，可多选或点击全选。";
                MessageBoxW(owner, text.c_str(), L"小爱音箱", MB_OK);
                return 0;
            }
            case XiaoAiUiOperation::TestNotification:
                complete();
                if (!result->error.empty()) {
                    MessageBoxW(owner, to_wide(result->error).c_str(), L"小爱音箱", MB_ICONERROR | MB_OK);
                } else {
                    MessageBoxW(owner, L"测试播报已发送。", L"小爱音箱", MB_OK);
                }
                return 0;
        }
    }
    if (message == kMonitorMessage) { process_monitor_updates(); return 0; }
    if (message == kTrayCallback) {
        if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) {
            const auto menu = build_menu(false);
            const auto point = cursor_position();
            SetForegroundWindow(hwnd);
            const auto command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD,
                                                point.x, point.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (command) SendMessageW(message_window_, WM_COMMAND, command, 0);
        } else if (lparam == WM_LBUTTONDBLCLK) show_pet(!settings_.pet_visible);
        return 0;
    }
    if (message == WM_COMMAND) {
        switch (LOWORD(wparam)) {
            case kMenuPet: show_pet(!settings_.pet_visible); break;
            case kMenuSound: toggle_sound(); break;
            case kMenuStartup: toggle_startup(); break;
            case kMenuFolder: open_sessions_folder(); break;
            case kMenuSettings: show_settings(); break;
            case kMenuUpdate: open_latest_release(); break;
            case kMenuExit: exit_application(); break;
            default: break;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int NativeApp::run_utility(HINSTANCE instance, const std::vector<std::wstring>& arguments) {
    auto has = [&](std::wstring_view value) {
        return std::find(arguments.begin(), arguments.end(), value) != arguments.end();
    };
    if (has(L"--version")) {
        write_stdout(std::string(CODEXPETS_VERSION) + "\n");
        return 0;
    }
    if (has(L"--startup-smoke-test")) {
        NativeApp app(instance, arguments);
        std::string startup_error;
        if (!app.initialize(&startup_error)) {
            write_stdout("startup-smoke-test: " + startup_error + "\n");
            return 1;
        }
        app.shutdown();
        write_stdout("startup-smoke-test: ok\n");
        return 0;
    }
    Renderer renderer;
    std::string error;
    if (!renderer.initialize(instance, &error)) { write_stdout(error + "\n"); return 1; }
    auto finish = [&](int code) { renderer.shutdown(); return code; };
    if (has(L"--validate-resources")) {
        const auto ok = renderer.validate(&error);
        if (ok) write_stdout("resources: ok\n"); else write_stdout("resources: " + error + "\n");
        return finish(ok ? 0 : 1);
    }
    if (has(L"--smoke-test") || has(L"--preview")) {
        RenderState state;
        MonitorSnapshot preview_snapshot;
        preview_snapshot.active_count = 1;
        preview_snapshot.active_titles = {"原生渲染检查"};
        preview_snapshot.active_plan_progress_labels = {std::optional<std::string>("1/3")};
        preview_snapshot.completed_plan_step_count = 1;
        preview_snapshot.total_plan_step_count = 3;
        preview_snapshot.last_completed_title = "原生渲染检查：任务已完成";
        preview_snapshot.last_aborted_title = "原生渲染检查：模拟异常";
        preview_snapshot.last_interrupted_title = "原生渲染检查：任务已中断";
        const auto configure_preview_state = [&](ReminderState visual) {
            MonitorSnapshot snapshot = preview_snapshot;
            if (visual == ReminderState::Idle) {
                snapshot.active_count = 0;
                snapshot.active_titles.clear();
                snapshot.active_plan_progress_labels.clear();
                snapshot.completed_plan_step_count = 0;
                snapshot.total_plan_step_count = 0;
            }
            const auto content = make_visual_content(visual, snapshot);
            state.state = visual;
            state.status_text = content.status_text;
            state.thought_text = content.thought_text;
            state.task_titles = content.task_titles;
            state.progress_labels = content.progress_labels;
            state.selected_task_index = 0;
            state.scroll_offset = 0;
            state.animation_tick = (visual == ReminderState::Busy || visual == ReminderState::Completed) ? 18 : 0;
        };
        const std::array<std::pair<ReminderState, const char*>, 5> states{{
            {ReminderState::Idle, "idle"}, {ReminderState::Busy, "busy"},
            {ReminderState::Completed, "completed"}, {ReminderState::Error, "error"},
            {ReminderState::Interrupted, "interrupted"}}};
        std::filesystem::path preview_folder;
        const auto preview_it = std::find(arguments.begin(), arguments.end(), L"--preview");
        if (preview_it != arguments.end() && std::next(preview_it) != arguments.end()) {
            preview_folder = std::filesystem::path(*std::next(preview_it));
            std::filesystem::create_directories(preview_folder);
        }
        for (const auto& [visual, name] : states) {
            configure_preview_state(visual);
            if (preview_folder.empty()) {
                if (!renderer.render(state, 1.0, &error)) return finish(1);
            } else if (!renderer.save_preview(preview_folder / (std::string(name) + ".png"), state, 1.0, &error)) {
                write_stdout(error + "\n"); return finish(1);
            }
        }
        state.docked = true;
        for (const auto& [visual, name] : states) {
            configure_preview_state(visual);
            for (const auto edge : {DockEdge::Left, DockEdge::Right}) {
                state.dock_edge = edge;
                const auto side = edge == DockEdge::Left ? "left" : "right";
                if (preview_folder.empty()) {
                    if (!renderer.render(state, 1.0, &error)) return finish(1);
                } else if (!renderer.save_preview(
                               preview_folder / (std::string("dock-") + side + "-" + name + ".png"),
                               state, 1.0, &error)) {
                    return finish(1);
                }
            }
        }
        write_stdout(preview_folder.empty() ? "smoke-test: ok\n" : "preview: ok\n");
        return finish(0);
    }
    if (has(L"--test-sound")) {
        const auto folder = std::filesystem::temp_directory_path() / L"CodeXPetsNativeAudio";
        for (const auto sound : {NotificationSound::Started, NotificationSound::Completed,
                                 NotificationSound::Error, NotificationSound::Interrupted}) {
            const auto name = sound == NotificationSound::Started ? L"start.mp3" :
                              sound == NotificationSound::Completed ? L"complete.mp3" :
                              sound == NotificationSound::Error ? L"error.mp3" : L"interrupted.mp3";
            const auto path = folder / name;
            if (!renderer.extract_audio(sound, path, &error)) return finish(1);
            const auto command = std::wstring(L"open \"") + path.native() + L"\" type mpegvideo alias codexpets_test";
            if (mciSendStringW(command.c_str(), nullptr, 0, nullptr) != 0) return finish(1);
            mciSendStringW(L"play codexpets_test wait", nullptr, 0, nullptr);
            mciSendStringW(L"close codexpets_test", nullptr, 0, nullptr);
        }
        write_stdout("sound: ok\n");
        return finish(0);
    }
    return finish(0);
}

} // namespace codexpets::windows

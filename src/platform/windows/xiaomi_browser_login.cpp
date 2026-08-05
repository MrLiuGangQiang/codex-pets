#include "xiaomi_browser_login.h"

#include "resource_ids.h"
#include "../../../src/core/platform_text.h"

#include <shlobj.h>
#include <wrl.h>
#ifndef interface
#define CODEXPETS_DEFINED_INTERFACE_MACRO
#define interface struct
#endif
#include <WebView2.h>
#ifdef CODEXPETS_DEFINED_INTERFACE_MACRO
#undef interface
#undef CODEXPETS_DEFINED_INTERFACE_MACRO
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <system_error>
#include <thread>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace codexpets::windows {
namespace {
using Microsoft::WRL::ComPtr;

constexpr wchar_t kLoginClass[] = L"CodeXPets.XiaomiBrowserLogin";
constexpr wchar_t kLoginUrl[] = L"https://account.xiaomi.com/pass/serviceLogin?sid=micoapi&_locale=zh_CN";
std::shared_ptr<class XiaomiBrowserLogin> g_login;

std::wstring make_user_data_folder() {
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) return {};
    wchar_t text[64]{};
    if (StringFromGUID2(id, text, static_cast<int>(std::size(text))) <= 0) return {};

    std::error_code error;
    const auto temporary_root = std::filesystem::temp_directory_path(error);
    if (error || temporary_root.empty()) return {};
    try {
        return (temporary_root / (std::wstring(L"CodeXPets-XiaomiLogin-") + text)).wstring();
    } catch (...) {
        return {};
    }
}

void remove_user_data_folder(std::wstring folder) {
    if (folder.empty()) return;
    try {
        std::thread([folder = std::move(folder)] {
            try {
                std::error_code error;
                const auto root = std::filesystem::temp_directory_path(error).lexically_normal();
                if (error || root.empty()) return;

                const auto target = std::filesystem::path(folder).lexically_normal();
                const auto name = target.filename().wstring();
                constexpr std::wstring_view prefix = L"CodeXPets-XiaomiLogin-";
                if (target.parent_path() != root || name.size() <= prefix.size() ||
                    name.compare(0, prefix.size(), prefix) != 0) {
                    return;
                }

                for (int attempt = 0; attempt < 20; ++attempt) {
                    error.clear();
                    std::filesystem::remove_all(target, error);
                    error.clear();
                    if (!std::filesystem::exists(target, error) && !error) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            } catch (...) {
                // Cleanup is best effort; never let a background cleanup failure terminate the app.
            }
        }).detach();
    } catch (...) {
        // A transient thread-creation failure must not break login completion.
    }
}

using CreateEnvironment = HRESULT(STDAPICALLTYPE *)(
    PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

std::wstring loader_architecture() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return L"arm64";
#else
    return L"x64";
#endif
}

HMODULE load_embedded_loader() {
    static HMODULE loader = [] {
        const auto instance = GetModuleHandleW(nullptr);
        const auto resource = FindResourceW(instance, MAKEINTRESOURCEW(IDR_WEBVIEW2_LOADER), RT_RCDATA);
        if (!resource) return static_cast<HMODULE>(nullptr);
        const auto size = SizeofResource(instance, resource);
        const auto loaded = LoadResource(instance, resource);
        const auto data = loaded ? LockResource(loaded) : nullptr;
        if (!data || size == 0) return static_cast<HMODULE>(nullptr);

        PWSTR local_app_data = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr,
                                         &local_app_data)) || !local_app_data) {
            return static_cast<HMODULE>(nullptr);
        }
        std::filesystem::path target;
        try {
            const auto version = codexpets::utf8_to_wide(CODEXPETS_VERSION);
            target = std::filesystem::path(local_app_data) / L"CodeXPets" / L"runtime" /
                (L"WebView2Loader-" + version + L"-" + loader_architecture() + L".dll");
            CoTaskMemFree(local_app_data);
            local_app_data = nullptr;
            std::filesystem::create_directories(target.parent_path());
            std::error_code ec;
            const auto existing_size = std::filesystem::file_size(target, ec);
            if (ec || existing_size != size) {
                const auto temporary = target.wstring() + L".tmp";
                HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                          CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
                if (file == INVALID_HANDLE_VALUE) return static_cast<HMODULE>(nullptr);
                DWORD written{};
                const bool written_ok = WriteFile(file, data, size, &written, nullptr) &&
                    written == size && FlushFileBuffers(file);
                CloseHandle(file);
                if (!written_ok) {
                    DeleteFileW(temporary.c_str());
                    return static_cast<HMODULE>(nullptr);
                }
                if (!MoveFileExW(temporary.c_str(), target.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    DeleteFileW(temporary.c_str());
                    return static_cast<HMODULE>(nullptr);
                }
            }
        } catch (...) {
            if (local_app_data) CoTaskMemFree(local_app_data);
            return static_cast<HMODULE>(nullptr);
        }
        return LoadLibraryExW(target.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                                        LOAD_LIBRARY_SEARCH_SYSTEM32);
    }();
    return loader;
}

HRESULT create_environment(
    PCWSTR user_data_folder,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* completed) {
    const auto loader = load_embedded_loader();
    if (!loader) return HRESULT_FROM_WIN32(GetLastError());

    const auto create = reinterpret_cast<CreateEnvironment>(
        GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions"));
    if (!create) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    return create(nullptr, user_data_folder, nullptr, completed);
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const auto needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), needed, nullptr, nullptr);
    return result;
}

class CallbackRefCount {
public:
    ULONG AddRef() { return ++references_; }
    ULONG Release() { const auto result = --references_; if (!result) delete this; return result; }
protected:
    HRESULT query(REFIID wanted, REFIID own, void* self, void** object) {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (wanted != IID_IUnknown && wanted != own) return E_NOINTERFACE;
        *object = self;
        AddRef();
        return S_OK;
    }
protected:
    virtual ~CallbackRefCount() = default;

private:
    std::atomic<ULONG> references_{1};
};

class EnvironmentHandler final
    : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
      private CallbackRefCount {
public:
    explicit EnvironmentHandler(
        std::function<HRESULT(HRESULT, ICoreWebView2Environment*)> invoke)
        : invoke_(std::move(invoke)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        return query(iid, IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                     static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(this),
                     object);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return CallbackRefCount::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return CallbackRefCount::Release(); }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* environment) override {
        return invoke_(result, environment);
    }

private:
    std::function<HRESULT(HRESULT, ICoreWebView2Environment*)> invoke_;
};

class ControllerHandler final
    : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
      private CallbackRefCount {
public:
    explicit ControllerHandler(
        std::function<HRESULT(HRESULT, ICoreWebView2Controller*)> invoke)
        : invoke_(std::move(invoke)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        return query(iid, IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                     static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(this),
                     object);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return CallbackRefCount::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return CallbackRefCount::Release(); }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* controller) override {
        return invoke_(result, controller);
    }

private:
    std::function<HRESULT(HRESULT, ICoreWebView2Controller*)> invoke_;
};

class NavigationHandler final
    : public ICoreWebView2NavigationCompletedEventHandler,
      private CallbackRefCount {
public:
    explicit NavigationHandler(
        std::function<HRESULT(ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*)> invoke)
        : invoke_(std::move(invoke)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        return query(iid, IID_ICoreWebView2NavigationCompletedEventHandler,
                     static_cast<ICoreWebView2NavigationCompletedEventHandler*>(this), object);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return CallbackRefCount::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return CallbackRefCount::Release(); }
    HRESULT STDMETHODCALLTYPE Invoke(
        ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) override {
        return invoke_(sender, args);
    }

private:
    std::function<HRESULT(ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*)> invoke_;
};

class CookiesHandler final
    : public ICoreWebView2GetCookiesCompletedHandler,
      private CallbackRefCount {
public:
    explicit CookiesHandler(std::function<HRESULT(HRESULT, ICoreWebView2CookieList*)> invoke)
        : invoke_(std::move(invoke)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        return query(iid, IID_ICoreWebView2GetCookiesCompletedHandler,
                     static_cast<ICoreWebView2GetCookiesCompletedHandler*>(this), object);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return CallbackRefCount::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return CallbackRefCount::Release(); }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2CookieList* cookies) override {
        return invoke_(result, cookies);
    }

private:
    std::function<HRESULT(HRESULT, ICoreWebView2CookieList*)> invoke_;
};

class XiaomiBrowserLogin : public std::enable_shared_from_this<XiaomiBrowserLogin> {
public:
    XiaomiBrowserLogin(HWND owner, std::function<void(std::string, std::string)> done)
        : owner_(owner), folder_(make_user_data_folder()), done_(std::move(done)) {}

    void show() {
        if (folder_.empty()) { finish({}, "无法创建临时小米登录目录"); return; }
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = window_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kLoginClass;
        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(0, kLoginClass, L"登录小米账号", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 860, 700, owner_, nullptr, wc.hInstance, this);
        if (!hwnd_) finish({}, "无法创建小米登录窗口");
    }

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<XiaomiBrowserLogin*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<XiaomiBrowserLogin*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            // WM_CREATE is sent before CreateWindowExW returns, so initialize this before WebView2 starts.
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
        switch (message) {
        case WM_CREATE: self->create_webview(); return 0;
        case WM_SIZE: self->resize(); return 0;
        case WM_TIMER:
            if (wparam == kCookiePollTimer) self->collect_cookies();
            return 0;
        case kFinishMessage:
            self->finalize_finish();
            return 0;
        case WM_CLOSE: self->finish({}, "已取消小米网页登录"); return 0;
        case WM_NCDESTROY:
            KillTimer(hwnd, kCookiePollTimer);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->hwnd_ = nullptr;
            return DefWindowProcW(hwnd, message, wparam, lparam);
        default: return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }

    void create_webview() {
        const auto self = shared_from_this();
        environment_handler_.Attach(new EnvironmentHandler([self](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            if (FAILED(result) || !environment) { self->finish({}, "无法初始化 Edge WebView2 运行时"); return S_OK; }

            self->controller_handler_.Attach(new ControllerHandler([self](HRESULT controller_result, ICoreWebView2Controller* controller) -> HRESULT {
                if (FAILED(controller_result) || !controller) { self->finish({}, "无法创建小米登录浏览器，错误码：" + std::to_string(static_cast<unsigned long>(controller_result))); return S_OK; }

                self->controller_ = controller;
                self->controller_->get_CoreWebView2(&self->webview_);
                if (FAILED(self->webview_->QueryInterface(IID_ICoreWebView2_2, reinterpret_cast<void**>(self->webview2_.GetAddressOf())))) { self->finish({}, "当前 Edge WebView2 版本不支持读取授权信息"); return S_OK; }
                self->resize();
                self->navigation_handler_.Attach(new NavigationHandler([self](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                    self->handle_navigation_completed();
                    return S_OK;
                }));
                self->webview_->add_NavigationCompleted(self->navigation_handler_.Get(), &self->navigation_token_);
                SetTimer(self->hwnd_, kCookiePollTimer, kCookiePollIntervalMs, nullptr);
                self->clear_auth_cookies_and_navigate();
                return S_OK;
            }));
            const auto controller_hr = environment->CreateCoreWebView2Controller(self->hwnd_, self->controller_handler_.Get());
            if (FAILED(controller_hr)) self->finish({}, "无法请求创建小米登录浏览器，错误码：" + std::to_string(static_cast<unsigned long>(controller_hr)));
            return S_OK;
        }));
        const auto hr = create_environment(folder_.c_str(), environment_handler_.Get());
        if (FAILED(hr)) finish({}, "无法启动 Edge WebView2，错误码：" + std::to_string(static_cast<unsigned long>(hr)));
    }
    void resize() const { if (controller_ && hwnd_) { RECT bounds{}; GetClientRect(hwnd_, &bounds); controller_->put_Bounds(bounds); } }

    void handle_navigation_completed() {
        // The browser is used only to establish the Xiaomi passport session.  The
        // MiNA token exchange is performed by the native HTTP client, which avoids
        // the SNS callback being rendered as an Edge HTTP 401 page.
        collect_cookies();
    }

    void clear_auth_cookies_and_navigate() {
        ComPtr<ICoreWebView2CookieManager> manager;
        if (FAILED(webview2_->get_CookieManager(&manager)) || !manager) {
            finish({}, "无法重置小米登录状态");
            return;
        }
        // Start with a clean dedicated profile so a rejected/expired passport session cannot
        // immediately redirect back to the MiNA 401 page.
        if (FAILED(manager->DeleteAllCookies())) {
            finish({}, "无法重置小米登录状态");
            return;
        }
        has_passport_session_ = false;
        has_mina_service_token_ = false;
        webview_->Navigate(kLoginUrl);
    }

    void collect_cookies() {
        if (collecting_ || finished_ || !webview2_) return;
        collecting_ = true;
        cookie_jar_.clear();
        collect_for(L"https://account.xiaomi.com/", true);
    }

    void collect_for(const wchar_t* uri, bool then_mina) {
        ComPtr<ICoreWebView2CookieManager> manager;
        if (FAILED(webview2_->get_CookieManager(&manager)) || !manager) {
            collecting_ = false;
            return;
        }
        const auto self = shared_from_this();
        // WebView2 retains this callback until GetCookies completes. Keeping every
        // polling callback in the window object caused unbounded memory growth.
        ComPtr<ICoreWebView2GetCookiesCompletedHandler> completed;
        completed.Attach(new CookiesHandler(
            [self, then_mina](HRESULT result, ICoreWebView2CookieList* cookies) -> HRESULT {
                if (SUCCEEDED(result) && cookies) self->append_cookies(cookies, !then_mina);
                if (then_mina) {
                    // A WeChat/SNS login finishes on account.xiaomi.com with a passport
                    // session. Do not navigate the embedded browser to the MiNA callback:
                    // that callback rejects the browser flow with a visible 401.
                    if (self->has_passport_session_ &&
                        (self->has_cookie("userId") || self->has_cookie("cUserId"))) {
                        self->collecting_ = false;
                        self->finish(self->serialized_cookies(), {});
                    } else {
                        self->collect_for(L"https://api2.mina.mi.com/", false);
                    }
                } else {
                    self->collecting_ = false;
                    if (self->has_mina_service_token_) self->finish(self->serialized_cookies(), {});
                }
                return S_OK;
            }));
        if (FAILED(manager->GetCookies(uri, completed.Get()))) collecting_ = false;
    }

    static bool is_authorization_cookie(std::string_view name) {
        return name == "serviceToken" || name == "userId" || name == "cUserId" ||
               name == "deviceId" || name == "passToken";
    }

    bool has_cookie(std::string_view name) const {
        return std::any_of(cookie_jar_.begin(), cookie_jar_.end(), [name](const auto& cookie) {
            return cookie.first == name && !cookie.second.empty();
        });
    }

    std::string serialized_cookies() const {
        std::string result;
        for (const auto& [name, value] : cookie_jar_) {
            if (value.empty()) continue;
            if (!result.empty()) result += "; ";
            result += name;
            result += "=";
            result += value;
        }
        return result;
    }

    void append_cookies(ICoreWebView2CookieList* cookies, bool from_mina) {
        UINT32 count{};
        if (FAILED(cookies->get_Count(&count))) return;
        for (UINT32 index = 0; index < count; ++index) {
            ComPtr<ICoreWebView2Cookie> cookie;
            if (FAILED(cookies->GetValueAtIndex(index, &cookie)) || !cookie) continue;
            LPWSTR name = nullptr;
            LPWSTR value = nullptr;
            if (SUCCEEDED(cookie->get_Name(&name)) && SUCCEEDED(cookie->get_Value(&value)) && name && value) {
                const auto cookie_name = utf8(name);
                const auto cookie_value = utf8(value);
                if (cookie_name == "serviceToken" && !cookie_value.empty() && from_mina) {
                    has_mina_service_token_ = true;
                }
                if (!from_mina && cookie_name == "passToken" && !cookie_value.empty()) {
                    has_passport_session_ = true;
                }
                if (is_authorization_cookie(cookie_name) &&
                    (cookie_name != "serviceToken" || from_mina)) {
                    const auto existing = std::find_if(cookie_jar_.begin(), cookie_jar_.end(),
                        [&cookie_name](const auto& item) { return item.first == cookie_name; });
                    if (existing == cookie_jar_.end()) cookie_jar_.emplace_back(cookie_name, cookie_value);
                    else existing->second = cookie_value;
                }
            }
            if (name) CoTaskMemFree(name);
            if (value) CoTaskMemFree(value);
        }
    }

    void finish(std::string cookies, std::string error) {
        if (finished_) return;
        finished_ = true;
        pending_cookies_ = std::move(cookies);
        pending_error_ = std::move(error);

        // WebView2 may invoke this method from inside a browser callback.  Do not
        // close/reset the controller or invoke the application callback reentrantly;
        // EmbeddedBrowserWebView can still be using the callback stack at this point.
        if (hwnd_) {
            if (!finish_posted_) {
                finish_posted_ = true;
                PostMessageW(hwnd_, kFinishMessage, 0, 0);
            }
            return;
        }
        finalize_finish();
    }

    void finalize_finish() {
        const auto keep_alive = shared_from_this();
        if (hwnd_) KillTimer(hwnd_, kCookiePollTimer);
        if (webview_ && navigation_handler_) {
            webview_->remove_NavigationCompleted(navigation_token_);
        }
        if (controller_) controller_->Close();
        webview2_.Reset();
        webview_.Reset();
        controller_.Reset();
        navigation_handler_.Reset();
        controller_handler_.Reset();
        environment_handler_.Reset();
        if (hwnd_) DestroyWindow(hwnd_);
        auto done = std::move(done_);
        auto folder = std::move(folder_);
        g_login.reset();
        remove_user_data_folder(std::move(folder));
        if (done) done(std::move(pending_cookies_), std::move(pending_error_));
        (void)keep_alive;
    }

    static constexpr UINT_PTR kCookiePollTimer = 1;
    static constexpr UINT kCookiePollIntervalMs = 750;
static constexpr UINT kFinishMessage = WM_APP + 0x241;

    HWND owner_{};
    HWND hwnd_{};
    std::wstring folder_;
    std::function<void(std::string, std::string)> done_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
    ComPtr<ICoreWebView2_2> webview2_;
    ComPtr<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> environment_handler_;
    ComPtr<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> controller_handler_;
    ComPtr<ICoreWebView2NavigationCompletedEventHandler> navigation_handler_;
    EventRegistrationToken navigation_token_{};
    std::vector<std::pair<std::string, std::string>> cookie_jar_;
    bool collecting_{};
    bool has_passport_session_{};
    bool has_mina_service_token_{};
    bool finished_{};
    bool finish_posted_{};
    std::string pending_cookies_;
    std::string pending_error_;
};
} // namespace

void start_xiaomi_browser_login(
    HWND owner, std::function<void(std::string cookies, std::string error)> completed) {
    if (g_login) {
        if (completed) completed({}, "小米登录窗口已经打开");
        return;
    }
    g_login = std::make_shared<XiaomiBrowserLogin>(owner, std::move(completed));
    g_login->show();
}
} // namespace codexpets::windows

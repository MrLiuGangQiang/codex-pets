#include "xiaomi_browser_login.h"

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include <functional>
#include <string>
#include <utility>

namespace codexpets::macos {

std::string xiaoai_cookie_header(NSArray<NSHTTPCookie*>* cookies, bool* has_mina_token,
                                 bool* has_user) {
    std::string result;
    std::string user_id;
    std::string service_token;
    std::string device_id;
    for (NSHTTPCookie* cookie in cookies) {
        const char* name = cookie.name.UTF8String;
        const char* value = cookie.value.UTF8String;
        const char* domain = cookie.domain.UTF8String;
        if (!name || !value) continue;
        const std::string cookie_name(name);
        const std::string cookie_value(value);
        const std::string cookie_domain = domain ? domain : "";
        if (cookie_name == "userId" || cookie_name == "cUserId") {
            if (user_id.empty()) user_id = cookie_value;
        } else if (cookie_name == "serviceToken" &&
                   cookie_domain.find("mina.mi.com") != std::string::npos) {
            service_token = cookie_value;
            if (has_mina_token) *has_mina_token = true;
        } else if (cookie_name == "deviceId") {
            device_id = cookie_value;
        }
    }
    if (has_user) *has_user = !user_id.empty();
    if (user_id.empty() || service_token.empty()) return {};
    result = "userId=" + user_id + "; serviceToken=" + service_token;
    if (!device_id.empty()) result += "; deviceId=" + device_id;
    return result;
}

} // namespace codexpets::macos

namespace {
constexpr char kLoginUrl[] =
    "https://account.xiaomi.com/pass/serviceLogin?sid=micoapi&_locale=zh_CN";
constexpr char kMinaUrl[] = "https://api2.mina.mi.com/";
}

@interface XiaomiBrowserLogin : NSObject <WKNavigationDelegate, NSWindowDelegate> {
    NSWindow* _window;
    WKWebView* _webView;
    NSTimer* _timer;
    std::function<void(std::string, std::string)> _completed;
    BOOL _finished;
    BOOL _requestedMina;
}
- (instancetype)initWithOwner:(NSWindow*)owner
                      completed:(std::function<void(std::string, std::string)>)completed;
- (void)show;
@end

static __strong XiaomiBrowserLogin* g_login = nil;

@implementation XiaomiBrowserLogin

- (instancetype)initWithOwner:(NSWindow*)owner
                      completed:(std::function<void(std::string, std::string)>)completed {
    self = [super init];
    if (self) {
        _completed = std::move(completed);
        _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 900, 680)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                      NSWindowStyleMaskResizable
            backing:NSBackingStoreBuffered defer:NO];
        _window.title = @"登录小米账号";
        _window.delegate = self;
        if (owner) [owner addChildWindow:_window ordered:NSWindowAbove];
    }
    return self;
}

- (void)show {
    WKWebViewConfiguration* configuration = [WKWebViewConfiguration new];
    configuration.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
    _webView = [[WKWebView alloc] initWithFrame:_window.contentView.bounds
                                  configuration:configuration];
    _webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _webView.navigationDelegate = self;
    _window.contentView = _webView;
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [_webView loadRequest:[NSURLRequest requestWithURL:
        [NSURL URLWithString:[NSString stringWithUTF8String:kLoginUrl]]]];
    _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                             selector:@selector(collectCookies)
                                             userInfo:nil repeats:YES];
}

- (void)collectCookies {
    if (_finished) return;
    WKHTTPCookieStore* store = _webView.configuration.websiteDataStore.httpCookieStore;
    __weak XiaomiBrowserLogin* weakSelf = self;
    [store getAllCookies:^(NSArray<NSHTTPCookie*>* cookies) {
        XiaomiBrowserLogin* strongSelf = weakSelf;
        if (!strongSelf || strongSelf->_finished) return;
        bool has_mina_token = false;
        bool has_user = false;
        const auto header = codexpets::macos::xiaoai_cookie_header(
            cookies, &has_mina_token, &has_user);
        if (!header.empty()) {
            [strongSelf finish:header error:@""];
        } else if (has_user && !has_mina_token && !strongSelf->_requestedMina) {
            strongSelf->_requestedMina = YES;
            [strongSelf->_webView loadRequest:[NSURLRequest requestWithURL:
                [NSURL URLWithString:[NSString stringWithUTF8String:kMinaUrl]]]];
        }
    }];
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    (void)webView;
    (void)navigation;
    [self collectCookies];
}

- (void)finish:(std::string)cookies error:(NSString*)error {
    if (_finished) return;
    _finished = YES;
    [_timer invalidate];
    _timer = nil;
    if (_window.parentWindow) [_window.parentWindow removeChildWindow:_window];
    [_window close];
    auto completed = std::move(_completed);
    g_login = nil;
    if (completed) completed(std::move(cookies), error.UTF8String ? error.UTF8String : "");
}

- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    [self finish:{} error:@"已取消小米网页登录"];
}

@end

namespace codexpets::macos {

void start_xiaomi_browser_login(
    NSWindow* owner, std::string user_data_folder,
    std::function<void(std::string cookies, std::string error)> completed) {
    (void)user_data_folder;
    if (g_login) {
        if (completed) completed({}, "小米登录窗口已经打开");
        return;
    }
    g_login = [[XiaomiBrowserLogin alloc] initWithOwner:owner completed:std::move(completed)];
    [g_login show];
}

} // namespace codexpets::macos

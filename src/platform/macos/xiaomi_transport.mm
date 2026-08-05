#include "xiaomi_transport.h"

#import <Foundation/Foundation.h>

#include <dispatch/dispatch.h>

#include <stdexcept>
#include <string>

namespace codexpets::macos {
namespace {

@interface XiaoAiRedirectDelegate : NSObject <NSURLSessionTaskDelegate>
@property(nonatomic) BOOL followRedirects;
@end

@implementation XiaoAiRedirectDelegate
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
    newRequest:(NSURLRequest*)request
    completionHandler:(void (^)(NSURLRequest*))completionHandler {
    (void)session;
    (void)task;
    (void)response;
    completionHandler(self.followRedirects ? request : nil);
}
@end

XiaoAiHttpResponse request(const XiaoAiHttpRequest& input) {
    @autoreleasepool {
        NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:input.url.c_str()]];
        if (!url) throw std::runtime_error("小米请求地址无效");
        NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
        request.HTTPMethod = [NSString stringWithUTF8String:input.method.c_str()];
        for (const auto& [name, value] : input.headers) {
            NSString* field = [NSString stringWithUTF8String:name.c_str()];
            NSString* text = [NSString stringWithUTF8String:value.c_str()];
            [request setValue:text forHTTPHeaderField:field];
        }
        if (!input.body.empty()) {
            request.HTTPBody = [NSData dataWithBytes:input.body.data() length:input.body.size()];
        }
        request.timeoutInterval = 20.0;

        XiaoAiRedirectDelegate* delegate = [XiaoAiRedirectDelegate new];
        delegate.followRedirects = input.follow_redirects;
        NSURLSessionConfiguration* configuration =
            [NSURLSessionConfiguration ephemeralSessionConfiguration];
        configuration.HTTPShouldSetCookies = NO;
        configuration.HTTPCookieStorage = nil;
        dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
        __block NSData* response_data = nil;
        __block NSURLResponse* response = nil;
        __block NSError* response_error = nil;
        NSURLSession* session = [NSURLSession sessionWithConfiguration:configuration
                                                               delegate:delegate
                                                          delegateQueue:nil];
        NSURLSessionDataTask* task = [session dataTaskWithRequest:request
                                                 completionHandler:
            ^(NSData* data, NSURLResponse* received, NSError* error) {
                response_data = data;
                response = received;
                response_error = error;
                dispatch_semaphore_signal(semaphore);
            }];
        [task resume];
        const auto wait_status = dispatch_semaphore_wait(
            semaphore, dispatch_time(DISPATCH_TIME_NOW, 25LL * NSEC_PER_SEC));
        if (wait_status != 0) {
            [task cancel];
            [session invalidateAndCancel];
            throw std::runtime_error("小米请求超时");
        }
        [session finishTasksAndInvalidate];
        if (response_error) {
            const char* text = response_error.localizedDescription.UTF8String;
            throw std::runtime_error(text ? text : "小米网络请求失败");
        }
        auto* http = static_cast<NSHTTPURLResponse*>(response);
        if (!http) throw std::runtime_error("小米响应无效");

        XiaoAiHttpResponse result;
        result.status = static_cast<int>(http.statusCode);
        if (response_data.length > 0) {
            result.body.assign(static_cast<const char*>(response_data.bytes), response_data.length);
        }
        for (id key in http.allHeaderFields) {
            id value = http.allHeaderFields[key];
            const char* name = [key.description UTF8String];
            const char* text = [value.description UTF8String];
            if (name && text) result.headers.emplace_back(name, text);
        }
        return result;
    }
}

} // namespace

XiaoAiHttpTransport make_xiaoai_http_transport() {
    return [](const XiaoAiHttpRequest& input) { return request(input); };
}

} // namespace codexpets::macos

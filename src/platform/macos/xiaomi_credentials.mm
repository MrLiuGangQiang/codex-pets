#include "xiaomi_credentials.h"

#include "../../../src/core/xiaomi_speaker.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

namespace codexpets::macos {
namespace {

NSString* const kService = @"com.mrliugangqiang.codexpets.xiaoai";
NSString* const kAccount = @"Xiaomi";

NSDictionary* keychain_query() {
    return @{
        (__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService: kService,
        (__bridge id)kSecAttrAccount: kAccount,
    };
}

std::string security_error(OSStatus status) {
    NSString* message = CFBridgingRelease(SecCopyErrorMessageString(status, nullptr));
    const char* text = message.UTF8String;
    return text ? std::string(text) : ("Keychain error " + std::to_string(status));
}

} // namespace

std::string load_xiaoai_authorization() {
    @autoreleasepool {
        NSMutableDictionary* query = [keychain_query() mutableCopy];
        query[(__bridge id)kSecReturnData] = @YES;
        CFTypeRef result = nullptr;
        if (SecItemCopyMatching((__bridge CFDictionaryRef)query, &result) != errSecSuccess || !result) {
            return {};
        }
        NSData* data = CFBridgingRelease(result);
        return std::string(static_cast<const char*>(data.bytes), data.length);
    }
}

bool save_xiaoai_authorization(std::string_view cookies, std::string* error) {
    @autoreleasepool {
        NSDictionary* query = keychain_query();
        if (cookies.empty()) {
            const auto status = SecItemDelete((__bridge CFDictionaryRef)query);
            if (status == errSecSuccess || status == errSecItemNotFound) return true;
            if (error) *error = "删除小米授权信息失败：" + security_error(status);
            return false;
        }
        const auto compact = codexpets::compact_xiaoai_authorization(cookies);
        if (compact.empty()) {
            if (error) *error = "小米授权信息不完整，未保存";
            return false;
        }
        NSMutableDictionary* attributes = [query mutableCopy];
        attributes[(__bridge id)kSecValueData] =
            [NSData dataWithBytes:compact.data() length:compact.size()];
        attributes[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlock;
        const auto delete_status = SecItemDelete((__bridge CFDictionaryRef)query);
        if (delete_status != errSecSuccess && delete_status != errSecItemNotFound) {
            if (error) *error = "更新小米授权信息失败：" + security_error(delete_status);
            return false;
        }
        const auto status = SecItemAdd((__bridge CFDictionaryRef)attributes, nullptr);
        if (status == errSecSuccess) return true;
        if (error) *error = "保存小米授权信息失败：" + security_error(status);
        return false;
    }
}

} // namespace codexpets::macos

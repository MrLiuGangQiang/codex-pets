#include "telegram_credentials.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

namespace codexpets::macos {
namespace {

NSString* const kService = @"com.mrliugangqiang.codexpets.telegram";
NSString* const kAccount = @"BotToken";

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

std::string load_telegram_bot_token() {
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

bool save_telegram_bot_token(std::string_view token, std::string* error) {
    @autoreleasepool {
        NSDictionary* query = keychain_query();
        const auto delete_status = SecItemDelete((__bridge CFDictionaryRef)query);
        if (delete_status != errSecSuccess && delete_status != errSecItemNotFound) {
            if (error) *error = "更新 Telegram Bot Token 失败：" + security_error(delete_status);
            return false;
        }
        if (token.empty()) return true;
        NSMutableDictionary* attributes = [query mutableCopy];
        attributes[(__bridge id)kSecValueData] =
            [NSData dataWithBytes:token.data() length:token.size()];
        attributes[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlock;
        const auto status = SecItemAdd((__bridge CFDictionaryRef)attributes, nullptr);
        if (status == errSecSuccess) return true;
        if (error) *error = "保存 Telegram Bot Token 失败：" + security_error(status);
        return false;
    }
}

} // namespace codexpets::macos

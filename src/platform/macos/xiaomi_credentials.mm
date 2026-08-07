#include "xiaomi_credentials.h"

#include "../../../src/core/xiaomi_speaker.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

namespace codexpets::macos {
namespace {

NSString* const kService = @"com.mrliugangqiang.codexpets.xiaoai";
NSString* const kMinaAccount = @"Xiaomi";
NSString* const kMiotSsecurityAccount = @"Xiaomi.MiotSsecurity";
NSString* const kMiotServiceTokenAccount = @"Xiaomi.MiotServiceToken";

NSDictionary* keychain_query(NSString* account) {
    return @{
        (__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService: kService,
        (__bridge id)kSecAttrAccount: account,
    };
}

std::string security_error(OSStatus status) {
    NSString* message = CFBridgingRelease(SecCopyErrorMessageString(status, nullptr));
    const char* text = message.UTF8String;
    return text ? std::string(text) : ("Keychain error " + std::to_string(status));
}

std::string read_credential(NSString* account) {
    NSMutableDictionary* query = [keychain_query(account) mutableCopy];
    query[(__bridge id)kSecReturnData] = @YES;
    CFTypeRef result = nullptr;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)query, &result) != errSecSuccess || !result) return {};
    NSData* data = CFBridgingRelease(result);
    return std::string(static_cast<const char*>(data.bytes), data.length);
}

bool delete_credential(NSString* account, std::string_view label, std::string* error) {
    const auto status = SecItemDelete((__bridge CFDictionaryRef)keychain_query(account));
    if (status == errSecSuccess || status == errSecItemNotFound) return true;
    if (error) *error = std::string(label) + "删除失败：" + security_error(status);
    return false;
}

bool write_credential(NSString* account, std::string_view value, std::string_view label,
                      std::string* error) {
    NSDictionary* query = keychain_query(account);
    NSMutableDictionary* attributes = [query mutableCopy];
    attributes[(__bridge id)kSecValueData] =
        [NSData dataWithBytes:value.data() length:value.size()];
    attributes[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlock;
    const auto delete_status = SecItemDelete((__bridge CFDictionaryRef)query);
    if (delete_status != errSecSuccess && delete_status != errSecItemNotFound) {
        if (error) *error = std::string(label) + "更新失败：" + security_error(delete_status);
        return false;
    }
    const auto status = SecItemAdd((__bridge CFDictionaryRef)attributes, nullptr);
    if (status == errSecSuccess) return true;
    if (error) *error = std::string(label) + "保存失败：" + security_error(status);
    return false;
}

} // namespace

std::string load_xiaoai_authorization() {
    @autoreleasepool {
        XiaoAiAuthorizationParts parts;
        parts.mina = read_credential(kMinaAccount);
        if (parts.mina.empty()) return {};
        parts.miot_ssecurity = read_credential(kMiotSsecurityAccount);
        parts.miot_service_token = read_credential(kMiotServiceTokenAccount);
        return combine_xiaoai_authorization(parts);
    }
}

bool save_xiaoai_authorization(std::string_view cookies, std::string* error) {
    @autoreleasepool {
        if (cookies.empty()) {
            const bool mina_deleted = delete_credential(kMinaAccount, "小米授权信息", error);
            const bool security_deleted = delete_credential(kMiotSsecurityAccount, "小米 MiOT 安全票据", error);
            const bool token_deleted = delete_credential(kMiotServiceTokenAccount, "小米 MiOT 服务票据", error);
            return mina_deleted && security_deleted && token_deleted;
        }
        const auto parts = split_xiaoai_authorization(cookies);
        if (parts.mina.empty()) {
            if (error) *error = "小米授权信息不完整，未保存";
            return false;
        }
        const auto save_optional = [&](NSString* account, std::string_view value, std::string_view label) {
            return value.empty() ? delete_credential(account, label, error)
                                 : write_credential(account, value, label, error);
        };
        return save_optional(kMiotSsecurityAccount, parts.miot_ssecurity, "小米 MiOT 安全票据") &&
            save_optional(kMiotServiceTokenAccount, parts.miot_service_token, "小米 MiOT 服务票据") &&
            write_credential(kMinaAccount, parts.mina, "小米 MiNA 授权票据", error);
    }
}

} // namespace codexpets::macos

#include "xiaomi_credentials.h"

#include "../../../src/core/xiaomi_speaker.h"

#include <windows.h>
#include <wincred.h>

#include <string>

namespace codexpets::windows {
namespace {

constexpr wchar_t kMinaCredentialTarget[] = L"CodeXPets.XiaoAiAuth";
constexpr wchar_t kMiotSsecurityCredentialTarget[] = L"CodeXPets.XiaoAiMiotSsecurity";
constexpr wchar_t kMiotServiceTokenCredentialTarget[] = L"CodeXPets.XiaoAiMiotServiceToken";
constexpr wchar_t kLegacyCredentialTarget[] = L"CodeXPets.XiaoAi";

std::string win32_error(std::string_view action, DWORD code) {
    return std::string(action) + "（Win32 错误 " + std::to_string(code) + "）";
}

std::string read_credential(const wchar_t* target) {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target, CRED_TYPE_GENERIC, 0, &credential) || !credential) return {};
    std::string value;
    if (credential->CredentialBlob && credential->CredentialBlobSize > 0) {
        value.assign(reinterpret_cast<const char*>(credential->CredentialBlob),
                     credential->CredentialBlobSize);
    }
    CredFree(credential);
    return value;
}

bool delete_credential(const wchar_t* target, std::string_view action, std::string* error) {
    if (CredDeleteW(target, CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND) return true;
    if (error) *error = win32_error(action, GetLastError());
    return false;
}

bool write_credential(const wchar_t* target, std::string_view value, std::string_view label,
                      std::string* error) {
    if (value.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        if (error) *error = std::string(label) + "超过 Windows 单条凭据存储上限，未保存";
        return false;
    }
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(target);
    credential.CredentialBlobSize = static_cast<DWORD>(value.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(value.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t*>(L"Xiaomi");
    if (CredWriteW(&credential, 0)) return true;
    if (error) *error = win32_error(std::string("保存") + std::string(label) + "失败", GetLastError());
    return false;
}

} // namespace

std::string load_xiaoai_authorization() {
    XiaoAiAuthorizationParts parts;
    parts.mina = read_credential(kMinaCredentialTarget);
    if (parts.mina.empty()) return {};
    parts.miot_ssecurity = read_credential(kMiotSsecurityCredentialTarget);
    parts.miot_service_token = read_credential(kMiotServiceTokenCredentialTarget);
    return combine_xiaoai_authorization(parts);
}

bool save_xiaoai_authorization(std::string_view cookies, std::string* error) {
    if (cookies.empty()) {
        const bool mina_deleted = delete_credential(kMinaCredentialTarget, "删除小米授权信息失败", error);
        const bool ssecurity_deleted = delete_credential(kMiotSsecurityCredentialTarget,
            "删除小米 MiOT 授权信息失败", error);
        const bool token_deleted = delete_credential(kMiotServiceTokenCredentialTarget,
            "删除小米 MiOT 授权信息失败", error);
        return mina_deleted && ssecurity_deleted && token_deleted;
    }

    const auto parts = split_xiaoai_authorization(cookies);
    if (parts.mina.empty()) {
        if (error) *error = "小米授权信息不完整，未保存";
        return false;
    }
    // Each value gets a dedicated Credential Manager entry.  MiOT tokens can be
    // substantially larger than MiNA tokens, while Windows caps each blob at 2560 bytes.
    const auto save_optional = [&](const wchar_t* target, std::string_view value,
                                   std::string_view label) {
        return value.empty() ? delete_credential(target, std::string("删除") + std::string(label) + "失败", error)
                             : write_credential(target, value, label, error);
    };
    if (!save_optional(kMiotSsecurityCredentialTarget, parts.miot_ssecurity, "小米 MiOT 安全票据") ||
        !save_optional(kMiotServiceTokenCredentialTarget, parts.miot_service_token, "小米 MiOT 服务票据") ||
        !write_credential(kMinaCredentialTarget, parts.mina, "小米 MiNA 授权票据", error)) {
        return false;
    }
    return true;
}

void remove_legacy_xiaoai_authorization() noexcept {
    CredDeleteW(kLegacyCredentialTarget, CRED_TYPE_GENERIC, 0);
}

} // namespace codexpets::windows

#include "xiaomi_credentials.h"

#include "../../../src/core/xiaomi_speaker.h"

#include <windows.h>
#include <wincred.h>

#include <string>

namespace codexpets::windows {
namespace {

constexpr wchar_t kCredentialTarget[] = L"CodeXPets.XiaoAiAuth";
constexpr wchar_t kLegacyCredentialTarget[] = L"CodeXPets.XiaoAi";

std::string win32_error(std::string_view action, DWORD code) {
    return std::string(action) + "（Win32 错误 " + std::to_string(code) + "）";
}

} // namespace

std::string load_xiaoai_authorization() {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(kCredentialTarget, CRED_TYPE_GENERIC, 0, &credential) || !credential) return {};
    std::string cookies;
    if (credential->CredentialBlob && credential->CredentialBlobSize > 0) {
        cookies.assign(reinterpret_cast<const char*>(credential->CredentialBlob),
                       credential->CredentialBlobSize);
    }
    CredFree(credential);
    return cookies;
}

bool save_xiaoai_authorization(std::string_view cookies, std::string* error) {
    if (cookies.empty()) {
        if (CredDeleteW(kCredentialTarget, CRED_TYPE_GENERIC, 0)) return true;
        const auto code = GetLastError();
        if (code == ERROR_NOT_FOUND) return true;
        if (error) *error = win32_error("删除小米授权信息失败", code);
        return false;
    }
    const auto compact = codexpets::compact_xiaoai_authorization(cookies);
    if (compact.empty()) {
        if (error) *error = "小米授权信息不完整，未保存";
        return false;
    }
    if (compact.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        if (error) *error = "小米授权令牌超过 Windows 凭据存储上限，未保存";
        return false;
    }
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(kCredentialTarget);
    credential.CredentialBlobSize = static_cast<DWORD>(compact.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(compact.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t*>(L"Xiaomi");
    if (CredWriteW(&credential, 0)) return true;
    if (error) *error = win32_error("保存小米授权信息失败", GetLastError());
    return false;
}

void remove_legacy_xiaoai_authorization() noexcept {
    CredDeleteW(kLegacyCredentialTarget, CRED_TYPE_GENERIC, 0);
}

} // namespace codexpets::windows

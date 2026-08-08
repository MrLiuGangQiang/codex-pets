#include "telegram_credentials.h"

#include <windows.h>
#include <wincred.h>

namespace codexpets::windows {
namespace {

constexpr wchar_t kCredentialTarget[] = L"CodeXPets.TelegramBotToken";

std::string win32_error(std::string_view action, DWORD code) {
    return std::string(action) + "（Win32 错误 " + std::to_string(code) + "）";
}

} // namespace

std::string load_telegram_bot_token() {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(kCredentialTarget, CRED_TYPE_GENERIC, 0, &credential) || !credential) return {};
    std::string value;
    if (credential->CredentialBlob && credential->CredentialBlobSize > 0) {
        value.assign(reinterpret_cast<const char*>(credential->CredentialBlob),
                     credential->CredentialBlobSize);
    }
    CredFree(credential);
    return value;
}

bool save_telegram_bot_token(std::string_view token, std::string* error) {
    if (token.empty()) {
        if (CredDeleteW(kCredentialTarget, CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND) {
            return true;
        }
        if (error) *error = win32_error("删除 Telegram Bot Token 失败", GetLastError());
        return false;
    }
    if (token.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        if (error) *error = "Telegram Bot Token 超过 Windows 凭据存储上限";
        return false;
    }
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(kCredentialTarget);
    credential.CredentialBlobSize = static_cast<DWORD>(token.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(token.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t*>(L"Telegram");
    if (CredWriteW(&credential, 0)) return true;
    if (error) *error = win32_error("保存 Telegram Bot Token 失败", GetLastError());
    return false;
}

} // namespace codexpets::windows

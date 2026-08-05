#include "xiaomi_transport.h"

#include "../../../src/core/platform_text.h"

#include <windows.h>
#include <winhttp.h>

#include <string>

namespace codexpets::windows {
namespace {

XiaoAiHttpResponse request(const XiaoAiHttpRequest& input) {
    XiaoAiHttpResponse result;
    const auto url = utf8_to_wide(input.url);
    URL_COMPONENTSW components{sizeof(components)};
    wchar_t host[512]{}; wchar_t path[8192]{}; wchar_t extra[4096]{};
    components.lpszHostName = host; components.dwHostNameLength = ARRAYSIZE(host);
    components.lpszUrlPath = path; components.dwUrlPathLength = ARRAYSIZE(path);
    components.lpszExtraInfo = extra; components.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) return result;

    HINTERNET session = WinHttpOpen(L"CodeXPets/XiaoAi", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return result;
    WinHttpSetTimeouts(session, 20000, 20000, 20000, 20000);
    HINTERNET connection = WinHttpConnect(session, host, components.nPort, 0);
    if (!connection) { WinHttpCloseHandle(session); return result; }
    const auto flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request_handle = WinHttpOpenRequest(connection, utf8_to_wide(input.method).c_str(),
                                                   (std::wstring(path) + extra).c_str(), nullptr,
                                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request_handle) { WinHttpCloseHandle(connection); WinHttpCloseHandle(session); return result; }
    if (!input.follow_redirects) {
        DWORD disable = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(request_handle, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
    }
    std::wstring headers;
    for (const auto& [name, value] : input.headers) {
        headers += utf8_to_wide(name) + L": " + utf8_to_wide(value) + L"\r\n";
    }
    const auto body = utf8_to_wide(input.body); // body is ASCII/form or UTF-8 JSON only in this protocol.
    const auto* body_bytes = input.body.empty() ? WINHTTP_NO_REQUEST_DATA
                                                 : reinterpret_cast<const void*>(input.body.data());
    if (!WinHttpSendRequest(request_handle, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1L),
                            const_cast<void*>(body_bytes), static_cast<DWORD>(input.body.size()),
                            static_cast<DWORD>(input.body.size()), 0) ||
        !WinHttpReceiveResponse(request_handle, nullptr)) {
        WinHttpCloseHandle(request_handle); WinHttpCloseHandle(connection); WinHttpCloseHandle(session); return result;
    }
    DWORD status{}, status_size = sizeof(status);
    WinHttpQueryHeaders(request_handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    result.status = static_cast<int>(status);
    DWORD header_size{};
    WinHttpQueryHeaders(request_handle, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                        nullptr, &header_size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && header_size > 0) {
        std::wstring raw(header_size / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(request_handle, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                raw.data(), &header_size, WINHTTP_NO_HEADER_INDEX)) {
            std::size_t start = 0;
            while (start < raw.size()) {
                const auto end = raw.find(L'\n', start);
                const auto line = raw.substr(start, end == std::wstring::npos ? end : end - start);
                const auto colon = line.find(L':');
                if (colon != std::wstring::npos) {
                    auto name = wide_to_utf8(line.substr(0, colon));
                    auto value = wide_to_utf8(line.substr(colon + 1));
                    while (!value.empty() && (value.front() == ' ' || value.front() == '\r')) value.erase(value.begin());
                    while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
                    result.headers.emplace_back(std::move(name), std::move(value));
                }
                if (end == std::wstring::npos) break;
                start = end + 1;
            }
        }
    }
    for (;;) {
        DWORD available{};
        if (!WinHttpQueryDataAvailable(request_handle, &available) || available == 0) break;
        std::string buffer(available, '\0'); DWORD read{};
        if (!WinHttpReadData(request_handle, buffer.data(), available, &read) || read == 0) break;
        result.body.append(buffer.data(), read);
    }
    WinHttpCloseHandle(request_handle); WinHttpCloseHandle(connection); WinHttpCloseHandle(session);
    return result;
}

} // namespace

XiaoAiHttpTransport make_xiaoai_http_transport() {
    return [](const XiaoAiHttpRequest& input) { return request(input); };
}

} // namespace codexpets::windows

#include "telegram_transport.h"

#include "xiaomi_transport.h"

#include <utility>

namespace codexpets::windows {

TelegramHttpTransport make_telegram_http_transport() {
    auto transport = make_xiaoai_http_transport();
    return [transport = std::move(transport)](const TelegramHttpRequest& input) {
        XiaoAiHttpRequest request;
        request.method = input.method;
        request.url = input.url;
        request.body = input.body;
        request.headers = input.headers;
        const auto response = transport(request);
        return TelegramHttpResponse{response.status, response.body};
    };
}

} // namespace codexpets::windows

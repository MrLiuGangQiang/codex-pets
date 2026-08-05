#pragma once

#include "settings.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace codexpets {

enum class XiaoAiEvent : unsigned char { Started, Completed, Error, Interrupted };

struct XiaoAiHttpRequest {
    std::string method;
    std::string url;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    // Service-token redemption must expose the original 302 Set-Cookie response.
    bool follow_redirects{true};
};

struct XiaoAiHttpResponse {
    int status{};
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

using XiaoAiHttpTransport = std::function<XiaoAiHttpResponse(const XiaoAiHttpRequest&)>;

struct XiaoAiDeviceInfo {
    std::string id;
    std::string name;
    std::string alias;
    std::string hardware;
};

// Keeps only the MiNA session cookies that are needed by the notifier.
// Callers should persist the result in the platform secure credential store.
[[nodiscard]] std::string compact_xiaoai_authorization(std::string_view cookies);

class XiaoAiNotifier {
public:
    explicit XiaoAiNotifier(XiaoAiHttpTransport transport);
    ~XiaoAiNotifier();
    XiaoAiNotifier(const XiaoAiNotifier&) = delete;
    XiaoAiNotifier& operator=(const XiaoAiNotifier&) = delete;

    void configure(const XiaoAiSettings& settings);
    void notify(XiaoAiEvent event, std::string_view context_label = {});
    // Verifies that the saved authorization can access the MiNA device list without speaking.
    bool validate(XiaoAiSettings& settings, std::string* error);
    bool discover_devices(const XiaoAiSettings& settings, std::vector<XiaoAiDeviceInfo>* devices, std::string* error);
    bool test(const XiaoAiSettings& settings, std::string* error);
    void stop() noexcept;

private:
    struct Job {
        XiaoAiSettings settings;
        XiaoAiEvent event{};
        std::string context_label;
    };

    void worker_loop();
    XiaoAiHttpTransport transport_;
    XiaoAiSettings settings_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<Job> jobs_;
    bool stopping_{};
    std::thread worker_;
};

} // namespace codexpets

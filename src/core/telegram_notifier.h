#pragma once

#include "settings.h"
#include "types.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace codexpets {

struct TelegramHttpRequest {
    std::string method;
    std::string url;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

struct TelegramHttpResponse {
    int status{};
    std::string body;
};

using TelegramHttpTransport = std::function<TelegramHttpResponse(const TelegramHttpRequest&)>;

[[nodiscard]] std::string format_telegram_task_card(
    const TaskNotification& notification,
    SystemClock::time_point timestamp = SystemClock::now());

class TelegramNotifier {
public:
    explicit TelegramNotifier(TelegramHttpTransport transport);
    ~TelegramNotifier();
    TelegramNotifier(const TelegramNotifier&) = delete;
    TelegramNotifier& operator=(const TelegramNotifier&) = delete;

    void configure(const TelegramSettings& settings);
    void notify(const TaskNotification& notification);
    bool test(const TelegramSettings& settings, std::string* error);
    using TestCallback = std::function<void(std::string)>;
    void test_async(TelegramSettings settings, TestCallback callback);
    void stop() noexcept;

private:
    struct Job {
        TelegramSettings settings;
        TaskNotification notification;
    };

    bool send(const TelegramSettings& settings, const TaskNotification& notification,
              std::string* error) const;
    void worker_loop();

    TelegramHttpTransport transport_;
    TelegramSettings settings_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<Job> jobs_;
    std::queue<std::function<void()>> control_tasks_;
    bool stopping_{};
    std::thread worker_;
};

} // namespace codexpets

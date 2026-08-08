#include "telegram_notifier.h"

#include "json.h"
#include "platform_text.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace codexpets {
namespace {

constexpr std::size_t kMaximumProjectBytes = 120;
constexpr std::size_t kMaximumTaskBytes = 260;
constexpr std::size_t kMaximumStepBytes = 180;
constexpr std::size_t kMaximumSummaryBytes = 700;
constexpr std::size_t kMaximumVisibleSteps = 10;

std::string truncate_utf8(std::string value, std::size_t maximum_bytes) {
    value = trim_ascii(value);
    if (value.size() <= maximum_bytes) return value;
    std::size_t end = maximum_bytes;
    while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U) --end;
    value.resize(end);
    value += "…";
    return value;
}

std::string html_escape(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    for (const char ch : input) {
        switch (ch) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            default: result.push_back(ch); break;
        }
    }
    return result;
}

std::string remove_markdown_tokens(std::string line) {
    const auto erase_all = [&](std::string_view token) {
        std::size_t position{};
        while ((position = line.find(token, position)) != std::string::npos) {
            line.erase(position, token.size());
        }
    };
    erase_all("**");
    erase_all("__");
    erase_all("`");
    while (!line.empty() && line.front() == '#') line.erase(line.begin());
    line = trim_ascii(line);
    if (line.size() >= 2 && (line.starts_with("- ") || line.starts_with("* "))) {
        line.replace(0, 2, "• ");
    }
    return line;
}

std::string clean_summary(std::string_view raw) {
    std::istringstream input{std::string(raw)};
    std::ostringstream output;
    std::string line;
    bool in_code_block = false;
    bool previous_blank = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto trimmed = trim_ascii(line);
        if (trimmed.starts_with("```")) {
            in_code_block = !in_code_block;
            continue;
        }
        if (in_code_block) continue;
        auto cleaned = remove_markdown_tokens(std::move(line));
        const bool blank = cleaned.empty();
        if (blank && previous_blank) continue;
        if (output.tellp() > 0) output << '\n';
        if (!blank) output << cleaned;
        previous_blank = blank;
    }
    return truncate_utf8(trim_ascii(output.str()), kMaximumSummaryBytes);
}

std::string status_icon(TaskNotificationState state) {
    switch (state) {
        case TaskNotificationState::Completed: return "✅";
        case TaskNotificationState::Error: return "❌";
        case TaskNotificationState::Interrupted: return "⏹";
        case TaskNotificationState::Started:
        default: return "🔵";
    }
}

std::string status_text(TaskNotificationState state) {
    switch (state) {
        case TaskNotificationState::Completed: return "已完成";
        case TaskNotificationState::Error: return "出现异常";
        case TaskNotificationState::Interrupted: return "已中断";
        case TaskNotificationState::Started:
        default: return "进行中";
    }
}

std::string step_icon(TaskStepState state) {
    switch (state) {
        case TaskStepState::Completed: return "✅";
        case TaskStepState::InProgress: return "🔵";
        case TaskStepState::Error: return "❌";
        case TaskStepState::Interrupted: return "⏹";
        case TaskStepState::Pending:
        default: return "⚪";
    }
}

TaskStep fallback_step(TaskNotificationState state) {
    switch (state) {
        case TaskNotificationState::Completed:
            return {"任务已完成", TaskStepState::Completed};
        case TaskNotificationState::Error:
            return {"任务执行失败", TaskStepState::Error};
        case TaskNotificationState::Interrupted:
            return {"任务已中断", TaskStepState::Interrupted};
        case TaskNotificationState::Started:
        default:
            return {"任务已开始", TaskStepState::InProgress};
    }
}

std::string default_summary(const TaskNotification& notification,
                            const std::vector<TaskStep>& steps) {
    if (notification.state == TaskNotificationState::Interrupted) {
        const auto normalized = lowercase_ascii(trim_ascii(notification.summary));
        if (normalized == "interrupted" || normalized == "cancelled" || normalized == "canceled") {
            return "任务被用户中断。";
        }
        const auto cleaned = clean_summary(notification.summary);
        return cleaned.empty() ? "任务被中断，未提供更多原因。" : cleaned;
    }
    if (notification.state == TaskNotificationState::Error) {
        const auto cleaned = clean_summary(notification.summary);
        return cleaned.empty() ? "任务执行失败，未提供更多错误信息。" : cleaned;
    }
    if (!notification.summary.empty()) {
        const auto cleaned = clean_summary(notification.summary);
        if (!cleaned.empty()) return cleaned;
    }
    if (notification.state == TaskNotificationState::Completed) return "任务已完成。";
    for (const auto& step : steps) {
        if (step.state == TaskStepState::InProgress && !step.text.empty()) {
            return "正在执行：" + truncate_utf8(step.text, kMaximumStepBytes) + "。";
        }
    }
    return "任务已经开始，正在等待后续执行结果。";
}

std::string format_time(SystemClock::time_point value) {
    const auto raw = SystemClock::to_time_t(value);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &raw);
#else
    localtime_r(&raw, &local);
#endif
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

bool event_enabled(const TelegramSettings& settings, TaskNotificationState state) noexcept {
    switch (state) {
        case TaskNotificationState::Started: return settings.notify_started;
        case TaskNotificationState::Completed: return settings.notify_completed;
        case TaskNotificationState::Error: return settings.notify_error;
        case TaskNotificationState::Interrupted: return settings.notify_interrupted;
    }
    return false;
}

std::string response_error(const TelegramHttpResponse& response) {
    try {
        const auto root = parse_json(response.body);
        if (const auto* description = root.get("description"); description && description->is_string()) {
            const auto text = trim_ascii(description->string());
            if (!text.empty()) return text;
        }
    } catch (...) {}
    if (response.status == 0) return "Telegram 网络请求失败";
    return "Telegram 返回 HTTP " + std::to_string(response.status);
}

} // namespace

std::string format_telegram_task_card(const TaskNotification& notification,
                                      SystemClock::time_point timestamp) {
    const auto project = truncate_utf8(notification.project_name.empty()
        ? std::string("未命名项目") : notification.project_name, kMaximumProjectBytes);
    const auto task = truncate_utf8(notification.task_title.empty()
        ? std::string("未命名任务") : notification.task_title, kMaximumTaskBytes);
    auto steps = notification.steps;
    if (steps.empty()) steps.push_back(fallback_step(notification.state));

    const auto completed = static_cast<int>(std::count_if(steps.begin(), steps.end(),
        [](const TaskStep& step) { return step.state == TaskStepState::Completed; }));
    const auto summary = default_summary(notification, steps);

    std::ostringstream output;
    output << status_icon(notification.state) << " <b>" << html_escape(project) << " · "
           << status_text(notification.state) << "</b>\n\n"
           << "<b>任务</b>　" << html_escape(task) << "\n\n"
           << "<b>步骤 · " << completed << " / " << steps.size() << "</b>\n";
    const auto visible = std::min(steps.size(), kMaximumVisibleSteps);
    for (std::size_t index = 0; index < visible; ++index) {
        const auto text = truncate_utf8(steps[index].text.empty()
            ? std::string("未命名步骤") : steps[index].text, kMaximumStepBytes);
        output << step_icon(steps[index].state) << ' ' << html_escape(text) << '\n';
    }
    if (steps.size() > visible) {
        output << "⚪ 其余 " << (steps.size() - visible) << " 个步骤\n";
    }
    output << "\n<blockquote>" << html_escape(summary) << "</blockquote>\n"
           << "<i>" << format_time(timestamp) << "</i>";
    return output.str();
}

TelegramNotifier::TelegramNotifier(TelegramHttpTransport transport)
    : transport_(std::move(transport)), worker_([this] { worker_loop(); }) {}

TelegramNotifier::~TelegramNotifier() { stop(); }

void TelegramNotifier::configure(const TelegramSettings& settings) {
    std::lock_guard lock(mutex_);
    settings_ = settings;
}

void TelegramNotifier::notify(const TaskNotification& notification) {
    std::lock_guard lock(mutex_);
    if (stopping_ || !settings_.enabled || settings_.bot_token.empty() ||
        settings_.chat_id.empty() || !event_enabled(settings_, notification.state)) return;
    jobs_.push(Job{settings_, notification});
    condition_.notify_one();
}

bool TelegramNotifier::send(const TelegramSettings& settings,
                            const TaskNotification& notification,
                            std::string* error) const {
    if (settings.bot_token.empty()) {
        if (error) *error = "请填写 Bot Token";
        return false;
    }
    if (settings.chat_id.empty()) {
        if (error) *error = "请填写用户或 Chat ID";
        return false;
    }
    try {
        TelegramHttpRequest request;
        request.method = "POST";
        request.url = "https://api.telegram.org/bot" + settings.bot_token + "/sendMessage";
        request.headers.emplace_back("Content-Type", "application/json; charset=utf-8");
        request.body = "{\"chat_id\":\"" + json_escape(settings.chat_id) +
            "\",\"text\":\"" + json_escape(format_telegram_task_card(notification)) +
            "\",\"parse_mode\":\"HTML\",\"disable_web_page_preview\":true}";
        const auto response = transport_(request);
        if (response.status < 200 || response.status >= 300) {
            if (error) *error = response_error(response);
            return false;
        }
        const auto root = parse_json(response.body);
        const auto* ok = root.get("ok");
        if (!ok || !ok->is_boolean() || !ok->boolean()) {
            if (error) *error = response_error(response);
            return false;
        }
        if (error) error->clear();
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    } catch (...) {
        if (error) *error = "Telegram 推送发生未知错误";
        return false;
    }
}

bool TelegramNotifier::test(const TelegramSettings& settings, std::string* error) {
    TaskNotification notification;
    notification.state = TaskNotificationState::Completed;
    notification.project_name = "CodeXPets";
    notification.task_title = "测试 Telegram 任务通知";
    notification.steps = {{"验证 Bot Token 与聊天 ID", TaskStepState::Completed}};
    notification.summary = "Telegram 配置有效，测试消息已成功发送。";
    return send(settings, notification, error);
}

void TelegramNotifier::test_async(TelegramSettings settings, TestCallback callback) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        control_tasks_.push([this, settings = std::move(settings), callback = std::move(callback)]() mutable {
            std::string error;
            (void)test(settings, &error);
            if (callback) callback(std::move(error));
        });
    }
    condition_.notify_one();
}

void TelegramNotifier::worker_loop() {
    for (;;) {
        std::function<void()> control;
        std::optional<Job> job;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] {
                return stopping_ || !control_tasks_.empty() || !jobs_.empty();
            });
            if (stopping_ && control_tasks_.empty() && jobs_.empty()) return;
            if (!control_tasks_.empty()) {
                control = std::move(control_tasks_.front());
                control_tasks_.pop();
            } else if (!jobs_.empty()) {
                job = std::move(jobs_.front());
                jobs_.pop();
            }
        }
        if (control) {
            try { control(); } catch (...) {}
        } else if (job) {
            std::string ignored;
            (void)send(job->settings, job->notification, &ignored);
        }
    }
}

void TelegramNotifier::stop() noexcept {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
}

} // namespace codexpets

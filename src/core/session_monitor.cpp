#include "session_monitor.h"

#include "json.h"
#include "paths.h"
#include "platform_text.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <malloc/malloc.h>
#endif

namespace codexpets {
namespace {

constexpr std::size_t kReadBufferSize = 64 * 1024;
constexpr std::size_t kMaximumBufferedLineBytes = 256 * 1024;
constexpr std::size_t kRetainedLineBufferBytes = 16 * 1024;
constexpr std::size_t kInitialLineBufferBytes = 512;
constexpr std::size_t kMaximumTrackedFiles = 40;
constexpr std::size_t kMaximumPendingToolCalls = 64;
constexpr auto kMaximumPendingToolCallAge = std::chrono::hours(6);
constexpr auto kFullDiscoveryInterval = std::chrono::seconds(600);
constexpr auto kFastDiscoveryInterval = std::chrono::milliseconds(1200);
constexpr auto kStaleTurnCheckInterval = std::chrono::seconds(30);
constexpr auto kDiagnosticsSnapshotInterval = std::chrono::seconds(5);
constexpr int kStaleTurnGraceSeconds = 600;

struct PathHash {
    std::size_t operator()(const std::filesystem::path& path) const noexcept {
#ifdef _WIN32
        auto text = path.native();
        std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
            return ch >= L'A' && ch <= L'Z' ? static_cast<wchar_t>(ch + 32) : ch;
        });
        return std::hash<std::wstring>{}(text);
#else
        return std::filesystem::hash_value(path);
#endif
    }
};

struct PathEqual {
    bool operator()(const std::filesystem::path& left,
                    const std::filesystem::path& right) const noexcept {
#ifdef _WIN32
        const auto a = left.native();
        const auto b = right.native();
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
            if (x >= L'A' && x <= L'Z') x = static_cast<wchar_t>(x + 32);
            if (y >= L'A' && y <= L'Z') y = static_cast<wchar_t>(y + 32);
            return x == y;
        });
#else
        return left == right;
#endif
    }
};

bool session_file_may_have_live_writer(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        return false;
    }
    const auto error = GetLastError();
    return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
#elif defined(__APPLE__)
    try {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error) return false;
        auto target = std::filesystem::weakly_canonical(path, error);
        if (error) target = path.lexically_normal();
        const auto target_text = target.string();
        const int required_bytes = proc_listpidspath(
            PROC_ALL_PIDS, 0, target_text.c_str(), PROC_LISTPIDSPATH_EXCLUDE_EVTONLY,
            nullptr, 0);
        if (required_bytes < 0) return true;
        if (required_bytes == 0) return false;
        std::vector<pid_t> pids(static_cast<std::size_t>(required_bytes) / sizeof(pid_t) + 8);
        const int listed_bytes = proc_listpidspath(
            PROC_ALL_PIDS, 0, target_text.c_str(), PROC_LISTPIDSPATH_EXCLUDE_EVTONLY,
            pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
        return listed_bytes != 0;
    } catch (...) {
        // Failure to inspect process handles must not make a real long turn look idle.
        return true;
    }
#else
    (void)path;
    return false;
#endif
}

SystemClock::time_point file_time_to_system(std::filesystem::file_time_type value) noexcept {
    return SystemClock::now() + std::chrono::duration_cast<SystemClock::duration>(
        value - std::filesystem::file_time_type::clock::now());
}

SystemClock::time_point safe_last_write(const std::filesystem::path& path) noexcept {
    std::error_code error;
    const auto value = std::filesystem::last_write_time(path, error);
    return error ? SystemClock::time_point::min() : file_time_to_system(value);
}

bool complete_json(std::string_view value) noexcept {
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r' || value.back() == '\n')) value.remove_suffix(1);
    return value.size() > 1 && value.back() == '}';
}

std::string_view normalize_json_line(std::string_view value) noexcept {
    if (value.size() >= 3 && static_cast<unsigned char>(value[0]) == 0xef &&
        static_cast<unsigned char>(value[1]) == 0xbb &&
        static_cast<unsigned char>(value[2]) == 0xbf) value.remove_prefix(3);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    return value;
}

#ifdef _WIN32
std::time_t timegm_portable(std::tm* value) { return _mkgmtime(value); }
#else
std::time_t timegm_portable(std::tm* value) { return timegm(value); }
#endif

std::optional<SystemClock::time_point> parse_timestamp(std::string_view raw) noexcept {
    try {
        if (raw.size() < 19) return std::nullopt;
        auto integer = [&](std::size_t offset, std::size_t count) -> std::optional<int> {
            int value{};
            const auto* first = raw.data() + offset;
            const auto* last = first + count;
            const auto result = std::from_chars(first, last, value);
            if (result.ec != std::errc{} || result.ptr != last) return std::nullopt;
            return value;
        };
        const auto year = integer(0, 4), month = integer(5, 2), day = integer(8, 2);
        const auto hour = integer(11, 2), minute = integer(14, 2), second = integer(17, 2);
        if (!year || !month || !day || !hour || !minute || !second ||
            raw[4] != '-' || raw[7] != '-' || (raw[10] != 'T' && raw[10] != ' ') ||
            raw[13] != ':' || raw[16] != ':') return std::nullopt;
        std::tm tm{};
        tm.tm_year = *year - 1900;
        tm.tm_mon = *month - 1;
        tm.tm_mday = *day;
        tm.tm_hour = *hour;
        tm.tm_min = *minute;
        tm.tm_sec = *second;
        const auto base = timegm_portable(&tm);
        if (base == static_cast<std::time_t>(-1)) return std::nullopt;
        std::size_t cursor = 19;
        std::chrono::nanoseconds fractional{};
        if (cursor < raw.size() && raw[cursor] == '.') {
            ++cursor;
            std::int64_t digits{};
            int count{};
            while (cursor < raw.size() && raw[cursor] >= '0' && raw[cursor] <= '9') {
                if (count < 9) { digits = digits * 10 + (raw[cursor] - '0'); ++count; }
                ++cursor;
            }
            while (count++ < 9) digits *= 10;
            fractional = std::chrono::nanoseconds(digits);
        }
        int offset_seconds{};
        if (cursor < raw.size() && raw[cursor] != 'Z' && raw[cursor] != 'z') {
            if (raw[cursor] != '+' && raw[cursor] != '-') return std::nullopt;
            const bool positive = raw[cursor] == '+';
            ++cursor;
            if (cursor + 5 > raw.size()) return std::nullopt;
            const auto offset_hour = integer(cursor, 2);
            const auto offset_minute = integer(cursor + 3, 2);
            if (!offset_hour || !offset_minute || raw[cursor + 2] != ':') return std::nullopt;
            offset_seconds = (*offset_hour * 3600 + *offset_minute * 60) * (positive ? 1 : -1);
        }
        return SystemClock::from_time_t(base - offset_seconds) +
            std::chrono::duration_cast<SystemClock::duration>(fractional);
    } catch (...) { return std::nullopt; }
}

std::string format_local_time(SystemClock::time_point value) {
    if (value == SystemClock::time_point::min()) return "无";
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

std::string json_string(const JsonValue* value) {
    return value && value->is_string() ? value->string() : std::string{};
}

SystemClock::time_point event_time(const JsonValue& root) noexcept {
    if (const auto parsed = parse_timestamp(json_string(root.get("timestamp")))) return *parsed;
    return SystemClock::now();
}

std::filesystem::path day_directory(const std::filesystem::path& root, int days_ago) {
    auto raw = SystemClock::to_time_t(SystemClock::now() - std::chrono::hours(24 * days_ago));
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &raw);
#else
    localtime_r(&raw, &local);
#endif
    char year[5]{}, month[3]{}, day[3]{};
    std::strftime(year, sizeof(year), "%Y", &local);
    std::strftime(month, sizeof(month), "%m", &local);
    std::strftime(day, sizeof(day), "%d", &local);
    return root / year / month / day;
}

} // namespace

struct CodexSessionMonitor::Impl {
    struct Plan {
        int total{};
        int completed{};
        std::string current_step;
        std::vector<TaskStep> steps;
        friend bool operator==(const Plan&, const Plan&) = default;
    };

    struct TailState {
        std::filesystem::path path;
        std::uintmax_t position{};
        std::string line_buffer;
        SystemClock::time_point last_write{SystemClock::time_point::min()};
        SystemClock::time_point last_activity{SystemClock::time_point::min()};
        bool skip_current_line{};
        bool event_handled{};

        explicit TailState(std::filesystem::path value) : path(std::move(value)) {
            last_write = safe_last_write(path);
            last_activity = last_write == SystemClock::time_point::min()
                ? SystemClock::now() : last_write;
            line_buffer.reserve(kInitialLineBufferBytes);
        }

        void clear_line_buffer() {
            if (line_buffer.capacity() <= kRetainedLineBufferBytes) {
                line_buffer.clear();
                return;
            }
            std::string replacement;
            replacement.reserve(kInitialLineBufferBytes);
            line_buffer.swap(replacement);
        }

        void reset() {
            position = 0;
            clear_line_buffer();
            last_write = SystemClock::time_point::min();
            last_activity = SystemClock::now();
            skip_current_line = false;
            event_handled = false;
        }
    };

    struct ActiveTurn {
        std::string turn_id;
        std::filesystem::path source_path;
        std::string project_name;
        std::uint64_t start_sequence{};
        SystemClock::time_point started{};
        std::string title;
        bool start_event_emitted{};
        std::optional<Plan> plan;
        std::unordered_map<std::string, SystemClock::time_point> pending_tool_calls;
        SystemClock::time_point last_activity{SystemClock::time_point::min()};
    };

    explicit Impl(std::filesystem::path root)
        : sessions_root(root.empty() ? paths::default_sessions_root()
                                     : paths::normalize_sessions_root(path_to_utf8(root))) {
        discover_files(true);
    }

    std::filesystem::path sessions_root;
    std::unordered_map<std::filesystem::path, TailState, PathHash, PathEqual> files;
    std::unordered_map<std::filesystem::path, std::string, PathHash, PathEqual>
        project_names_by_file;
    std::unordered_map<std::filesystem::path, std::filesystem::file_time_type, PathHash, PathEqual>
        discovery_directory_writes;
    std::unordered_map<std::string, ActiveTurn> active_turns;
    std::unordered_map<std::filesystem::path, Plan, PathHash, PathEqual> plans_by_file;
    std::vector<MonitorEventKind> events;
    std::vector<std::string> event_contexts;
    std::vector<std::optional<TaskNotification>> event_notifications;
    std::vector<std::string> last_taken_event_contexts;
    std::vector<std::optional<TaskNotification>> last_taken_event_notifications;
    std::string last_completed_title;
    std::string last_completed_project_name;
    std::string last_aborted_title;
    std::string last_aborted_project_name;
    std::string last_interrupted_title;
    std::string last_interrupted_project_name;
    std::filesystem::path last_read_file;
    std::string last_event_type;
    std::filesystem::path last_event_file;
    std::string last_plan_update_turn_id;
    std::string last_error;
    std::uint64_t next_started_sequence{};
    int parse_error_count{};
    int read_error_count{};
    int stale_turn_cleanup_count{};
    SystemClock::time_point last_discovery{SystemClock::time_point::min()};
    SystemClock::time_point last_read{SystemClock::time_point::min()};
    SystemClock::time_point last_event{SystemClock::time_point::min()};
    SystemClock::time_point last_poll{SystemClock::time_point::min()};
    Clock::time_point next_discovery{Clock::time_point::min()};
    Clock::time_point next_full_discovery{Clock::time_point::min()};
    Clock::time_point next_stale_check{Clock::time_point::min()};

    std::vector<const ActiveTurn*> ordered_turns() const {
        std::vector<const ActiveTurn*> result;
        result.reserve(active_turns.size());
        for (const auto& [_, turn] : active_turns) result.push_back(&turn);
        std::sort(result.begin(), result.end(), [](const auto* left, const auto* right) {
            return left->start_sequence < right->start_sequence;
        });
        return result;
    }

    void emit(MonitorEventKind event, std::string context = {},
              std::optional<TaskNotification> notification = std::nullopt) {
        events.push_back(event);
        event_contexts.push_back(std::move(context));
        event_notifications.push_back(std::move(notification));
    }

    static std::string limited_text(std::string value, std::size_t maximum_bytes = 2400) {
        value = trim_ascii(value);
        if (value.size() <= maximum_bytes) return value;
        std::size_t end = maximum_bytes;
        while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U) --end;
        value.resize(end);
        value += "…";
        return value;
    }

    static bool ignored_user_task_text(std::string_view text) {
        const auto trimmed = trim_ascii(text);
        return trimmed.starts_with("<environment_context>") ||
               trimmed.starts_with("<turn_aborted>") ||
               trimmed.starts_with("# AGENTS.md instructions for") ||
               trimmed.starts_with("<permissions instructions>");
    }

    static std::string normalize_user_task_text(std::string text) {
        text = trim_ascii(text);
        if (ignored_user_task_text(text)) return {};
        while (text.starts_with("<image ")) {
            const auto end = text.find("</image>");
            if (end == std::string::npos) break;
            text.erase(0, end + std::string_view("</image>").size());
            text = trim_ascii(text);
        }
        return limited_text(std::move(text));
    }

    static std::string user_task_text(const JsonValue& payload) {
        const auto* content = payload.get("content");
        if (!content) return normalize_user_task_text(json_string(payload.get("message")));
        if (content->is_string()) return normalize_user_task_text(content->string());
        if (!content->is_array()) return {};
        std::string result;
        for (const auto& item : content->array()) {
            if (!item.is_object() || json_string(item.get("type")) != "input_text") continue;
            const auto part = trim_ascii(json_string(item.get("text")));
            if (part.empty()) continue;
            if (!result.empty()) result += '\n';
            result += part;
        }
        return normalize_user_task_text(std::move(result));
    }

    static std::string message_from_value(const JsonValue* value, int depth = 0) {
        if (!value || value->is_null() || depth > 4) return {};
        if (value->is_string()) {
            auto text = trim_ascii(value->string());
            if (text.empty()) return {};
            if ((text.front() == '{' && text.back() == '}') ||
                (text.front() == '[' && text.back() == ']')) {
                try {
                    const auto nested = parse_json(text);
                    if (auto extracted = message_from_value(&nested, depth + 1); !extracted.empty()) {
                        return extracted;
                    }
                } catch (...) {}
            }
            return limited_text(std::move(text));
        }
        if (value->is_object()) {
            for (const auto key : {"message", "error", "detail", "reason", "description"}) {
                if (auto extracted = message_from_value(value->get(key), depth + 1); !extracted.empty()) {
                    return extracted;
                }
            }
        }
        return {};
    }

    static std::string failure_reason(const JsonValue& payload) {
        if (auto reason = message_from_value(payload.get("error")); !reason.empty()) return reason;
        if (auto reason = message_from_value(payload.get("message")); !reason.empty()) return reason;
        if (auto reason = message_from_value(payload.get("reason")); !reason.empty()) return reason;
        const auto status = trim_ascii(json_string(payload.get("status")));
        return status.empty() ? std::string{} : "任务状态：" + status;
    }

    static std::string interruption_reason(const JsonValue& payload) {
        if (auto reason = message_from_value(payload.get("reason")); !reason.empty()) return reason;
        if (auto reason = message_from_value(payload.get("message")); !reason.empty()) return reason;
        return message_from_value(payload.get("error"));
    }

    static std::vector<TaskStep> terminal_steps(const std::optional<Plan>& plan,
                                                TaskNotificationState state) {
        if (!plan) return {};
        auto result = plan->steps;
        if (state != TaskNotificationState::Error && state != TaskNotificationState::Interrupted) {
            return result;
        }
        const auto terminal = state == TaskNotificationState::Error
            ? TaskStepState::Error : TaskStepState::Interrupted;
        for (auto& step : result) {
            if (step.state == TaskStepState::InProgress) {
                step.state = terminal;
                return result;
            }
        }
        return result;
    }

    static TaskNotification make_notification(TaskNotificationState state,
                                              const ActiveTurn& turn,
                                              std::string summary = {}) {
        TaskNotification result;
        result.state = state;
        result.project_name = turn.project_name;
        result.task_title = turn.title;
        result.steps = terminal_steps(turn.plan, state);
        result.summary = limited_text(std::move(summary));
        return result;
    }

    ActiveTurn* latest_turn_for_file(const std::filesystem::path& path) noexcept {
        ActiveTurn* result = nullptr;
        for (auto& [_, turn] : active_turns) {
            if (PathEqual{}(turn.source_path, path) &&
                (!result || turn.start_sequence > result->start_sequence)) result = &turn;
        }
        return result;
    }

    static std::string_view compact_json_string_view(std::string_view line,
                                                     std::string_view marker) noexcept {
        const auto start = line.find(marker);
        if (start == std::string_view::npos) return {};
        const auto value_start = start + marker.size();
        const auto value_end = line.find('"', value_start);
        if (value_end == std::string_view::npos) return {};
        return line.substr(value_start, value_end - value_start);
    }

    static std::string compact_json_string(std::string_view line, std::string_view marker) {
        return std::string(compact_json_string_view(line, marker));
    }

    static std::string project_name_from_location(std::string value) {
        while (!value.empty() && (value.back() == '/' || value.back() == '\\')) {
            value.pop_back();
        }
        const auto separator = value.find_last_of("/\\");
        auto name = value.substr(separator == std::string::npos ? 0 : separator + 1);
        if (name.size() > 4 && name.ends_with(".git")) name.resize(name.size() - 4);
        return name;
    }

    static std::string project_name_from_session_meta(std::string_view line) {
        const auto repository = compact_json_string(line, "\"repository_url\":\"");
        if (!repository.empty()) return project_name_from_location(repository);
        return project_name_from_location(compact_json_string(line, "\"cwd\":\""));
    }

    void remember_project_name(const std::filesystem::path& source_path,
                               std::string_view line) {
        const auto project_name = project_name_from_session_meta(line);
        if (project_name.empty()) return;
        project_names_by_file[source_path] = project_name;
        for (auto& [_, turn] : active_turns) {
            if (PathEqual{}(turn.source_path, source_path)) turn.project_name = project_name;
        }
    }

    void track_tool_call_lifecycle(std::string_view line,
                                   const std::filesystem::path& source_path) {
        if (line.find("\"type\":\"response_item\"") == std::string_view::npos) return;
        const auto payload_start = line.find("\"payload\":{");
        if (payload_start == std::string_view::npos) return;
        const auto payload = line.substr(payload_start);
        const auto payload_type = compact_json_string(payload, "\"type\":\"");
        const bool started = payload_type == "function_call";
        const bool finished = payload_type == "function_call_output";
        if (!started && !finished) return;
        const auto call_id = compact_json_string(payload, "\"call_id\":\"");
        if (call_id.empty()) return;
        if (finished) {
            for (auto& [_, turn] : active_turns) turn.pending_tool_calls.erase(call_id);
            return;
        }
        if (auto* turn = latest_turn_for_file(source_path);
            turn && (turn->pending_tool_calls.contains(call_id) ||
                     turn->pending_tool_calls.size() < kMaximumPendingToolCalls)) {
            const auto timestamp = compact_json_string_view(line, "\"timestamp\":\"");
            turn->pending_tool_calls.insert_or_assign(
                call_id, parse_timestamp(timestamp).value_or(SystemClock::now()));
        }
    }

    void remove_superseded_turns(const std::filesystem::path& source_path,
                                 std::string_view current_turn_id) {
        for (auto it = active_turns.begin(); it != active_turns.end();) {
            if (it->first != current_turn_id && PathEqual{}(it->second.source_path, source_path)) {
                it = active_turns.erase(it);
            } else {
                ++it;
            }
        }
    }

    void record_event(std::string event_type, const std::filesystem::path& source_path) {
        last_event_type = std::move(event_type);
        last_event_file = source_path;
        last_event = SystemClock::now();
    }

    void report_error(std::string_view operation, std::string_view error, bool parse = false) {
        if (parse) ++parse_error_count; else ++read_error_count;
        last_error = std::string(operation) + "：" + std::string(error);
    }

    void poll() {
        const auto now = Clock::now();
        if (now >= next_discovery) {
            discover_files(false);
            next_discovery = now + kFastDiscoveryInterval;
        }
        for (auto& [_, state] : files) read_new_bytes(state, false);
        if (now >= next_stale_check) {
            next_stale_check = now + kStaleTurnCheckInterval;
            clear_stale_turns(SystemClock::now());
        }
        last_poll = SystemClock::now();
    }

    struct Candidate {
        std::filesystem::path path;
        SystemClock::time_point write;
    };
    struct OldestFirst {
        bool operator()(const Candidate& a, const Candidate& b) const noexcept {
            return a.write > b.write;
        }
    };

    static void add_candidate(std::priority_queue<Candidate, std::vector<Candidate>, OldestFirst>& queue,
                              const std::filesystem::path& path) {
        Candidate candidate{path, safe_last_write(path)};
        if (queue.size() < kMaximumTrackedFiles) queue.push(std::move(candidate));
        else if (candidate.write > queue.top().write) {
            queue.pop();
            queue.push(std::move(candidate));
        }
    }

    template <typename Iterator>
    void enumerate_candidates(Iterator begin, Iterator end,
                              std::priority_queue<Candidate, std::vector<Candidate>, OldestFirst>& queue) {
        for (auto it = begin; it != end; ++it) {
            std::error_code error;
            if (!it->is_regular_file(error) || error || it->path().extension() != ".jsonl") continue;
            add_candidate(queue, it->path());
        }
    }

    void discover_files(bool initial) {
        last_discovery = SystemClock::now();
        std::error_code exists_error;
        if (!std::filesystem::is_directory(sessions_root, exists_error)) return;
        std::priority_queue<Candidate, std::vector<Candidate>, OldestFirst> candidates;
        const auto now = Clock::now();
        const bool full = initial || now >= next_full_discovery;
        if (full) {
            next_full_discovery = now + kFullDiscoveryInterval;
            std::error_code error;
            std::filesystem::recursive_directory_iterator begin(
                sessions_root, std::filesystem::directory_options::skip_permission_denied, error), end;
            if (!error) enumerate_candidates(begin, end, candidates);
            else report_error("完整扫描会话目录", error.message());
        } else {
            for (int days_ago = 0; days_ago <= 2; ++days_ago) {
                const auto folder = day_directory(sessions_root, days_ago);
                std::error_code write_error;
                const auto directory_write = std::filesystem::last_write_time(folder, write_error);
                const auto cached = discovery_directory_writes.find(folder);
                if (!write_error && cached != discovery_directory_writes.end() &&
                    cached->second == directory_write) {
                    continue;
                }
                if (write_error) discovery_directory_writes.erase(folder);

                std::error_code error;
                std::filesystem::directory_iterator begin(
                    folder, std::filesystem::directory_options::skip_permission_denied, error), end;
                if (!error) {
                    enumerate_candidates(begin, end, candidates);
                    if (!write_error) {
                        discovery_directory_writes.insert_or_assign(folder, directory_write);
                    }
                }
            }
        }

        std::vector<Candidate> ordered;
        ordered.reserve(candidates.size());
        while (!candidates.empty()) {
            ordered.push_back(candidates.top());
            candidates.pop();
        }
        std::sort(ordered.begin(), ordered.end(), [](const Candidate& a, const Candidate& b) {
            return a.write < b.write;
        });
        for (const auto& candidate : ordered) {
            if (files.find(candidate.path) != files.end()) continue;
            auto [it, inserted] = files.emplace(candidate.path, TailState(candidate.path));
            if (inserted) read_new_bytes(it->second, initial);
        }
        prune_tracked_files();
    }

    void prune_tracked_files() {
        if (files.size() <= kMaximumTrackedFiles) return;
        std::unordered_set<std::filesystem::path, PathHash, PathEqual> active_paths;
        for (const auto& [_, turn] : active_turns) active_paths.insert(turn.source_path);
        std::vector<TailState*> removable;
        for (auto& [path, state] : files) {
            if (active_paths.find(path) == active_paths.end()) removable.push_back(&state);
        }
        std::sort(removable.begin(), removable.end(), [](const TailState* left, const TailState* right) {
            return left->last_activity < right->last_activity;
        });
        for (const auto* state : removable) {
            if (files.size() <= kMaximumTrackedFiles) break;
            plans_by_file.erase(state->path);
            files.erase(state->path);
        }
    }

    void read_new_bytes(TailState& state, bool suppress_notifications) {
        try {
            std::error_code error;
            // Tracked entries were already validated during discovery. file_size() also
            // reports deletion/type errors, avoiding an extra stat call on every poll.
            const auto length = std::filesystem::file_size(state.path, error);
            if (error) return;
            const auto write = safe_last_write(state.path);
            if (length < state.position) {
                remove_turns_for_file(state.path);
                plans_by_file.erase(state.path);
                state.reset();
            }
            if (length == state.position && write <= state.last_write) return;
            if (length == state.position) {
                state.last_write = write;
                return;
            }
            std::ifstream stream(state.path, std::ios::binary);
            if (!stream) {
                report_error("读取会话文件", "无法打开 " + path_to_utf8(state.path));
                return;
            }
            stream.seekg(static_cast<std::streamoff>(state.position));
            std::vector<char> buffer(kReadBufferSize);
            while (stream) {
                stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto count = stream.gcount();
                if (count <= 0) break;
                consume_text(state, std::string_view(buffer.data(), static_cast<std::size_t>(count)),
                             suppress_notifications);
                state.position += static_cast<std::uintmax_t>(count);
            }
            const auto read_time = SystemClock::now();
            const auto activity = suppress_notifications && write != SystemClock::time_point::min()
                ? write : read_time;
            state.last_write = write;
            state.last_activity = activity;
            last_read_file = state.path;
            last_read = read_time;
            touch_turns_for_file(state.path, activity);
        } catch (const std::exception& exception) {
            report_error("读取会话文件", exception.what());
        }
    }

    void consume_text(TailState& state, std::string_view text, bool suppress_notifications) {
        std::size_t offset{};
        while (offset < text.size()) {
            const auto newline = text.find('\n', offset);
            const bool has_newline = newline != std::string_view::npos;
            const auto end = has_newline ? newline : text.size();
            const auto count = end - offset;
            if (!state.skip_current_line && count > 0) {
                const auto capacity = kMaximumBufferedLineBytes - state.line_buffer.size();
                if (capacity > 0) state.line_buffer.append(text.substr(offset, std::min(capacity, count)));
                if (!state.event_handled) {
                    if (try_process_event(state.line_buffer, state.path, suppress_notifications)) {
                        state.event_handled = true;
                        state.clear_line_buffer();
                        if (!has_newline) state.skip_current_line = true;
                    } else if (state.line_buffer.size() >= kMaximumBufferedLineBytes) {
                        state.skip_current_line = true;
                        state.clear_line_buffer();
                    }
                }
            }
            if (has_newline) {
                if (!state.event_handled && !state.skip_current_line && !state.line_buffer.empty()) {
                    try_process_event(state.line_buffer, state.path, suppress_notifications);
                }
                state.clear_line_buffer();
                state.skip_current_line = false;
                state.event_handled = false;
                offset = newline + 1;
            } else offset = text.size();
        }
    }

    bool try_process_event(std::string_view raw, const std::filesystem::path& source_path,
                           bool suppress_notifications) {
        const auto line = normalize_json_line(raw);
        if (line.empty()) return false;
        track_tool_call_lifecycle(line, source_path);
        if (line.find("\"type\":\"session_meta\"") != std::string_view::npos) {
            remember_project_name(source_path, line);
            return true;
        }
        if (try_process_user_response_item(line, source_path, suppress_notifications)) return true;
        if (try_process_plan_update(line, source_path, suppress_notifications)) return true;
        if (line.find("\"type\":\"event_msg\"") == std::string_view::npos) {
            if (!complete_json(line)) return false;
            // Most JSONL records are response items, and tool outputs can be very large.
            // Only their timestamp is needed here; building a full JSON DOM for every
            // historical output caused large temporary allocations during startup scans.
            if (const auto timestamp = parse_timestamp(
                    compact_json_string_view(line, "\"timestamp\":\""))) {
                touch_turns_for_file(source_path, *timestamp);
            }
            return true;
        }

        JsonValue root;
        try { root = parse_json(line); }
        catch (const std::exception& exception) {
            if (complete_json(line)) report_error("解析 event_msg", exception.what(), true);
            return false;
        }
        if (!root.is_object() || json_string(root.get("type")) != "event_msg") return false;
        const auto* payload = root.get("payload");
        if (!payload || !payload->is_object()) return true;
        auto event_type = json_string(payload->get("type"));
        if (event_type.empty()) return true;
        if (event_type == "turn_started") event_type = "task_started";
        else if (event_type == "turn_complete") event_type = "task_complete";
        const auto event_at = event_time(root);
        record_event(event_type, source_path);

        auto turn_id = json_string(payload->get("turn_id"));
        if (turn_id.empty()) turn_id = json_string(root.get("turn_id"));
        if (event_type == "user_message") {
            const auto title = normalize_user_task_text(json_string(payload->get("message")));
            if (!title.empty()) {
                set_title_for_turn(source_path, turn_id, title, event_at, suppress_notifications);
            }
            return true;
        }

        if (turn_id.empty()) {
            touch_turns_for_file(source_path, event_at);
            return true;
        }

        const auto before = active_turns.size();
        if (event_type == "task_started") {
            remove_superseded_turns(source_path, turn_id);
            auto it = active_turns.find(turn_id);
            bool added = false;
            if (it == active_turns.end()) {
                ActiveTurn turn;
                turn.turn_id = turn_id;
                turn.source_path = source_path;
                if (const auto project = project_names_by_file.find(source_path);
                    project != project_names_by_file.end()) {
                    turn.project_name = project->second;
                }
                turn.start_sequence = next_started_sequence++;
                turn.started = event_at;
                if (const auto plan = plans_by_file.find(source_path); plan != plans_by_file.end()) {
                    turn.plan = plan->second;
                }
                it = active_turns.emplace(turn_id, std::move(turn)).first;
                added = true;
            }
            touch_turn(it->second, event_at);
            if (added && suppress_notifications) it->second.start_event_emitted = true;
        } else if (event_type == "task_complete") {
            const auto it = active_turns.find(turn_id);
            if (it != active_turns.end()) {
                auto completed = std::move(it->second);
                active_turns.erase(it);
                if (!suppress_notifications) {
                    if (is_abnormal_completion(*payload)) {
                        last_aborted_title = completed.title.empty() ? "发生异常的任务" : completed.title;
                        last_aborted_project_name = completed.project_name;
                        emit(MonitorEventKind::TaskAborted, completed.project_name,
                             make_notification(TaskNotificationState::Error, completed,
                                               failure_reason(*payload)));
                    } else {
                        last_completed_title = completed.title.empty() ? "已完成的任务" : completed.title;
                        last_completed_project_name = completed.project_name;
                        emit(MonitorEventKind::TaskCompleted, completed.project_name,
                             make_notification(TaskNotificationState::Completed, completed,
                                               json_string(payload->get("last_agent_message"))));
                    }
                }
            } else if (!suppress_notifications && is_abnormal_completion(*payload)) {
                last_aborted_title = "发生异常的任务";
                last_aborted_project_name.clear();
                TaskNotification notification;
                notification.state = TaskNotificationState::Error;
                notification.task_title = last_aborted_title;
                notification.summary = limited_text(failure_reason(*payload));
                emit(MonitorEventKind::TaskAborted, {}, std::move(notification));
            }
        } else if (event_type == "turn_aborted") {
            const auto it = active_turns.find(turn_id);
            if (it != active_turns.end()) {
                auto aborted = std::move(it->second);
                active_turns.erase(it);
                if (!suppress_notifications) {
                    last_interrupted_title = aborted.title.empty() ? "未知任务" : aborted.title;
                    last_interrupted_project_name = aborted.project_name;
                    emit(MonitorEventKind::TaskInterrupted, aborted.project_name,
                         make_notification(TaskNotificationState::Interrupted, aborted,
                                           interruption_reason(*payload)));
                }
            } else if (!suppress_notifications) {
                last_interrupted_title = "未知任务";
                last_interrupted_project_name.clear();
                TaskNotification notification;
                notification.state = TaskNotificationState::Interrupted;
                notification.task_title = last_interrupted_title;
                notification.summary = limited_text(interruption_reason(*payload));
                emit(MonitorEventKind::TaskInterrupted, {}, std::move(notification));
            }
        } else if (is_failure_event_type(event_type)) {
            const auto it = active_turns.find(turn_id);
            if (it != active_turns.end()) {
                auto aborted = std::move(it->second);
                active_turns.erase(it);
                if (!suppress_notifications) {
                    last_aborted_title = aborted.title.empty() ? "发生异常的任务" : aborted.title;
                    last_aborted_project_name = aborted.project_name;
                    emit(MonitorEventKind::TaskAborted, aborted.project_name,
                         make_notification(TaskNotificationState::Error, aborted,
                                           failure_reason(*payload)));
                }
            }
        } else {
            if (auto it = active_turns.find(turn_id); it != active_turns.end()) touch_turn(it->second, event_at);
            else touch_turns_for_file(source_path, event_at);
        }
        if (before != active_turns.size()) emit(MonitorEventKind::StateChanged);
        return true;
    }

    bool try_process_user_response_item(std::string_view line,
                                        const std::filesystem::path& source_path,
                                        bool suppress_notifications) {
        if (line.find("\"type\":\"message\"") == std::string_view::npos ||
            line.find("\"role\":\"user\"") == std::string_view::npos) return false;
        try {
            const auto root = parse_json(line);
            if (json_string(root.get("type")) != "response_item") return false;
            const auto* payload = root.get("payload");
            if (!payload || !payload->is_object() ||
                json_string(payload->get("type")) != "message" ||
                json_string(payload->get("role")) != "user") return false;
            auto turn_id = json_string(payload->get("turn_id"));
            if (const auto* metadata = payload->get("internal_chat_message_metadata_passthrough")) {
                if (turn_id.empty()) turn_id = json_string(metadata->get("turn_id"));
            }
            const auto event_at = event_time(root);
            const auto title = user_task_text(*payload);
            if (!title.empty()) {
                set_title_for_turn(source_path, turn_id, title, event_at, suppress_notifications);
            } else {
                touch_turns_for_file(source_path, event_at);
            }
            return true;
        } catch (const std::exception& exception) {
            if (complete_json(line)) report_error("解析用户任务内容", exception.what(), true);
            return false;
        }
    }

    bool try_process_plan_update(std::string_view line, const std::filesystem::path& source_path,
                                 bool suppress_notifications) {
        if (line.find("update_plan") == std::string_view::npos ||
            line.find("function_call") == std::string_view::npos) return false;
        try {
            const auto root = parse_json(line);
            const auto* payload = root.get("payload");
            if (!payload || !payload->is_object() ||
                json_string(payload->get("type")) != "function_call" ||
                json_string(payload->get("name")) != "update_plan") return false;
            record_event("update_plan", source_path);
            const auto event_at = event_time(root);
            const auto arguments_text = json_string(payload->get("arguments"));
            if (arguments_text.empty()) return true;
            const auto arguments = parse_json(arguments_text);
            const auto* plan_value = arguments.get("plan");
            if (!plan_value || !plan_value->is_array()) return true;

            std::string turn_id;
            if (const auto* metadata = payload->get("internal_chat_message_metadata_passthrough")) {
                turn_id = json_string(metadata->get("turn_id"));
            }
            ActiveTurn* turn = nullptr;
            if (!turn_id.empty()) {
                if (auto it = active_turns.find(turn_id); it != active_turns.end()) turn = &it->second;
            }
            if (!turn) {
                for (auto& [_, candidate] : active_turns) {
                    if (PathEqual{}(candidate.source_path, source_path) &&
                        (!turn || candidate.start_sequence > turn->start_sequence)) turn = &candidate;
                }
            }
            if (!turn) return true;
            touch_turn(*turn, event_at);

            Plan next;
            next.total = static_cast<int>(plan_value->array().size());
            std::string first_pending;
            for (const auto& item : plan_value->array()) {
                if (!item.is_object()) continue;
                const auto status = json_string(item.get("status"));
                const auto step = json_string(item.get("step"));
                TaskStepState step_state = TaskStepState::Pending;
                if (status == "completed") {
                    ++next.completed;
                    step_state = TaskStepState::Completed;
                } else if (status == "in_progress") {
                    step_state = TaskStepState::InProgress;
                    if (next.current_step.empty()) next.current_step = step;
                } else if (status == "pending" && first_pending.empty()) {
                    first_pending = step;
                }
                if (!step.empty()) next.steps.push_back(TaskStep{step, step_state});
            }
            if (next.current_step.empty()) next.current_step = first_pending;
            plans_by_file[source_path] = next;
            const bool changed = !turn->plan || *turn->plan != next;
            turn->plan = std::move(next);
            if (changed && !suppress_notifications) {
                last_plan_update_turn_id = turn->turn_id;
                emit(MonitorEventKind::StateChanged);
                emit(MonitorEventKind::PlanUpdated);
            }
            return true;
        } catch (const std::exception& exception) {
            if (complete_json(line)) report_error("解析 update_plan", exception.what(), true);
            return false;
        }
    }

    static bool is_failure_event_type(std::string_view value) noexcept {
        return value == "task_failed" || value == "turn_failed" ||
               value == "stream_error" || value == "request_error" || value == "error";
    }

    static bool is_abnormal_completion(const JsonValue& payload) {
        if (const auto* error = payload.get("error"); error && !error->is_null()) {
            if (error->is_object()) {
                // Mirror Codex's ErrorEvent::affects_turn_status: an error
                // marks the turn failed unless codex_error_info is one of
                // these two non-fatal values.
                const auto info = lowercase_ascii(trim_ascii(json_string(error->get("codex_error_info"))));
                if (info != "thread_rollback_failed" && info != "active_turn_not_steerable") return true;
            } else if (!trim_ascii(error->string_or()).empty()) {
                return true;
            }
        }
        const auto status = lowercase_ascii(json_string(payload.get("status")));
        return status == "failed" || status == "error" || status == "aborted" || status == "cancelled";
    }

    void remove_turns_for_file(const std::filesystem::path& path) {
        bool changed = false;
        for (auto it = active_turns.begin(); it != active_turns.end();) {
            if (PathEqual{}(it->second.source_path, path)) { it = active_turns.erase(it); changed = true; }
            else ++it;
        }
        if (changed) emit(MonitorEventKind::StateChanged);
    }

    void set_title_for_turn(const std::filesystem::path& path, std::string_view turn_id,
                            const std::string& title, SystemClock::time_point activity,
                            bool suppress_notifications) {
        ActiveTurn* turn = nullptr;
        if (!turn_id.empty()) {
            if (auto it = active_turns.find(std::string(turn_id)); it != active_turns.end() &&
                PathEqual{}(it->second.source_path, path)) turn = &it->second;
        }
        if (!turn) {
            for (auto& [_, candidate] : active_turns) {
                if (PathEqual{}(candidate.source_path, path) &&
                    (!turn || candidate.start_sequence > turn->start_sequence)) turn = &candidate;
            }
        }
        if (!turn) return;
        touch_turn(*turn, activity);
        if (turn->title.empty()) {
            turn->title = title;
            if (!suppress_notifications) emit(MonitorEventKind::StateChanged);
        }
        if (turn->start_event_emitted || turn->title.empty()) return;
        turn->start_event_emitted = true;
        if (!suppress_notifications) {
            emit(MonitorEventKind::TaskStarted, turn->project_name,
                 make_notification(TaskNotificationState::Started, *turn));
        }
    }

    static void touch_turn(ActiveTurn& turn, SystemClock::time_point activity) noexcept {
        if (activity != SystemClock::time_point::min() &&
            (turn.last_activity == SystemClock::time_point::min() || activity > turn.last_activity)) {
            turn.last_activity = activity;
        }
    }

    void touch_turns_for_file(const std::filesystem::path& path, SystemClock::time_point activity) {
        for (auto& [_, turn] : active_turns) if (PathEqual{}(turn.source_path, path)) touch_turn(turn, activity);
    }

    void clear_stale_turns(SystemClock::time_point now) {
        std::vector<std::string> stale;
        const auto pending_cutoff = now - kMaximumPendingToolCallAge;
        for (auto& [id, turn] : active_turns) {
            for (auto it = turn.pending_tool_calls.begin(); it != turn.pending_tool_calls.end();) {
                if (it->second < pending_cutoff) it = turn.pending_tool_calls.erase(it);
                else ++it;
            }
            // An unfinished tool call is only a reason to keep a stale turn while
            // the session file is still held by a live writer. Interrupted sessions
            // can leave a function_call without its function_call_output; without
            // this liveness check they remain visible for the full six-hour pending
            // call retention window.
            const bool live_writer = session_file_may_have_live_writer(turn.source_path);
            if (!CodexSessionMonitor::is_turn_stale(
                    turn.last_activity, safe_last_write(turn.source_path), now,
                    kStaleTurnGraceSeconds, !turn.pending_tool_calls.empty() && live_writer)) continue;
            if (!live_writer) stale.push_back(id);
        }
        if (stale.empty()) return;
        for (const auto& id : stale) active_turns.erase(id);
        stale_turn_cleanup_count += static_cast<int>(stale.size());
        emit(MonitorEventKind::StateChanged);
    }

    MonitorSnapshot make_snapshot(bool include_diagnostics = true) const {
        MonitorSnapshot result;
        const auto turns = ordered_turns();
        result.active_count = static_cast<int>(turns.size());
        result.active_titles.reserve(turns.size());
        result.active_project_names.reserve(turns.size());
        result.active_plan_progress_labels.reserve(turns.size());
        result.active_plan_steps.reserve(turns.size());
        for (const auto* turn : turns) {
            result.active_titles.push_back(turn->title.empty() ? "正在处理任务…" : turn->title);
            result.active_project_names.push_back(turn->project_name);
            if (!turn->plan || turn->plan->total <= 1) result.active_plan_progress_labels.push_back(std::nullopt);
            else result.active_plan_progress_labels.push_back(
                std::to_string(turn->plan->completed) + "/" + std::to_string(turn->plan->total));
            result.active_plan_steps.push_back(turn->plan ? turn->plan->steps : std::vector<TaskStep>{});
            if (turn->plan) {
                result.total_plan_step_count += turn->plan->total;
                result.completed_plan_step_count += turn->plan->completed;
            }
        }
        result.last_completed_title = last_completed_title;
        result.last_completed_project_name = last_completed_project_name;
        result.last_aborted_title = last_aborted_title;
        result.last_aborted_project_name = last_aborted_project_name;
        result.last_interrupted_title = last_interrupted_title;
        result.last_interrupted_project_name = last_interrupted_project_name;
        result.last_event_type = last_event_type;
        result.latest_event_active_title_index = active_title_index(last_event_file);
        result.event_contexts = last_taken_event_contexts;
        result.event_notifications = last_taken_event_notifications;
        result.latest_plan_update_active_title_index = active_turn_index(last_plan_update_turn_id, turns);
        if (include_diagnostics) result.diagnostics_text = make_diagnostics();
        return result;
    }

    int active_turn_index(std::string_view turn_id, const std::vector<const ActiveTurn*>& turns) const noexcept {
        if (turn_id.empty()) return -1;
        for (std::size_t i = 0; i < turns.size(); ++i) {
            if (turns[i]->turn_id == turn_id) return static_cast<int>(i);
        }
        return -1;
    }

    int active_title_index(const std::filesystem::path& source_path) const {
        if (source_path.empty()) return -1;
        const auto turns = ordered_turns();
        for (std::size_t i = 0; i < turns.size(); ++i) {
            if (PathEqual{}(turns[i]->source_path, source_path)) return static_cast<int>(i);
        }
        return -1;
    }

    std::string make_diagnostics() const {
        std::ostringstream output;
        std::error_code error;
        output << "会话监听\n"
               << "  目录：" << path_to_utf8(sessions_root) << "\n"
               << "  目录存在：" << (std::filesystem::is_directory(sessions_root, error) ? "是" : "否") << "\n"
               << "  已跟踪文件：" << files.size() << "\n"
               << "  活跃任务：" << active_turns.size() << "\n"
               << "  最近轮询：" << format_local_time(last_poll) << "\n"
               << "  最近扫描：" << format_local_time(last_discovery) << "\n"
               << "  最近读取：" << format_local_time(last_read) << "\n";
        if (!last_read_file.empty()) output << "  最近读取文件：" << path_to_utf8(last_read_file) << "\n";
        output << "  最近事件：" << (last_event_type.empty() ? "无" : last_event_type + " · " + format_local_time(last_event)) << "\n";
        if (!last_event_file.empty()) output << "  最近事件文件：" << path_to_utf8(last_event_file) << "\n";
        output << "  JSON 解析错误：" << parse_error_count << "\n"
               << "  文件读取错误：" << read_error_count << "\n"
               << "  过期任务清理：" << stale_turn_cleanup_count << "\n";
        if (!last_error.empty()) output << "  最近错误：" << last_error << "\n";
        if (!active_turns.empty()) {
            output << "\n活跃任务明细\n";
            for (const auto* turn : ordered_turns()) {
                output << "  " << turn->turn_id << " | "
                       << (turn->title.empty() ? "未命名任务" : turn->title) << "\n"
                       << "    最近活动：" << format_local_time(turn->last_activity) << "\n"
                       << "    文件：" << path_to_utf8(turn->source_path) << "\n";
            }
        }
        return output.str();
    }
};

CodexSessionMonitor::CodexSessionMonitor(std::filesystem::path sessions_root)
    : impl_(std::make_unique<Impl>(std::move(sessions_root))) {}
CodexSessionMonitor::~CodexSessionMonitor() = default;
CodexSessionMonitor::CodexSessionMonitor(CodexSessionMonitor&&) noexcept = default;
CodexSessionMonitor& CodexSessionMonitor::operator=(CodexSessionMonitor&&) noexcept = default;
void CodexSessionMonitor::poll() { impl_->poll(); }
MonitorSnapshot CodexSessionMonitor::snapshot(bool include_diagnostics) const {
    return impl_->make_snapshot(include_diagnostics);
}
std::vector<MonitorEventKind> CodexSessionMonitor::take_events() {
    auto result = std::move(impl_->events);
    impl_->events.clear();
    impl_->last_taken_event_contexts = std::move(impl_->event_contexts);
    impl_->event_contexts.clear();
    impl_->last_taken_event_notifications = std::move(impl_->event_notifications);
    impl_->event_notifications.clear();
    return result;
}
int CodexSessionMonitor::active_count() const { return static_cast<int>(impl_->active_turns.size()); }
std::string CodexSessionMonitor::primary_active_title() const {
    for (const auto* turn : impl_->ordered_turns()) if (!turn->title.empty()) return turn->title;
    return {};
}
std::string CodexSessionMonitor::primary_current_plan_step() const {
    for (const auto* turn : impl_->ordered_turns()) if (turn->plan && turn->plan->total > 0) return turn->plan->current_step;
    return {};
}
std::string CodexSessionMonitor::last_completed_title() const { return impl_->last_completed_title; }
std::string CodexSessionMonitor::last_aborted_title() const { return impl_->last_aborted_title; }
std::string CodexSessionMonitor::last_interrupted_title() const { return impl_->last_interrupted_title; }
int CodexSessionMonitor::total_plan_step_count() const {
    return impl_->make_snapshot(false).total_plan_step_count;
}
int CodexSessionMonitor::completed_plan_step_count() const {
    return impl_->make_snapshot(false).completed_plan_step_count;
}
std::vector<std::optional<std::string>> CodexSessionMonitor::active_plan_progress_labels() const {
    return impl_->make_snapshot(false).active_plan_progress_labels;
}
std::string CodexSessionMonitor::diagnostics_text() const { return impl_->make_diagnostics(); }
void CodexSessionMonitor::report_unexpected_error(std::string_view operation, std::string_view error) {
    impl_->report_error(operation, error);
}

bool CodexSessionMonitor::is_turn_stale(SystemClock::time_point last_activity,
                                        SystemClock::time_point file_write,
                                        SystemClock::time_point now,
                                        int grace_seconds,
                                        bool has_pending_tool_call) noexcept {
    if (grace_seconds <= 0 || has_pending_tool_call) return false;
    const auto cutoff = now - std::chrono::seconds(grace_seconds);
    return last_activity < cutoff && file_write < cutoff;
}

struct MonitorWorker::Impl {
    std::filesystem::path root;
    Callback callback;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable condition;
    MonitorWorkerOptions options;
    bool running{};
    bool stopping{};

    Impl(std::filesystem::path value, Callback fn, MonitorWorkerOptions worker_options)
        : root(std::move(value)), callback(std::move(fn)), options(worker_options) {}

    void run() {
        CodexSessionMonitor monitor(root);
        (void)monitor.take_events();
        if (callback) {
            callback({MonitorEventKind::StateChanged}, monitor.snapshot(options.include_diagnostics));
        }
#ifdef __APPLE__
        // Initial recovery scans can temporarily parse large historical JSON records.
        // Return freed C++ allocator pages instead of keeping the startup high-water mark.
        if (auto* zone = malloc_default_zone()) {
            (void)malloc_zone_pressure_relief(zone, 0);
        }
#endif
        auto next_snapshot = Clock::now() + kDiagnosticsSnapshotInterval;
        std::unique_lock lock(mutex);
        while (!stopping) {
            condition.wait_for(lock, std::chrono::milliseconds(500), [&] { return stopping; });
            if (stopping) break;
            lock.unlock();
            try { monitor.poll(); }
            catch (const std::exception& exception) {
                monitor.report_unexpected_error("读取 Codex 会话", exception.what());
            }
            auto events = monitor.take_events();
            const auto now = Clock::now();
            const bool periodic_due = options.emit_periodic_snapshots && now >= next_snapshot;
            if ((!events.empty() || periodic_due) && callback) {
                callback(std::move(events), monitor.snapshot(options.include_diagnostics));
                if (options.emit_periodic_snapshots) {
                    next_snapshot = now + kDiagnosticsSnapshotInterval;
                }
            }
            lock.lock();
        }
    }
};

MonitorWorker::MonitorWorker(std::filesystem::path sessions_root, Callback callback,
                             MonitorWorkerOptions options)
    : impl_(std::make_unique<Impl>(std::move(sessions_root), std::move(callback), options)) {}
MonitorWorker::~MonitorWorker() { stop(); }
void MonitorWorker::start() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->running) return;
    impl_->stopping = false;
    impl_->running = true;
    impl_->thread = std::thread([this] { impl_->run(); });
}
void MonitorWorker::stop() noexcept {
    if (!impl_) return;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->running) return;
        impl_->stopping = true;
    }
    impl_->condition.notify_all();
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->running = false;
}

} // namespace codexpets

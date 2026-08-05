#pragma once

#include "types.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace codexpets {

inline constexpr std::size_t monitor_pending_event_limit = 64;

class CodexSessionMonitor {
public:
    explicit CodexSessionMonitor(std::filesystem::path sessions_root = {});
    ~CodexSessionMonitor();
    CodexSessionMonitor(CodexSessionMonitor&&) noexcept;
    CodexSessionMonitor& operator=(CodexSessionMonitor&&) noexcept;
    CodexSessionMonitor(const CodexSessionMonitor&) = delete;
    CodexSessionMonitor& operator=(const CodexSessionMonitor&) = delete;

    void poll();
    [[nodiscard]] MonitorSnapshot snapshot(bool include_diagnostics = true) const;
    [[nodiscard]] std::vector<MonitorEventKind> take_events();
    [[nodiscard]] int active_count() const;
    [[nodiscard]] std::string primary_active_title() const;
    [[nodiscard]] std::string primary_current_plan_step() const;
    [[nodiscard]] std::string last_completed_title() const;
    [[nodiscard]] std::string last_aborted_title() const;
    [[nodiscard]] std::string last_interrupted_title() const;
    [[nodiscard]] int total_plan_step_count() const;
    [[nodiscard]] int completed_plan_step_count() const;
    [[nodiscard]] std::vector<std::optional<std::string>> active_plan_progress_labels() const;
    [[nodiscard]] std::string diagnostics_text() const;
    void report_unexpected_error(std::string_view operation, std::string_view error);

    static bool is_turn_stale(SystemClock::time_point last_activity,
                              SystemClock::time_point file_write,
                              SystemClock::time_point now,
                              int grace_seconds,
                              bool has_pending_tool_call = false) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct MonitorWorkerOptions {
    bool emit_periodic_snapshots{true};
    bool include_diagnostics{true};
};

class MonitorWorker {
public:
    using Callback = std::function<void(std::vector<MonitorEventKind>, MonitorSnapshot)>;

    MonitorWorker(std::filesystem::path sessions_root, Callback callback,
                  MonitorWorkerOptions options = {});
    ~MonitorWorker();
    MonitorWorker(const MonitorWorker&) = delete;
    MonitorWorker& operator=(const MonitorWorker&) = delete;

    void start();
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace codexpets

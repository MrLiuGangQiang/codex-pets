#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace codexpets {

enum class ReminderState : std::uint8_t { Idle, Busy, Completed, Error, Interrupted };
enum class DockEdge : std::uint8_t { None, Left, Right };
enum class MonitorEventKind : std::uint8_t {
    StateChanged, TaskStarted, TaskCompleted, TaskAborted, TaskInterrupted, PlanUpdated
};

struct PointD {
    double x{};
    double y{};
};

struct RectD {
    double x{};
    double y{};
    double width{};
    double height{};

    [[nodiscard]] double left() const noexcept { return x; }
    [[nodiscard]] double top() const noexcept { return y; }
    [[nodiscard]] double right() const noexcept { return x + width; }
    [[nodiscard]] double bottom() const noexcept { return y + height; }
    [[nodiscard]] double center_x() const noexcept { return x + width / 2.0; }
    [[nodiscard]] bool contains(PointD point) const noexcept {
        return point.x >= left() && point.x <= right() && point.y >= top() && point.y <= bottom();
    }
};

struct PetPositionState {
    DockEdge dock_edge{DockEdge::None};
    std::string screen_identifier;
    double relative_x{};
    double relative_y{};

    void normalize() noexcept {
        relative_x = std::clamp(relative_x, 0.0, 1.0);
        relative_y = std::clamp(relative_y, 0.0, 1.0);
    }

    friend bool operator==(const PetPositionState&, const PetPositionState&) = default;
};

struct MonitorSnapshot {
    int active_count{};
    std::vector<std::string> active_titles;
    std::vector<std::optional<std::string>> active_plan_progress_labels;
    int total_plan_step_count{};
    int completed_plan_step_count{};
    std::string last_completed_title;
    std::string last_aborted_title;
    std::string last_interrupted_title;
    std::string last_event_type;
    int latest_event_active_title_index{-1};
    std::string diagnostics_text;
    int latest_plan_update_active_title_index{-1};
};

using Clock = std::chrono::steady_clock;
using SystemClock = std::chrono::system_clock;

} // namespace codexpets

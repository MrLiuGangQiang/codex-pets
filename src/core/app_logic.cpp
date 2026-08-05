#include "app_logic.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace codexpets::app_logic {
namespace {
constexpr std::string_view kBusyHeaderSessionSeparator = " • ";

bool blank(std::string_view value) {
    return value.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}
} // namespace

std::string format_abnormal_task_text(std::string_view title) {
    const auto normalized = blank(title) ? std::string("未知任务") : trim(title);
    return "任务失败：" + normalized;
}

std::string format_interrupted_task_text(std::string_view title) {
    const auto normalized = blank(title) ? std::string("未知任务") : trim(title);
    return "任务已中断：" + normalized;
}

int select_preferred_task_index(bool focus_latest_task, int latest_task_index) noexcept {
    return focus_latest_task ? latest_task_index : -1;
}

int reconcile_task_selection(ReminderState state, const std::vector<std::string>& titles,
                             int previous_index, std::string_view previously_selected_title,
                             bool select_newest_task, int preferred_task_index) noexcept {
    if (titles.empty() || state != ReminderState::Busy) return 0;
    if (preferred_task_index >= 0 && preferred_task_index < static_cast<int>(titles.size())) {
        return preferred_task_index;
    }
    if (select_newest_task) return static_cast<int>(titles.size()) - 1;
    if (!previously_selected_title.empty()) {
        for (std::size_t index = 0; index < titles.size(); ++index) {
            if (titles[index] == previously_selected_title) return static_cast<int>(index);
        }
    }
    return std::clamp(previous_index, 0, static_cast<int>(titles.size()) - 1);
}

ReminderState select_visual_state(int active_count, bool abnormal_recently,
                                  bool completed_recently,
                                  ReminderState latest_changed_state) noexcept {
    if (latest_changed_state == ReminderState::Error && abnormal_recently) {
        return ReminderState::Error;
    }
    if (latest_changed_state == ReminderState::Completed && completed_recently) {
        return ReminderState::Completed;
    }
    if (latest_changed_state == ReminderState::Busy && active_count > 0) {
        return ReminderState::Busy;
    }
    return active_count > 0 ? ReminderState::Busy : ReminderState::Idle;
}

int cloud_notification_seconds(ReminderState state, int configured_seconds) noexcept {
    const auto safe = std::max(1, configured_seconds);
    return state == ReminderState::Error ? std::max(10, safe) : safe;
}

bool should_show_thought_bubble(bool is_docked, ReminderState state,
                                Clock::time_point now,
                                Clock::time_point dock_thought_until) noexcept {
    if (!is_docked) return true;
    const bool has_task_state = state == ReminderState::Busy ||
                                state == ReminderState::Completed ||
                                state == ReminderState::Error ||
                                state == ReminderState::Interrupted;
    return has_task_state && now < dock_thought_until;
}

bool should_show_dock(Clock::time_point last_content_change, Clock::time_point now,
                      bool is_dragging, bool is_hovering,
                      Clock::time_point hover_reveal_until,
                      int idle_hide_seconds) noexcept {
    return is_dragging || is_hovering || now < hover_reveal_until || idle_hide_seconds <= 0 ||
           now - last_content_change < std::chrono::seconds(idle_hide_seconds);
}

DockEdge select_snap_edge(PointD cursor, RectD work_area, double snap_distance) noexcept {
    if (std::abs(cursor.x - work_area.left()) <= snap_distance) return DockEdge::Left;
    if (std::abs(cursor.x - work_area.right()) <= snap_distance) return DockEdge::Right;
    return DockEdge::None;
}

bool should_mirror_floating_sprite(PointD anchor, RectD work_area) noexcept {
    return anchor.x < work_area.center_x();
}

RectD dock_hover_bounds(DockEdge edge, RectD work_area, double dock_y, double scale,
                        bool fully_hidden, int hover_height) noexcept {
    const auto width = fully_hidden ? std::max(18.0, 28.0 * scale)
                                    : std::max(40.0, 56.0 * scale);
    const auto normalized_height = std::clamp(hover_height, 40, 1000);
    const auto half_height = std::max(20.0, normalized_height * scale / 2.0);
    const auto x = edge == DockEdge::Left ? work_area.left() : work_area.right() - width;
    const auto top = std::max(work_area.top(), dock_y - half_height);
    const auto bottom = std::min(work_area.bottom(), dock_y + half_height);
    return {x, top, width, std::max(1.0, bottom - top)};
}

bool segment_intersects_rect(PointD from, PointD to, const RectD& rect) noexcept {
    double enter = 0.0;
    double exit = 1.0;
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    const std::array<double, 4> p{-dx, dx, -dy, dy};
    const std::array<double, 4> q{from.x - rect.left(), rect.right() - from.x,
                                  from.y - rect.top(), rect.bottom() - from.y};
    for (std::size_t index = 0; index < p.size(); ++index) {
        if (std::abs(p[index]) < 1e-12) {
            if (q[index] < 0.0) return false;
            continue;
        }
        const double ratio = q[index] / p[index];
        if (p[index] < 0.0) {
            if (ratio > exit) return false;
            enter = std::max(enter, ratio);
        } else {
            if (ratio < enter) return false;
            exit = std::min(exit, ratio);
        }
    }
    return true;
}

bool is_task_switch_point(bool is_docked, bool bubble_visible, ReminderState state,
                          int task_count, RectD bubble_bounds, RectD content_bounds,
                          PointD point) noexcept {
    if (!bubble_visible || task_count <= 1 || state == ReminderState::Idle) return false;
    return (is_docked ? bubble_bounds : content_bounds).contains(point);
}

std::string format_busy_header(std::optional<std::string_view> step_progress,
                               int session_index, int session_count) {
    std::string header = !step_progress || blank(*step_progress)
        ? "进行中"
        : "进行中(" + std::string(*step_progress) + ")";
    if (session_count <= 1) return header;
    const auto safe_index = std::clamp(session_index, 0, session_count - 1);
    header += kBusyHeaderSessionSeparator;
    header += std::to_string(safe_index + 1);
    header += "/";
    header += std::to_string(session_count);
    return header;
}

int select_dock_sprite_index(DockEdge edge, ReminderState state, int frame) noexcept {
    const auto phase = std::abs(frame) % 20;
    const bool blink = phase == 11 || phase == 14;
    int base = 0;
    switch (state) {
        case ReminderState::Busy: base = 2; break;
        case ReminderState::Completed: base = 4; break;
        case ReminderState::Error: base = 6; break;
        case ReminderState::Interrupted: base = 8; break;
        case ReminderState::Idle:
        default: base = 0; break;
    }
    return (edge == DockEdge::Right ? 10 : 0) + base + (blink ? 1 : 0);
}

int select_floating_sprite_row(ReminderState state) noexcept {
    switch (state) {
        case ReminderState::Completed: return 1;
        case ReminderState::Busy: return 2;
        case ReminderState::Error: return 3;
        case ReminderState::Interrupted: return 4;
        default: return 0;
    }
}

int select_floating_frame(ReminderState state, int animation_tick) noexcept {
    if (state == ReminderState::Busy) {
        const auto phase = std::abs(animation_tick) % 64;
        if (phase < 40) return (phase / 2) % 4;
        if (phase < 44) return 4;
        if (phase < 60) return 5 + ((phase - 44) / 4) % 2;
        return 7;
    }
    if (state == ReminderState::Interrupted) {
        const auto phase = std::abs(animation_tick) % 20;
        return phase == 11 || phase == 14 ? 1 : 0;
    }
    return state == ReminderState::Completed
        ? std::abs(animation_tick / 3) % 4
        : std::abs(animation_tick / 3) % 8;
}

} // namespace codexpets::app_logic

#pragma once

#include "types.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace codexpets::app_logic {

std::string format_abnormal_task_text(std::string_view title);
std::string format_interrupted_task_text(std::string_view title);
std::string format_interrupted_task_text(std::string_view title);
int select_preferred_task_index(bool focus_latest_task, int latest_task_index) noexcept;
int reconcile_task_selection(ReminderState state, const std::vector<std::string>& titles,
                             int previous_index, std::string_view previously_selected_title,
                             bool select_newest_task, int preferred_task_index) noexcept;
ReminderState select_visual_state(int active_count, bool abnormal_recently,
                                  bool completed_recently,
                                  ReminderState latest_changed_state) noexcept;
int cloud_notification_seconds(ReminderState state, int configured_seconds) noexcept;
bool should_show_thought_bubble(bool is_docked, ReminderState state,
                                Clock::time_point now,
                                Clock::time_point dock_thought_until) noexcept;
bool should_show_dock(Clock::time_point last_content_change, Clock::time_point now,
                      bool is_dragging, bool is_hovering,
                      Clock::time_point hover_reveal_until,
                      int idle_hide_seconds) noexcept;
DockEdge select_snap_edge(PointD cursor, RectD work_area, double snap_distance) noexcept;
bool should_mirror_floating_sprite(PointD anchor, RectD work_area) noexcept;
RectD dock_hover_bounds(DockEdge edge, RectD work_area, double dock_y, double scale,
                        bool fully_hidden, int hover_height) noexcept;
bool is_task_switch_point(bool is_docked, bool bubble_visible, ReminderState state,
                          int task_count, RectD bubble_bounds, RectD content_bounds,
                          PointD point) noexcept;
std::string format_busy_header(std::optional<std::string_view> step_progress,
                               int session_index, int session_count);
int select_dock_sprite_index(DockEdge edge, ReminderState state, int frame) noexcept;
int select_floating_sprite_row(ReminderState state) noexcept;
int select_floating_frame(ReminderState state, int animation_tick) noexcept;

} // namespace codexpets::app_logic

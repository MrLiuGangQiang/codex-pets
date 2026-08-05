#pragma once

#include "types.h"

namespace codexpets::render_layout {

inline constexpr double logical_width = 420.0;
inline constexpr double logical_height = 260.0;
inline constexpr double bubble_width = 270.0;
inline constexpr double bubble_height = 110.0;
inline constexpr double bubble_inset = 45.0;
inline constexpr double dock_bubble_offset = 48.0;
inline constexpr double dock_bubble_switch_margin = 150.0;
inline constexpr double dock_pet_size = 104.0;
inline constexpr double floating_pet_width = 130.0;
inline constexpr double floating_pet_height = 140.0;
inline constexpr int cloud_bitmap_width = 540;
inline constexpr int cloud_bitmap_height = 220;

struct State {
    ReminderState reminder_state{ReminderState::Idle};
    DockEdge dock_edge{DockEdge::None};
    bool docked{};
    bool bubble_below{};
    bool mirror{};
    double dock_visibility{1.0};
    int animation_tick{};
};

struct ThoughtDots {
    RectD large;
    RectD secondary;
};

[[nodiscard]] RectD bubble_bounds(const State& state) noexcept;
[[nodiscard]] RectD visible_cloud_bounds(const State& state) noexcept;
[[nodiscard]] RectD floating_pet_bounds(const State& state) noexcept;
[[nodiscard]] RectD dock_pet_bounds(const State& state) noexcept;
[[nodiscard]] RectD pet_interaction_bounds(const State& state) noexcept;
[[nodiscard]] RectD visible_pet_bounds(const State& state) noexcept;
[[nodiscard]] ThoughtDots thought_dot_bounds(const State& state) noexcept;
[[nodiscard]] PointD bulb_origin(const State& state) noexcept;
[[nodiscard]] RectD header_bounds(const State& state) noexcept;
[[nodiscard]] RectD body_bounds(const State& state) noexcept;
[[nodiscard]] double dock_pet_center_y(const State& state) noexcept;

} // namespace codexpets::render_layout

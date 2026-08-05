#include "render_layout.h"

#include "app_logic.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace codexpets::render_layout {
namespace {

struct SpriteMetric {
    double anchor_x;
    double bottom;
    double x;
    double y;
    double width;
    double height;
};

constexpr std::array<std::array<SpriteMetric, 8>, 5> kFloatingMetrics{{
    {{{88.268827,157,43,30,113,127},{88.268827,157,43,30,113,127},
      {88.479689,157,43,30,115,127},{88.335350,153,42,30,113,123},
      {87.956078,158,42,30,114,128},{88.335350,153,42,30,113,123},
      {88.479689,157,43,30,115,127},{88.268827,157,43,30,113,127}}},
    {{{87.675970,158,42,30,114,128},{88.439236,158,43,30,115,128},
      {88.264264,156,42,30,113,126},{87.714413,159,43,30,114,129},
      {96,208,0,0,192,208},{96,208,0,0,192,208},
      {96,208,0,0,192,208},{96,208,0,0,192,208}}},
    {{{87.853720,130,41,35,128,95},{87.853720,130,41,35,128,95},
      {87.853720,130,41,35,128,95},{87.853720,130,41,35,128,95},
      {87.564935,127,39,28,134,99},{87.748331,160,44,28,113,132},
      {87.691131,160,44,28,113,132},{88.078308,129,35,28,128,101}}},
    {{{87.944566,145,39,31,120,114},{87.944566,145,39,31,120,114},
      {87.923949,144,39,31,118,113},{87.540754,143,39,30,116,113},
      {88.265314,145,40,31,117,114},{87.540754,143,39,30,116,113},
      {87.923949,144,39,31,118,113},{87.944566,145,39,31,120,114}}},
    {{{88.268827,157,43,30,113,127},{88.268827,157,43,30,113,127},
      {88.479689,157,43,30,115,127},{88.335350,153,42,30,113,123},
      {87.956078,158,42,30,114,128},{88.335350,153,42,30,113,123},
      {88.479689,157,43,30,115,127},{88.268827,157,43,30,113,127}}},
}};

struct SimpleRect { double x; double y; double width; double height; };
constexpr std::array<SimpleRect, 20> kDockOpaque{{
    {0,12,167,232},{0,12,167,232},{0,12,167,232},{0,12,167,232},{0,12,167,232},
    {0,12,167,232},{0,12,167,232},{0,12,167,232},{0,12,167,232},{0,12,167,232},
    {89,12,167,232},{89,12,167,232},{89,12,167,232},{89,12,167,232},{89,12,167,232},
    {89,12,167,232},{89,12,167,232},{89,12,167,232},{89,12,167,232},{89,12,167,232}
}};

double smooth_step(double value) noexcept {
    value = std::clamp(value, 0.0, 1.0);
    return value * value * (3.0 - 2.0 * value);
}

const SpriteMetric& floating_metric(const State& state) noexcept {
    const int row = app_logic::select_floating_sprite_row(state.reminder_state);
    const int frame = app_logic::select_floating_frame(state.reminder_state, state.animation_tick);
    return kFloatingMetrics[static_cast<std::size_t>(std::clamp(row, 0, 4))]
                           [static_cast<std::size_t>(std::clamp(frame, 0, 7))];
}

} // namespace

RectD bubble_bounds(const State& state) noexcept {
    double x = (logical_width - bubble_width) / 2.0;
    if (state.docked) x += state.dock_edge == DockEdge::Left ? -dock_bubble_offset : dock_bubble_offset;
    const double y = state.bubble_below
        ? logical_height - bubble_height - bubble_inset - (state.docked ? 2.0 : 0.0)
        : bubble_inset + (state.docked ? 6.0 : 0.0);
    return {x, y, bubble_width, bubble_height};
}

RectD visible_cloud_bounds(const State& state) noexcept {
    const auto cloud = bubble_bounds(state);
    return {cloud.x + 52.0 / 640.0 * cloud.width,
            cloud.y + 4.0 / 221.0 * cloud.height,
            536.0 / 640.0 * cloud.width,
            193.0 / 221.0 * cloud.height};
}

RectD floating_pet_bounds(const State& state) noexcept {
    const auto& metric = floating_metric(state);
    const double anchor = state.mirror ? 192.0 - metric.anchor_x : metric.anchor_x;
    return {logical_width / 2.0 - anchor * floating_pet_width / 192.0,
            logical_height - 3.0 - metric.bottom * floating_pet_height / 208.0,
            floating_pet_width, floating_pet_height};
}

RectD dock_pet_bounds(const State& state) noexcept {
    const double hidden = dock_pet_size * (1.0 - smooth_step(state.dock_visibility));
    const double x = state.dock_edge == DockEdge::Left ? -hidden : logical_width - dock_pet_size + hidden;
    const double y = state.bubble_below ? 4.0 : logical_height - dock_pet_size - 7.0;
    return {x, y, dock_pet_size, dock_pet_size};
}

RectD pet_interaction_bounds(const State& state) noexcept {
    return state.docked ? dock_pet_bounds(state) : floating_pet_bounds(state);
}

RectD visible_pet_bounds(const State& state) noexcept {
    if (state.docked) {
        const int frame = app_logic::select_dock_sprite_index(state.dock_edge, state.reminder_state,
                                                               state.animation_tick);
        const auto& opaque = kDockOpaque[static_cast<std::size_t>(std::clamp(frame, 0, 19))];
        const auto destination = dock_pet_bounds(state);
        constexpr double scale = dock_pet_size / 256.0;
        return {destination.x + opaque.x * scale, destination.y + opaque.y * scale,
                opaque.width * scale, opaque.height * scale};
    }

    const auto& metric = floating_metric(state);
    const auto destination = floating_pet_bounds(state);
    constexpr double scale_x = floating_pet_width / 192.0;
    constexpr double scale_y = floating_pet_height / 208.0;
    const double opaque_x = state.mirror ? 192.0 - (metric.x + metric.width) : metric.x;
    return {destination.x + opaque_x * scale_x, destination.y + metric.y * scale_y,
            metric.width * scale_x, metric.height * scale_y};
}

ThoughtDots thought_dot_bounds(const State& state) noexcept {
    constexpr double large_width = 17.0, large_height = 15.0;
    constexpr double small_width = 11.0, small_height = 10.0;
    const auto cloud = bubble_bounds(state);
    const auto visible_cloud = visible_cloud_bounds(state);
    const auto pet = visible_pet_bounds(state);

    if (state.docked) {
        const double inset = std::min(38.0, visible_cloud.width * 0.18);
        const double cloud_x = state.dock_edge == DockEdge::Left
            ? visible_cloud.x + inset : visible_cloud.right() - inset;
        const double inward = state.dock_edge == DockEdge::Left ? 1.0 : -1.0;
        const PointD cloud_anchor{cloud_x, state.bubble_below ? visible_cloud.y - 1.0
                                                               : visible_cloud.bottom() + 1.0};
        const PointD pet_anchor{pet.center_x() + inward * 2.0,
                                state.bubble_below ? pet.bottom() + 1.0 : pet.y - 1.0};
        const double dx = pet_anchor.x - cloud_anchor.x;
        const double dy = pet_anchor.y - cloud_anchor.y;
        const double distance = std::max(0.001, std::sqrt(dx * dx + dy * dy));
        const double ux = dx / distance;
        const double uy = dy / distance;
        const double large_radius = (std::abs(ux) * large_width + std::abs(uy) * large_height) / 2.0;
        const double small_radius = (std::abs(ux) * small_width + std::abs(uy) * small_height) / 2.0;
        const double gap = std::max(1.0, (distance - 2.0 * large_radius - 2.0 * small_radius) / 3.0);
        double large_distance = gap + large_radius;
        double small_distance = large_distance + large_radius + gap + small_radius;
        if (small_distance + small_radius > distance) {
            large_distance = distance / 3.0;
            small_distance = distance * 2.0 / 3.0;
        }
        const double perpendicular = state.dock_edge == DockEdge::Left
            ? (state.bubble_below ? 8.0 : -8.0) : (state.bubble_below ? -8.0 : 8.0);
        const PointD large_center{cloud_anchor.x + ux * large_distance,
                                  cloud_anchor.y + uy * large_distance};
        const PointD small_center{cloud_anchor.x + ux * small_distance - uy * perpendicular,
                                  cloud_anchor.y + uy * small_distance + ux * perpendicular};
        return {{large_center.x - large_width / 2.0, large_center.y - large_height / 2.0,
                 large_width, large_height},
                {small_center.x - small_width / 2.0, small_center.y - small_height / 2.0,
                 small_width, small_height}};
    }

    const double direction = state.mirror ? 1.0 : -1.0;
    const double desired = pet.y - small_height - 0.5;
    const double large_y = std::clamp(desired - large_height - 3.0,
                                      visible_cloud.bottom() + 3.0, visible_cloud.bottom() + 12.0);
    const double small_y = std::max(desired, large_y + large_height + 1.0);
    return {{cloud.center_x() + direction * 7.0 - large_width / 2.0, large_y,
             large_width, large_height},
            {cloud.center_x() + direction * 11.0 - small_width / 2.0, small_y,
             small_width, small_height}};
}

PointD bulb_origin(const State& state) noexcept {
    const auto visible_cloud = visible_cloud_bounds(state);
    return {std::round(visible_cloud.x + visible_cloud.width * 0.10),
            std::round(visible_cloud.y + visible_cloud.height * 0.32)};
}

RectD header_bounds(const State& state) noexcept {
    const auto cloud = bubble_bounds(state);
    return {cloud.x + cloud.width * 0.26, cloud.y + cloud.height * 0.10,
            cloud.width * 0.52, cloud.height * 0.20};
}

RectD body_bounds(const State& state) noexcept {
    const auto cloud = bubble_bounds(state);
    return {cloud.x + cloud.width * 0.30, cloud.y + cloud.height * 0.34, 156.0, 45.0};
}

double dock_pet_center_y(const State& state) noexcept {
    const auto bounds = visible_pet_bounds(state);
    return bounds.y + bounds.height / 2.0;
}

} // namespace codexpets::render_layout

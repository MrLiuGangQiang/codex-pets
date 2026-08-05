#pragma once

#include "settings.h"
#include "types.h"
#include "visual_state.h"
#include "xiaomi_speaker.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace codexpets {

enum class SoundCue : std::uint8_t { Started, Completed, Error };

struct MonitorEventEffect {
    MonitorEventKind event{MonitorEventKind::StateChanged};
    bool reveal_pet{};
    std::optional<SoundCue> sound;
    std::optional<XiaoAiEvent> xiaoai_event;
    std::string xiaoai_context;
};

// Applies the platform-independent event policy and returns side effects that
// the native shells execute (window reveal, sound, and optional speaker TTS).
[[nodiscard]] std::vector<MonitorEventEffect> apply_monitor_event_policy(
    VisualStateCoordinator& visual_coordinator,
    const MonitorSnapshot& snapshot,
    const std::vector<MonitorEventKind>& events,
    const AppSettings& settings,
    Clock::time_point now = Clock::now());

} // namespace codexpets

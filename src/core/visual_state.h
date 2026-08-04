#pragma once

#include "types.h"

#include <chrono>

namespace codexpets {

class VisualStateCoordinator {
public:
    [[nodiscard]] ReminderState latest_changed_state() const noexcept { return latest_changed_state_; }
    [[nodiscard]] bool show_newest_task_on_next_refresh() const noexcept { return show_newest_task_; }

    void record_started(int preferred_task_index = -1) noexcept;
    void record_completed(Clock::time_point now, std::chrono::seconds visible_for) noexcept;
    void record_aborted(Clock::time_point now, std::chrono::seconds visible_for) noexcept;
    void record_interrupted(Clock::time_point now, std::chrono::seconds visible_for) noexcept;
    [[nodiscard]] ReminderState select(int active_count, Clock::time_point now) const noexcept;
    [[nodiscard]] int preferred_task_index() const noexcept { return preferred_task_index_; }
    void consume_newest_task_focus() noexcept { show_newest_task_ = false; }

private:
    Clock::time_point completed_until_{Clock::time_point::min()};
    Clock::time_point abnormal_until_{Clock::time_point::min()};
    Clock::time_point interrupted_until_{Clock::time_point::min()};
    ReminderState latest_changed_state_{ReminderState::Idle};
    bool show_newest_task_{};
    int preferred_task_index_{-1};
};

} // namespace codexpets

#include "visual_state.h"
#include "app_logic.h"

namespace codexpets {

void VisualStateCoordinator::record_started(int preferred_task_index) noexcept {
    latest_changed_state_ = ReminderState::Busy;
    show_newest_task_ = true;
    preferred_task_index_ = preferred_task_index;
    completed_until_ = Clock::time_point::min();
    abnormal_until_ = Clock::time_point::min();
    interrupted_until_ = Clock::time_point::min();
}

void VisualStateCoordinator::record_completed(Clock::time_point now,
                                               std::chrono::seconds visible_for) noexcept {
    latest_changed_state_ = ReminderState::Completed;
    show_newest_task_ = false;
    preferred_task_index_ = -1;
    completed_until_ = now + visible_for;
    abnormal_until_ = Clock::time_point::min();
}

void VisualStateCoordinator::record_aborted(Clock::time_point now,
                                             std::chrono::seconds visible_for) noexcept {
    latest_changed_state_ = ReminderState::Error;
    show_newest_task_ = false;
    preferred_task_index_ = -1;
    abnormal_until_ = now + visible_for;
    completed_until_ = Clock::time_point::min();
    interrupted_until_ = Clock::time_point::min();
}

void VisualStateCoordinator::record_interrupted(Clock::time_point now,
                                                std::chrono::seconds visible_for) noexcept {
    latest_changed_state_ = ReminderState::Interrupted;
    show_newest_task_ = false;
    preferred_task_index_ = -1;
    interrupted_until_ = now + visible_for;
    abnormal_until_ = Clock::time_point::min();
    completed_until_ = Clock::time_point::min();
}

ReminderState VisualStateCoordinator::select(int active_count, Clock::time_point now) const noexcept {
    if (latest_changed_state_ == ReminderState::Interrupted && now < interrupted_until_) {
        return ReminderState::Interrupted;
    }
    return app_logic::select_visual_state(active_count, now < abnormal_until_,
                                          now < completed_until_, latest_changed_state_);
}

} // namespace codexpets

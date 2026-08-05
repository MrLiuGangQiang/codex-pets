#include "monitor_policy.h"

#include "app_logic.h"

namespace codexpets {

std::vector<MonitorEventEffect> apply_monitor_event_policy(
    VisualStateCoordinator& visual_coordinator,
    const MonitorSnapshot& snapshot,
    const std::vector<MonitorEventKind>& events,
    const AppSettings& settings,
    Clock::time_point now) {
    std::vector<MonitorEventEffect> effects;
    effects.reserve(events.size());
    for (const auto event : events) {
        const auto preferred_task_index = event == MonitorEventKind::PlanUpdated
            ? snapshot.latest_plan_update_active_title_index
            : event == MonitorEventKind::TaskStarted
                ? snapshot.latest_event_active_title_index
                : -1;
        MonitorEventEffect effect;
        switch (event) {
            case MonitorEventKind::TaskStarted:
                visual_coordinator.record_started(preferred_task_index);
                effect.reveal_pet = true;
                effect.sound = SoundCue::Started;
                effect.xiaoai_event = XiaoAiEvent::Started;
                effect.xiaoai_context = std::string(app_logic::select_notification_label(
                    snapshot.active_project_names, preferred_task_index));
                effects.push_back(std::move(effect));
                break;
            case MonitorEventKind::TaskCompleted:
                visual_coordinator.record_completed(now, std::chrono::seconds(
                    app_logic::cloud_notification_seconds(
                        ReminderState::Completed, settings.dock_notification_seconds)));
                effect.reveal_pet = true;
                effect.sound = SoundCue::Completed;
                effect.xiaoai_event = XiaoAiEvent::Completed;
                effect.xiaoai_context = snapshot.last_completed_project_name;
                effects.push_back(std::move(effect));
                break;
            case MonitorEventKind::TaskAborted:
                visual_coordinator.record_aborted(now, std::chrono::seconds(
                    app_logic::cloud_notification_seconds(
                        ReminderState::Error, settings.dock_notification_seconds)));
                effect.reveal_pet = true;
                effect.sound = SoundCue::Error;
                effect.xiaoai_event = XiaoAiEvent::Error;
                effect.xiaoai_context = snapshot.last_aborted_project_name;
                effects.push_back(std::move(effect));
                break;
            case MonitorEventKind::TaskInterrupted:
                visual_coordinator.record_interrupted(now, std::chrono::seconds(
                    app_logic::cloud_notification_seconds(
                        ReminderState::Interrupted, settings.dock_notification_seconds)));
                effect.reveal_pet = true;
                effect.xiaoai_event = XiaoAiEvent::Interrupted;
                effect.xiaoai_context = snapshot.last_interrupted_project_name;
                effects.push_back(std::move(effect));
                break;
            case MonitorEventKind::PlanUpdated:
                visual_coordinator.record_started(preferred_task_index);
                break;
            case MonitorEventKind::StateChanged:
                break;
        }
    }
    return effects;
}

} // namespace codexpets

#include "monitor_policy.h"

#include "app_logic.h"

#include <string_view>

namespace codexpets {
namespace {

std::string value_at(const std::vector<std::string>& values, int index, std::string fallback = {}) {
    if (index >= 0 && index < static_cast<int>(values.size())) {
        const auto& value = values[static_cast<std::size_t>(index)];
        if (!value.empty()) return value;
    }
    return fallback;
}

std::vector<TaskStep> steps_at(const MonitorSnapshot& snapshot, int index) {
    if (index >= 0 && index < static_cast<int>(snapshot.active_plan_steps.size())) {
        return snapshot.active_plan_steps[static_cast<std::size_t>(index)];
    }
    return {};
}

TaskNotification started_notification(const MonitorSnapshot& snapshot, int index,
                                      std::string_view event_context) {
    TaskNotification result;
    result.state = TaskNotificationState::Started;
    result.project_name = event_context.empty()
        ? value_at(snapshot.active_project_names, index) : std::string(event_context);
    result.task_title = value_at(snapshot.active_titles, index, "正在处理任务…");
    result.steps = steps_at(snapshot, index);
    return result;
}

TaskNotification fallback_notification(MonitorEventKind event, const MonitorSnapshot& snapshot,
                                       std::string_view event_context) {
    TaskNotification result;
    switch (event) {
        case MonitorEventKind::TaskCompleted:
            result.state = TaskNotificationState::Completed;
            result.task_title = snapshot.last_completed_title;
            result.project_name = snapshot.last_completed_project_name;
            break;
        case MonitorEventKind::TaskAborted:
            result.state = TaskNotificationState::Error;
            result.task_title = snapshot.last_aborted_title;
            result.project_name = snapshot.last_aborted_project_name;
            break;
        case MonitorEventKind::TaskInterrupted:
            result.state = TaskNotificationState::Interrupted;
            result.task_title = snapshot.last_interrupted_title;
            result.project_name = snapshot.last_interrupted_project_name;
            break;
        case MonitorEventKind::TaskStarted:
        case MonitorEventKind::PlanUpdated:
        case MonitorEventKind::StateChanged:
            break;
    }
    if (result.project_name.empty() && !event_context.empty()) {
        result.project_name = std::string(event_context);
    }
    return result;
}

std::optional<TaskNotification> captured_notification(const MonitorSnapshot& snapshot,
                                                      std::size_t event_index) {
    if (event_index >= snapshot.event_notifications.size()) return std::nullopt;
    return snapshot.event_notifications[event_index];
}

} // namespace

std::vector<MonitorEventEffect> apply_monitor_event_policy(
    VisualStateCoordinator& visual_coordinator,
    const MonitorSnapshot& snapshot,
    const std::vector<MonitorEventKind>& events,
    const AppSettings& settings,
    Clock::time_point now) {
    std::vector<MonitorEventEffect> effects;
    effects.reserve(events.size());
    for (std::size_t event_index = 0; event_index < events.size(); ++event_index) {
        const auto event = events[event_index];
        const auto event_context = event_index < snapshot.event_contexts.size()
            ? std::string_view(snapshot.event_contexts[event_index]) : std::string_view{};
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
                effect.xiaoai_context = event_context.empty() ? std::string(app_logic::select_notification_label(
                    snapshot.active_project_names, preferred_task_index)) : std::string(event_context);
                effect.task_notification = captured_notification(snapshot, event_index);
                if (!effect.task_notification) {
                    effect.task_notification = started_notification(
                        snapshot, preferred_task_index, event_context);
                }
                effects.push_back(std::move(effect));
                break;
            case MonitorEventKind::TaskCompleted:
                visual_coordinator.record_completed(now, std::chrono::seconds(
                    app_logic::cloud_notification_seconds(
                        ReminderState::Completed, settings.dock_notification_seconds)));
                effect.reveal_pet = true;
                effect.sound = SoundCue::Completed;
                effect.xiaoai_event = XiaoAiEvent::Completed;
                effect.xiaoai_context = event_context.empty()
                    ? snapshot.last_completed_project_name : std::string(event_context);
                effect.task_notification = captured_notification(snapshot, event_index);
                if (!effect.task_notification) {
                    effect.task_notification = fallback_notification(event, snapshot, event_context);
                }
                effects.push_back(std::move(effect));
                break;
            case MonitorEventKind::TaskAborted:
                visual_coordinator.record_aborted(now, std::chrono::seconds(
                    app_logic::cloud_notification_seconds(
                        ReminderState::Error, settings.dock_notification_seconds)));
                effect.reveal_pet = true;
                effect.sound = SoundCue::Error;
                effect.xiaoai_event = XiaoAiEvent::Error;
                effect.xiaoai_context = event_context.empty()
                    ? snapshot.last_aborted_project_name : std::string(event_context);
                effect.task_notification = captured_notification(snapshot, event_index);
                if (!effect.task_notification) {
                    effect.task_notification = fallback_notification(event, snapshot, event_context);
                }
                effects.push_back(std::move(effect));
                break;
            case MonitorEventKind::TaskInterrupted:
                visual_coordinator.record_interrupted(now, std::chrono::seconds(
                    app_logic::cloud_notification_seconds(
                        ReminderState::Interrupted, settings.dock_notification_seconds)));
                effect.reveal_pet = true;
                effect.sound = SoundCue::Interrupted;
                effect.xiaoai_event = XiaoAiEvent::Interrupted;
                effect.xiaoai_context = event_context.empty()
                    ? snapshot.last_interrupted_project_name : std::string(event_context);
                effect.task_notification = captured_notification(snapshot, event_index);
                if (!effect.task_notification) {
                    effect.task_notification = fallback_notification(event, snapshot, event_context);
                }
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

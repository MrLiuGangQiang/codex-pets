#include "presentation.h"

#include "app_logic.h"

namespace codexpets {
namespace {

std::string join_status_lines(const std::vector<std::string>& lines) {
    std::string result;
    for (const auto& line : lines) {
        if (line.empty()) continue;
        if (!result.empty()) result += '\n';
        result += line;
    }
    return result.empty() ? "空闲" : result;
}

} // namespace

VisualContent make_visual_content(ReminderState state, const MonitorSnapshot& snapshot) {
    VisualContent result;
    if (state == ReminderState::Error) {
        result.status_lines = {"异常"};
    } else if (state == ReminderState::Interrupted) {
        result.status_lines = {"已中断"};
    } else if (state == ReminderState::Completed) {
        result.status_lines = {"已完成"};
    } else if (state == ReminderState::Busy) {
        result.status_lines = app_logic::format_active_task_status_lines(
            snapshot.active_project_names, snapshot.active_titles, snapshot.active_plan_progress_labels);
    } else {
        result.status_lines = {"空闲"};
    }
    result.status_text = join_status_lines(result.status_lines);

    if (state == ReminderState::Error) {
        result.thought_text = "任务出现异常了。";
        result.task_titles = {app_logic::format_abnormal_task_text(snapshot.last_aborted_title)};
    } else if (state == ReminderState::Interrupted) {
        result.thought_text = "任务已中断了。";
        result.task_titles = {app_logic::format_interrupted_task_text(snapshot.last_interrupted_title)};
    } else if (state == ReminderState::Completed) {
        result.thought_text = "任务完成啦！";
        if (!snapshot.last_completed_title.empty()) result.task_titles = {snapshot.last_completed_title};
    } else if (state == ReminderState::Busy) {
        result.thought_text = snapshot.active_titles.empty() ? "正在认真处理你的任务…"
                                                              : snapshot.active_titles.front();
        result.task_titles = snapshot.active_titles;
        result.progress_labels = snapshot.active_plan_progress_labels;
        if (result.task_titles.empty()) result.task_titles = {"正在处理任务…"};
    } else {
        result.thought_text = "主人，现在没有在进行中的任务!别让我歇着!";
        result.task_titles = snapshot.active_titles;
        result.progress_labels = snapshot.active_plan_progress_labels;
    }
    return result;
}

} // namespace codexpets

#pragma once

#include "types.h"

#include <optional>
#include <string>
#include <vector>

namespace codexpets {

struct VisualContent {
    std::string status_text;
    std::string thought_text;
    std::vector<std::string> task_titles;
    std::vector<std::optional<std::string>> progress_labels;
};

[[nodiscard]] VisualContent make_visual_content(ReminderState state, const MonitorSnapshot& snapshot);

} // namespace codexpets

#include "monitor_update_queue.h"

#include <algorithm>
#include <iterator>

namespace codexpets {
namespace {

void append_events(std::vector<MonitorEventKind>& destination,
                   std::vector<std::string>& destination_contexts,
                   std::vector<MonitorEventKind>& source,
                   std::vector<std::string>& source_contexts) {
    const auto available = monitor_pending_event_limit -
        std::min(monitor_pending_event_limit, destination.size());
    const auto count = std::min(available, source.size());
    destination.insert(destination.end(),
                       std::make_move_iterator(source.begin()),
                       std::make_move_iterator(source.begin() +
                                               static_cast<std::ptrdiff_t>(count)));
    for (std::size_t index = 0; index < count; ++index) {
        destination_contexts.push_back(index < source_contexts.size()
            ? std::move(source_contexts[index]) : std::string{});
    }
}

void cap_events(std::vector<MonitorEventKind>& events, std::vector<std::string>& contexts) {
    if (events.size() > monitor_pending_event_limit) events.resize(monitor_pending_event_limit);
    contexts.resize(events.size());
}

} // namespace

bool MonitorUpdateQueue::push(PendingMonitorUpdate update) {
    std::lock_guard lock(mutex_);
    if (updates_.empty() || update.generation > updates_.back().generation) {
        updates_.clear();
        cap_events(update.events, update.snapshot.event_contexts);
        updates_.push_back(std::move(update));
        return true;
    }
    if (update.generation < updates_.back().generation) return false;

    auto& pending = updates_.back();
    auto contexts = std::move(pending.snapshot.event_contexts);
    append_events(pending.events, contexts, update.events, update.snapshot.event_contexts);
    pending.snapshot = std::move(update.snapshot);
    pending.snapshot.event_contexts = std::move(contexts);
    return true;
}

std::deque<PendingMonitorUpdate> MonitorUpdateQueue::take() {
    std::deque<PendingMonitorUpdate> result;
    std::lock_guard lock(mutex_);
    result.swap(updates_);
    return result;
}

void MonitorUpdateQueue::clear() noexcept {
    std::lock_guard lock(mutex_);
    updates_.clear();
}

} // namespace codexpets

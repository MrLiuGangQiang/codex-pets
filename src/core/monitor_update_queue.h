#pragma once

#include "session_monitor.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace codexpets {

struct PendingMonitorUpdate {
    std::uint64_t generation{};
    std::vector<MonitorEventKind> events;
    MonitorSnapshot snapshot;
};

// Thread-safe, bounded handoff between MonitorWorker and a platform UI thread.
// Updates from the same worker generation are coalesced to the newest snapshot,
// while a newer generation atomically discards stale work from an old sessions root.
class MonitorUpdateQueue {
public:
    MonitorUpdateQueue() = default;
    MonitorUpdateQueue(const MonitorUpdateQueue&) = delete;
    MonitorUpdateQueue& operator=(const MonitorUpdateQueue&) = delete;

    [[nodiscard]] bool push(PendingMonitorUpdate update);
    [[nodiscard]] std::deque<PendingMonitorUpdate> take();
    void clear() noexcept;

private:
    std::mutex mutex_;
    std::deque<PendingMonitorUpdate> updates_;
};

} // namespace codexpets

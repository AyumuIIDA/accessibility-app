#include "HandInput/Publication/ordered_hand_event_ring.h"

#include <algorithm>

namespace ryoiki::hand_input::publication
{
std::uint64_t OrderedHandEventRing::publish(
    recognition::HandEvent event) noexcept
{
    std::lock_guard lock{mutex_};
    event.sequence = nextSequence_++;
    events_[(event.sequence - 1) % events_.size()] = event;
    count_ = std::min(count_ + 1, events_.size());
    return event.sequence;
}

HandEventBatch OrderedHandEventRing::readAfter(
    const std::uint64_t afterSequence) const noexcept
{
    std::lock_guard lock{mutex_};
    HandEventBatch result{};
    const std::uint64_t newestSequence = nextSequence_ - 1;
    const std::uint64_t oldestSequence = count_ == 0
        ? nextSequence_
        : nextSequence_ - count_;
    std::uint64_t firstSequence = afterSequence == UINT64_MAX
        ? nextSequence_
        : afterSequence + 1;
    if (firstSequence < oldestSequence)
    {
        result.droppedCount = oldestSequence - firstSequence;
        firstSequence = oldestSequence;
    }
    for (std::uint64_t sequence = firstSequence;
         sequence <= newestSequence && result.count < result.events.size();
         ++sequence)
    {
        result.events[result.count++] =
            events_[(sequence - 1) % events_.size()];
    }
    result.nextSequence = result.count > 0
        ? result.events[result.count - 1].sequence
        : afterSequence;
    return result;
}

void OrderedHandEventRing::reset() noexcept
{
    std::lock_guard lock{mutex_};
    events_ = {};
    nextSequence_ = 1;
    count_ = 0;
}
}

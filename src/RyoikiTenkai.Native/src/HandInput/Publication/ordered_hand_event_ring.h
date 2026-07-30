#pragma once

#include "HandInput/Recognition/hand_event.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace ryoiki::hand_input::publication
{
inline constexpr std::size_t kHandEventRingCapacity = 64;
inline constexpr std::size_t kMaxHandEventBatch = 16;

struct HandEventBatch
{
    std::uint64_t nextSequence{0};
    std::uint64_t droppedCount{0};
    std::size_t count{0};
    std::array<recognition::HandEvent, kMaxHandEventBatch> events{};
};

class OrderedHandEventRing final
{
public:
    std::uint64_t publish(recognition::HandEvent event) noexcept;
    [[nodiscard]] HandEventBatch readAfter(
        std::uint64_t afterSequence) const noexcept;
    void reset() noexcept;

private:
    mutable std::mutex mutex_;
    std::array<recognition::HandEvent, kHandEventRingCapacity> events_{};
    std::uint64_t nextSequence_{1};
    std::size_t count_{0};
};
}

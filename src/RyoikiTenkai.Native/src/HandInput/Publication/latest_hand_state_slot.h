#pragma once

#include "HandInput/Recognition/hand_state.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace ryoiki::hand_input::publication
{
inline constexpr std::size_t kMaxHandStates = 16;

struct HandStateSnapshot
{
    std::uint64_t frameId{0};
    std::uint64_t timestampUs{0};
    std::size_t count{0};
    std::array<recognition::HandStateResult, kMaxHandStates> states{};
};

class LatestHandStateSlot final
{
public:
    void publish(const HandStateSnapshot& snapshot) noexcept;
    [[nodiscard]] HandStateSnapshot latest() const noexcept;
    void reset() noexcept;

private:
    mutable std::mutex mutex_;
    HandStateSnapshot snapshot_{};
};
}

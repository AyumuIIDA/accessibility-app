#pragma once

#include "HandInput/Recognition/hand_state.h"

#include <cstdint>

namespace ryoiki::hand_input::recognition
{
struct TimedStateConfig
{
    std::uint64_t enterDurationUs{0};
    std::uint64_t exitDurationUs{0};
    std::uint64_t missingGraceUs{0};
};

class TimedStateStabilizer final
{
public:
    explicit TimedStateStabilizer(TimedStateConfig config) noexcept;

    [[nodiscard]] HandStateResult update(
        std::uint32_t stateId,
        bool inputValid,
        bool conditionMet,
        float confidence,
        float inputQuality,
        std::uint64_t frameId,
        std::uint64_t timestampUs) noexcept;

    void reset() noexcept;

private:
    TimedStateConfig config_{};
    HandStatePhase phase_{HandStatePhase::Inactive};
    std::uint64_t candidateSinceUs_{0};
    std::uint64_t exitSinceUs_{0};
    std::uint64_t missingSinceUs_{0};
    std::uint64_t beganFrameId_{0};
};
}

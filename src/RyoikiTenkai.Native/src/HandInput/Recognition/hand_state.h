#pragma once

#include <cstdint>

namespace ryoiki::hand_input::recognition
{
inline constexpr std::uint32_t kDomainExpansionStateId = 1;
inline constexpr std::uint32_t kOpenPalmStateId = 2;

enum class HandStatePhase : std::uint32_t
{
    Inactive = 0,
    Candidate = 1,
    Active = 2
};

enum class HandStateTransition : std::uint32_t
{
    None = 0,
    Began = 1,
    Ended = 2,
    Cancelled = 3
};

enum class HandStateFlags : std::uint32_t
{
    None = 0,
    Stale = 1U << 0U,
    ExitPending = 1U << 1U
};

struct HandStateResult
{
    std::uint32_t id{0};
    HandStatePhase phase{HandStatePhase::Inactive};
    HandStateTransition transition{HandStateTransition::None};
    HandStateFlags flags{HandStateFlags::None};
    float confidence{0.0F};
    float inputQuality{0.0F};
    std::uint64_t beganFrameId{0};
    std::uint64_t currentFrameId{0};
    std::uint64_t timestampUs{0};
};
}

#pragma once

#include <cstdint>

namespace ryoiki::hand_input::recognition
{
struct HandEvent
{
    std::uint64_t sequence{0};
    std::uint32_t id{0};
    float confidence{0.0F};
    float inputQuality{0.0F};
    std::uint64_t beganFrameId{0};
    std::uint64_t endedFrameId{0};
    std::uint64_t beganTimestampUs{0};
    std::uint64_t endedTimestampUs{0};
    float displacementX{0.0F};
    float displacementY{0.0F};
    std::uint64_t durationUs{0};
};
}

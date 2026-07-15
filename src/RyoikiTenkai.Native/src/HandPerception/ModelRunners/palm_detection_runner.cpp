#include "HandPerception/ModelRunners/palm_detection_runner.h"

namespace ryoiki::hand_perception
{
std::span<float> PalmDetectionRawOutput::regressions() noexcept
{
    return regressions_;
}

std::span<const float> PalmDetectionRawOutput::regressions() const noexcept
{
    return regressions_;
}

std::span<float> PalmDetectionRawOutput::scores() noexcept
{
    return scores_;
}

std::span<const float> PalmDetectionRawOutput::scores() const noexcept
{
    return scores_;
}
}

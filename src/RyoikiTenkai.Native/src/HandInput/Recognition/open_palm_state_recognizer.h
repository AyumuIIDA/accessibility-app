#pragma once

#include "HandInput/Measurements/hand_measurement_extractor.h"
#include "HandInput/Recognition/hand_state.h"
#include "HandInput/Recognition/timed_state_stabilizer.h"

namespace ryoiki::hand_input::recognition
{
struct OpenPalmStateSample
{
    float fourFingerExtension{0.0F};
    float thumbOpenness{0.0F};
    float confidence{0.0F};
    bool detected{false};
    bool inputValid{false};
};

[[nodiscard]] OpenPalmStateSample recognizeOpenPalmState(
    const measurements::HandMeasurements& measurements) noexcept;

class OpenPalmStateRecognizer final
{
public:
    OpenPalmStateRecognizer() noexcept;

    [[nodiscard]] HandStateResult process(
        const OpenPalmStateSample& sample,
        float inputQuality,
        std::uint64_t frameId,
        std::uint64_t timestampUs) noexcept;

    void reset() noexcept;

private:
    TimedStateStabilizer stabilizer_;
};
}

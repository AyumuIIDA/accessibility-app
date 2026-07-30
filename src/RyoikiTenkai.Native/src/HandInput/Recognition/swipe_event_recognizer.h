#pragma once

#include "HandInput/Measurements/hand_measurement_extractor.h"
#include "HandInput/Recognition/hand_event.h"

#include <optional>

namespace ryoiki::hand_input::recognition
{
struct SwipeEventConfig
{
    float motionStartDistance{0.025F};
    float minimumDisplacement{0.16F};
    float maximumVerticalDisplacement{0.10F};
    float minimumStraightness{0.78F};
    float minimumScaleRatio{0.65F};
    float maximumScaleRatio{1.55F};
    std::uint64_t minimumDurationUs{100'000};
    std::uint64_t maximumDurationUs{700'000};
    std::uint64_t anchorTimeoutUs{250'000};
    std::uint64_t cooldownUs{350'000};
};

class SwipeEventRecognizer final
{
public:
    explicit SwipeEventRecognizer(SwipeEventConfig config = {}) noexcept;

    [[nodiscard]] std::optional<HandEvent> process(
        const measurements::HandMeasurementFrame& measurements,
        bool gestureGate,
        float gestureConfidence) noexcept;

    void reset() noexcept;

private:
    enum class Phase
    {
        Idle,
        Tracking,
        Cooldown
    };

    void captureAnchor(
        const measurements::ScreenPalmMeasurement& screenPalm) noexcept;

    SwipeEventConfig config_{};
    Phase phase_{Phase::Idle};
    float anchorX_{0.0F};
    float anchorY_{0.0F};
    float anchorScale_{0.0F};
    float previousX_{0.0F};
    float previousY_{0.0F};
    float pathLength_{0.0F};
    float minimumGateConfidence_{1.0F};
    std::uint64_t beganFrameId_{0};
    std::uint64_t beganTimestampUs_{0};
    std::uint64_t cooldownSinceUs_{0};
};
}

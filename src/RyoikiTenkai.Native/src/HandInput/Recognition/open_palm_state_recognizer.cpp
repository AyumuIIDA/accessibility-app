#include "HandInput/Recognition/open_palm_state_recognizer.h"

#include <algorithm>
#include <cmath>

namespace ryoiki::hand_input::recognition
{
namespace
{
float smoothStep(const float low, const float high, const float value) noexcept
{
    if (!std::isfinite(value) || high <= low) return 0.0F;
    const float t = std::clamp((value - low) / (high - low), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}
}

OpenPalmStateSample recognizeOpenPalmState(
    const measurements::HandMeasurements& measurements) noexcept
{
    OpenPalmStateSample result{};
    result.inputValid = measurements.present
        && measurements.quality
            == measurements::HandMeasurementQuality::Valid
        && measurements.trackingQuality >= 0.70F;
    if (!result.inputValid) return result;

    result.fourFingerExtension = *std::min_element(
        measurements.extension.begin() + 1,
        measurements.extension.end());
    const float thumbStraightness = measurements.extension[0];
    const float thumbSeparation = smoothStep(
        0.22F, 0.52F, measurements.thumbIndexDistance);
    result.thumbOpenness = std::max(thumbStraightness, thumbSeparation);

    const float fingerScore = smoothStep(
        0.62F, 0.90F, result.fourFingerExtension);
    const float thumbScore = smoothStep(
        0.35F, 0.75F, result.thumbOpenness);
    result.confidence = std::clamp(
        fingerScore * (0.75F + 0.25F * thumbScore), 0.0F, 1.0F);
    result.detected = result.fourFingerExtension >= 0.72F
        && result.thumbOpenness >= 0.45F;
    return result;
}

OpenPalmStateRecognizer::OpenPalmStateRecognizer() noexcept
    : stabilizer_{TimedStateConfig{
        .enterDurationUs = 120'000,
        .exitDurationUs = 150'000,
        .missingGraceUs = 220'000}}
{
}

HandStateResult OpenPalmStateRecognizer::process(
    const OpenPalmStateSample& sample,
    const float inputQuality,
    const std::uint64_t frameId,
    const std::uint64_t timestampUs) noexcept
{
    return stabilizer_.update(
        kOpenPalmStateId,
        sample.inputValid,
        sample.detected,
        sample.confidence,
        inputQuality,
        frameId,
        timestampUs);
}

void OpenPalmStateRecognizer::reset() noexcept
{
    stabilizer_.reset();
}
}

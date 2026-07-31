#include "HandInput/Measurements/hand_topology_history.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace ryoiki::hand_input::measurements
{
namespace
{
constexpr float kTwoPi = 6.283185307179586F;
constexpr float kPi = 3.141592653589793F;
// Signed palm area near zero carries no reliable side information. The dead
// band prevents sensor noise around an edge-on palm from being counted as a
// palm-turn crossing.
constexpr float kSignedAreaDeadBand = 0.002F;
constexpr float kMinimumHandScale = 1.0e-5F;
constexpr float kMinimumBoundingBoxArea = 1.0e-6F;

int signWithDeadBand(const float value) noexcept
{
    if (!std::isfinite(value) || std::abs(value) < kSignedAreaDeadBand)
    {
        return 0;
    }
    return value > 0.0F ? 1 : -1;
}
}

void HandTopologyHistory::push(
    const HandTopologyMeasurement& measurement) noexcept
{
    if (!measurement.valid)
    {
        noteMissingObservation();
        return;
    }

    missingObservations_ = 0;
    if (size_ == samples_.size())
    {
        popOldest();
    }
    samples_[(start_ + size_) % samples_.size()] = measurement;
    ++size_;
    pruneToWindow();
}

void HandTopologyHistory::noteMissingObservation() noexcept
{
    if (size_ == 0)
    {
        return;
    }
    ++missingObservations_;
    if (missingObservations_ > kHandTopologyGapToleranceFrames)
    {
        reset();
    }
}

void HandTopologyHistory::reset() noexcept
{
    start_ = 0;
    size_ = 0;
    missingObservations_ = 0;
}

void HandTopologyHistory::popOldest() noexcept
{
    if (size_ == 0)
    {
        return;
    }
    start_ = (start_ + 1) % samples_.size();
    --size_;
}

void HandTopologyHistory::pruneToWindow() noexcept
{
    while (size_ > 1)
    {
        const auto& newest = at(size_ - 1);
        const auto& oldest = at(0);
        if (newest.timestampUs < oldest.timestampUs
            || newest.timestampUs - oldest.timestampUs <= kHandTopologyWindowUs)
        {
            return;
        }
        popOldest();
    }
}

const HandTopologyMeasurement& HandTopologyHistory::at(
    const std::size_t chronologicalIndex) const noexcept
{
    return samples_[(start_ + chronologicalIndex) % samples_.size()];
}

std::size_t HandTopologyHistory::size() const noexcept
{
    return size_;
}

std::uint32_t HandTopologyHistory::missingObservationCount() const noexcept
{
    return missingObservations_;
}

HandTopologySummary HandTopologyHistory::summarize() const noexcept
{
    HandTopologySummary result{};
    if (size_ == 0)
    {
        return result;
    }

    const auto& first = at(0);
    const auto& last = at(size_ - 1);
    result.firstFrameId = first.frameId;
    result.lastFrameId = last.frameId;
    result.durationUs = last.timestampUs >= first.timestampUs
        ? last.timestampUs - first.timestampUs
        : 0;
    result.sampleCount = size_;
    result.startFingerStateMask = first.fingerStateMask;
    result.endFingerStateMask = last.fingerStateMask;

    std::array<float, 5> straightnessMin{};
    std::array<float, 5> straightnessMax{};
    straightnessMin.fill(std::numeric_limits<float>::max());
    straightnessMax.fill(std::numeric_limits<float>::lowest());
    float signedAreaMin = std::numeric_limits<float>::max();
    float signedAreaMax = std::numeric_limits<float>::lowest();
    // Palm compression is only meaningful when it was actually measured, so
    // non-positive samples are excluded from the min/max reduction.
    float compressionMin = 0.0F;
    float compressionMax = 0.0F;
    bool hasCompression = false;
    float depthMax = 0.0F;
    float scaleRatioFirst = 0.0F;
    float scaleRatioLast = 0.0F;
    float scaleRatioMin = std::numeric_limits<float>::max();
    float scaleRatioMax = std::numeric_limits<float>::lowest();
    float areaRatioFirst = 0.0F;
    float areaRatioLast = 0.0F;
    float areaRatioMin = std::numeric_limits<float>::max();
    float areaRatioMax = std::numeric_limits<float>::lowest();
    float handednessMin = std::numeric_limits<float>::max();
    float handednessMax = std::numeric_limits<float>::lowest();
    double handednessSum = 0.0;
    float unwrappedMin = std::numeric_limits<float>::max();
    float unwrappedMax = std::numeric_limits<float>::lowest();
    float previousRawAngle = first.palmAxisRadians;
    float angleOffset = 0.0F;
    int previousAreaSign = 0;

    const float initialScale = (std::max)(first.handScale, kMinimumHandScale);
    const float initialArea =
        (std::max)(first.boundingBoxArea, kMinimumBoundingBoxArea);
    const bool initialAreaUsable =
        first.boundingBoxArea > kMinimumBoundingBoxArea;

    for (std::size_t index = 0; index < size_; ++index)
    {
        const auto& sample = at(index);
        for (std::size_t finger = 0;
            finger < sample.fingerStraightness.size();
            ++finger)
        {
            straightnessMin[finger] = (std::min)(
                straightnessMin[finger], sample.fingerStraightness[finger]);
            straightnessMax[finger] = (std::max)(
                straightnessMax[finger], sample.fingerStraightness[finger]);
        }

        signedAreaMin = (std::min)(signedAreaMin, sample.signedPalmArea);
        signedAreaMax = (std::max)(signedAreaMax, sample.signedPalmArea);
        const int areaSign = signWithDeadBand(sample.signedPalmArea);
        if (areaSign != 0)
        {
            if (previousAreaSign != 0 && areaSign != previousAreaSign)
            {
                ++result.signedPalmAreaSignChanges;
            }
            previousAreaSign = areaSign;
        }

        if (sample.palmCompression > 0.0F)
        {
            compressionMin = hasCompression
                ? (std::min)(compressionMin, sample.palmCompression)
                : sample.palmCompression;
            compressionMax = hasCompression
                ? (std::max)(compressionMax, sample.palmCompression)
                : sample.palmCompression;
            hasCompression = true;
        }

        depthMax = (std::max)(depthMax, sample.palmDepthRange);

        const float scaleRatio = sample.handScale / initialScale;
        scaleRatioMin = (std::min)(scaleRatioMin, scaleRatio);
        scaleRatioMax = (std::max)(scaleRatioMax, scaleRatio);

        // A bounding box may be unavailable for a frame. The prototype falls
        // back to the squared palm-scale ratio, which is the area a uniform
        // scale change would have produced.
        const float areaRatio =
            (sample.boundingBoxArea <= kMinimumBoundingBoxArea
                || !initialAreaUsable)
            ? scaleRatio * scaleRatio
            : sample.boundingBoxArea / initialArea;
        areaRatioMin = (std::min)(areaRatioMin, areaRatio);
        areaRatioMax = (std::max)(areaRatioMax, areaRatio);

        handednessMin = (std::min)(handednessMin, sample.handedness);
        handednessMax = (std::max)(handednessMax, sample.handedness);
        handednessSum += static_cast<double>(sample.handedness);

        if (index == 0)
        {
            scaleRatioFirst = scaleRatio;
            areaRatioFirst = areaRatio;
            unwrappedMin = sample.palmAxisRadians;
            unwrappedMax = sample.palmAxisRadians;
        }
        else
        {
            const float delta = sample.palmAxisRadians - previousRawAngle;
            if (delta > kPi)
            {
                angleOffset -= kTwoPi;
            }
            else if (delta < -kPi)
            {
                angleOffset += kTwoPi;
            }
            const float unwrapped = sample.palmAxisRadians + angleOffset;
            unwrappedMin = (std::min)(unwrappedMin, unwrapped);
            unwrappedMax = (std::max)(unwrappedMax, unwrapped);

            const auto& previous = at(index - 1);
            result.fingerStateTransitionCount += static_cast<std::uint32_t>(
                std::popcount(
                    previous.fingerStateMask ^ sample.fingerStateMask));

            // Index 0's step refers to an observation outside this window, so
            // accumulation deliberately starts at index 1.
            result.translationDeltaX += sample.translationStepX;
            result.translationDeltaY += sample.translationStepY;
            result.palmTravel += std::hypot(
                sample.translationStepX, sample.translationStepY);
        }
        previousRawAngle = sample.palmAxisRadians;
        scaleRatioLast = scaleRatio;
        areaRatioLast = areaRatio;
    }

    for (std::size_t finger = 0; finger < straightnessMin.size(); ++finger)
    {
        result.fingerStraightnessRangeMax = (std::max)(
            result.fingerStraightnessRangeMax,
            straightnessMax[finger] - straightnessMin[finger]);
    }

    result.translationDistance = std::hypot(
        result.translationDeltaX, result.translationDeltaY);
    result.palmOrientationRangeRadians = unwrappedMax - unwrappedMin;
    result.handednessRange = handednessMax - handednessMin;
    result.handednessMean = static_cast<float>(
        handednessSum / static_cast<double>(size_));
    result.signedPalmAreaRange = signedAreaMax - signedAreaMin;
    result.palmCompressionMin = compressionMin;
    result.palmCompressionMax = compressionMax;
    result.palmCompressionDrop = compressionMax > 0.0F
        ? std::clamp(
            (compressionMax - compressionMin) / compressionMax, 0.0F, 1.0F)
        : 0.0F;
    result.palmDepthRangeMax = depthMax;
    result.handScaleRatioRange = scaleRatioMax - scaleRatioMin;
    result.handScaleRatioDelta = size_ < 2
        ? 0.0F
        : scaleRatioLast - scaleRatioFirst;
    result.boundingBoxAreaRatioRange = areaRatioMax - areaRatioMin;
    result.boundingBoxAreaRatioDelta = size_ < 2
        ? 0.0F
        : areaRatioLast - areaRatioFirst;

    result.palmTurnScore =
        (result.signedPalmAreaSignChanges > 0 ? 0.38F : 0.0F)
        + result.palmCompressionDrop * 0.42F
        + std::clamp(result.signedPalmAreaRange * 8.0F, 0.0F, 0.35F)
        + std::clamp(result.palmDepthRangeMax * 2.5F, 0.0F, 0.30F);

    const float sizeChangeScore = std::clamp(
        (std::max)(
            std::abs(result.handScaleRatioDelta),
            std::abs(result.boundingBoxAreaRatioDelta) * 0.5F),
        0.0F,
        1.2F);
    result.topologyChangeScore =
        result.palmTravel
        + result.palmOrientationRangeRadians * 0.6F
        + result.handednessRange * 0.8F
        + static_cast<float>(result.fingerStateTransitionCount) * 0.15F
        + result.fingerStraightnessRangeMax * 0.75F
        + result.palmTurnScore
        + sizeChangeScore * 0.55F;
    result.valid = std::isfinite(result.topologyChangeScore)
        && std::isfinite(result.palmTurnScore)
        && std::isfinite(result.palmTravel)
        && std::isfinite(result.translationDistance);
    return result;
}
}

#include "HandInput/Measurements/hand_unified_feature_frame.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>

namespace ryoiki::hand_input::measurements
{
namespace
{
using hand_perception::Landmark3f;

constexpr std::array<std::size_t, kFingerCount> kFingerBases{1, 5, 9, 13, 17};
constexpr std::array<std::size_t, kFingerCount> kFingerTips{4, 8, 12, 16, 20};

float distance2d(const Landmark3f& left, const Landmark3f& right) noexcept
{
    return std::hypot(left.x - right.x, left.y - right.y);
}

float lerp(const float a, const float b, const float t) noexcept
{
    const float clampedT = std::clamp(t, 0.0F, 1.0F);
    return a + (b - a) * clampedT;
}

float lerpAngle(const float a, const float b, const float t) noexcept
{
    constexpr float kTwoPi = 6.283185307179586F;
    constexpr float kPi = 3.141592653589793F;
    float delta = b - a;
    while (delta > kPi) delta -= kTwoPi;
    while (delta < -kPi) delta += kTwoPi;
    return a + delta * std::clamp(t, 0.0F, 1.0F);
}

// One observation's raw wrist/scale/area facts, retained alongside its
// UnifiedFeatureFrame so buildFeatureSequence's second pass can compute
// sequence-relative translation, velocity, and ratios without recomputing
// the frame's flat value vector. Mirrors the PR prototype's private
// RawFeatureFrame record.
struct RawObservationFacts
{
    std::uint64_t timestampUs{0};
    float wristX{0.0F};
    float wristY{0.0F};
    // wrist-to-middle-MCP distance, floored at 1 pixel, matching the PR
    // prototype's per-frame `scale`.
    float scale{0.0F};
    float boundingBoxArea{0.0F};
};

// --- summarizeTopology helpers, ported from GestureFeatureExtractor -------

// Mirrors GestureFeatureExtractor.UnwrapAngles.
std::vector<float> unwrapAngles(const std::vector<float>& angles)
{
    if (angles.empty())
    {
        return {};
    }
    constexpr float kPi = 3.14159265358979323846F;
    constexpr float kTwoPi = kPi * 2.0F;
    std::vector<float> result;
    result.reserve(angles.size());
    result.push_back(angles[0]);
    float previous = angles[0];
    float offset = 0.0F;
    for (std::size_t index = 1; index < angles.size(); ++index)
    {
        const float current = angles[index];
        const float delta = current - previous;
        if (delta > kPi)
        {
            offset -= kTwoPi;
        }
        else if (delta < -kPi)
        {
            offset += kTwoPi;
        }
        result.push_back(current + offset);
        previous = current;
    }
    return result;
}

// Mirrors GestureFeatureExtractor.CountSignChanges.
int countSignChanges(const std::vector<float>& values)
{
    int changes = 0;
    int previous = 0;
    for (const float value : values)
    {
        const int sign = std::abs(value) < 0.002F ? 0 : (value > 0.0F ? 1 : -1);
        if (sign == 0)
        {
            continue;
        }
        if (previous != 0 && sign != previous)
        {
            ++changes;
        }
        previous = sign;
    }
    return changes;
}
}

FingerPose analyzeFingerPose(
    const std::array<Landmark3f, kLandmarkCount>& imageLandmarks) noexcept
{
    FingerPose pose{};
    const Landmark3f& wrist = imageLandmarks[0];
    for (std::size_t finger = 0; finger < kFingerCount; ++finger)
    {
        const float baseDistance = (std::max)(
            distance2d(wrist, imageLandmarks[kFingerBases[finger]]), 1.0e-3F);
        const float tipDistance = distance2d(wrist, imageLandmarks[kFingerTips[finger]]);
        const float ratio = tipDistance / baseDistance;
        const float value = std::clamp((ratio - 1.05F) / 0.65F, 0.0F, 1.0F);
        pose.straightness[finger] = value;
        if (value >= kFingerOpenThreshold)
        {
            pose.stateMask |= 1U << finger;
        }
    }
    return pose;
}

UnifiedFeatureObservation makeUnifiedFeatureObservation(
    const hand_perception::HandLandmarkResult& hand, const std::uint64_t timestampUs) noexcept
{
    UnifiedFeatureObservation observation{};
    observation.timestampUs = timestampUs;
    observation.imageLandmarks = hand.landmarks;
    observation.handedness = hand.handedness;
    const float width = (std::max)(0.0F, hand.box.right - hand.box.left);
    const float height = (std::max)(0.0F, hand.box.bottom - hand.box.top);
    observation.boundingBoxArea = width * height;
    return observation;
}

UnifiedFeatureFrame buildFeatureFrame(const UnifiedFeatureObservation& observation) noexcept
{
    const auto& landmarks = observation.imageLandmarks;
    const Landmark3f& wrist = landmarks[0];
    // Landmark index 9 is the middle-finger MCP. It anchors both the
    // rotation-normalization scale/orientation (this block) and the
    // palm-turn quantities below, exactly as in the PR prototype's Extract()
    // and AnalyzePalmTurn, which independently floor the same wrist-to-
    // landmark-9 distance at two different minimums (1 and 0.001). That
    // difference only matters for near-degenerate synthetic input and is
    // preserved here for fidelity rather than resolved.
    const Landmark3f& middleMcp = landmarks[9];
    const float scale = (std::max)(1.0F, distance2d(wrist, middleMcp));
    const float orientation = std::atan2(middleMcp.y - wrist.y, middleMcp.x - wrist.x);
    const float cosNegOrientation = std::cos(-orientation);
    const float sinNegOrientation = std::sin(-orientation);
    const FingerPose pose = analyzeFingerPose(landmarks);

    UnifiedFeatureFrame frame{};
    frame.palmOrientationRadians = orientation;
    auto& values = frame.values;
    std::size_t offset = 0;
    for (std::size_t index = 0; index < kLandmarkCount; ++index)
    {
        const float x = (landmarks[index].x - wrist.x) / scale;
        const float y = (landmarks[index].y - wrist.y) / scale;
        values[offset++] = x * cosNegOrientation - y * sinNegOrientation;
        values[offset++] = x * sinNegOrientation + y * cosNegOrientation;
    }
    for (std::size_t finger = 0; finger < kFingerCount; ++finger)
    {
        const float d = distance2d(landmarks[kFingerBases[finger]], landmarks[kFingerTips[finger]]);
        values[offset++] = std::clamp(d / scale, 0.0F, 3.0F);
    }
    for (std::size_t finger = 1; finger < kFingerCount; ++finger)
    {
        const float d = distance2d(landmarks[kFingerTips[finger - 1]], landmarks[kFingerTips[finger]]);
        values[offset++] = std::clamp(d / scale, 0.0F, 3.0F);
    }
    for (std::size_t finger = 0; finger < kFingerCount; ++finger)
    {
        values[offset++] = pose.straightness[finger];
    }

    values[offset++] = std::sin(orientation);
    values[offset++] = std::cos(orientation);
    values[offset++] = observation.handedness;

    const Landmark3f& indexMcp = landmarks[5];
    const Landmark3f& pinkyMcp = landmarks[17];
    const float palmAxisLength = (std::max)(1.0e-3F, distance2d(wrist, middleMcp));
    const float signedPalmArea =
        ((indexMcp.x - wrist.x) * (pinkyMcp.y - wrist.y)
            - (indexMcp.y - wrist.y) * (pinkyMcp.x - wrist.x))
        / (std::max)(1.0e-3F, scale * scale);
    const float palmCompression = distance2d(indexMcp, pinkyMcp) / palmAxisLength;
    float minimumZ = landmarks[0].z;
    float maximumZ = landmarks[0].z;
    for (const auto& point : landmarks)
    {
        minimumZ = (std::min)(minimumZ, point.z);
        maximumZ = (std::max)(maximumZ, point.z);
    }
    const float depthRange = (maximumZ - minimumZ) / (std::max)(1.0e-3F, scale);

    values[offset++] = signedPalmArea;
    values[offset++] = palmCompression;
    values[offset++] = depthRange;
    values[offset++] = 1.0F; // hand-scale ratio identity; buildFeatureSequence overwrites.
    values[offset] = 1.0F;   // bounding-box-area ratio identity; buildFeatureSequence overwrites.

    return frame;
}

std::vector<UnifiedFeatureFrame> buildFeatureSequence(
    const std::vector<UnifiedFeatureObservation>& observations) noexcept
{
    if (observations.empty())
    {
        return {};
    }

    std::vector<UnifiedFeatureFrame> frames;
    std::vector<RawObservationFacts> raw;
    frames.reserve(observations.size());
    raw.reserve(observations.size());
    const std::uint64_t firstTimestampUs = observations.front().timestampUs;
    for (const auto& observation : observations)
    {
        frames.push_back(buildFeatureFrame(observation));
        frames.back().timeOffsetMs =
            (static_cast<double>(observation.timestampUs) - static_cast<double>(firstTimestampUs))
            / 1000.0;

        const Landmark3f& wrist = observation.imageLandmarks[0];
        const Landmark3f& middleMcp = observation.imageLandmarks[9];
        raw.push_back(RawObservationFacts{
            observation.timestampUs,
            wrist.x,
            wrist.y,
            (std::max)(1.0F, distance2d(wrist, middleMcp)),
            observation.boundingBoxArea});
    }

    const float firstScale = (std::max)(1.0e-3F, raw.front().scale);
    const float firstArea = (std::max)(1.0e-3F, raw.front().boundingBoxArea);
    float previousCenterX = 0.0F;
    float previousCenterY = 0.0F;
    for (std::size_t index = 0; index < frames.size(); ++index)
    {
        float velocity = 0.0F;
        if (index > 0)
        {
            const auto& previous = raw[index - 1];
            const auto& current = raw[index];
            const double deltaUs = current.timestampUs >= previous.timestampUs
                ? static_cast<double>(current.timestampUs - previous.timestampUs)
                : 0.0;
            const double dtSeconds = (std::max)(0.001, deltaUs * 1.0e-6);
            const float averageScale = (std::max)(1.0F, 0.5F * (previous.scale + current.scale));
            const float dx = (current.wristX - previous.wristX) / averageScale;
            const float dy = (current.wristY - previous.wristY) / averageScale;
            velocity = std::hypot(dx, dy) / static_cast<float>(dtSeconds);
            previousCenterX += dx;
            previousCenterY += dy;
        }

        const float scaleRatio = raw[index].scale / firstScale;
        const float areaRatio =
            (raw[index].boundingBoxArea <= 0.0F || firstArea <= 1.0e-3F)
            ? scaleRatio * scaleRatio
            : raw[index].boundingBoxArea / firstArea;
        frames[index].values[kHandScaleRatioOffset] = scaleRatio;
        frames[index].values[kBoundingBoxAreaRatioOffset] = areaRatio;
        frames[index].centerX = previousCenterX;
        frames[index].centerY = previousCenterY;
        frames[index].palmVelocity = velocity;
    }

    return frames;
}

std::array<float, kFingerStraightnessLength> readFingerStraightness(
    const std::array<float, kUnifiedFeatureVectorLength>& values) noexcept
{
    std::array<float, kFingerStraightnessLength> result{};
    for (std::size_t index = 0; index < kFingerStraightnessLength; ++index)
    {
        result[index] = values[kFingerStraightnessOffset + index];
    }
    return result;
}

std::uint32_t readFingerStateMask(
    const std::array<float, kUnifiedFeatureVectorLength>& values) noexcept
{
    const auto straightness = readFingerStraightness(values);
    std::uint32_t mask = 0;
    for (std::size_t index = 0; index < straightness.size(); ++index)
    {
        if (straightness[index] >= kFingerOpenThreshold)
        {
            mask |= 1U << index;
        }
    }
    return mask;
}

UnifiedFeatureFrame interpolate(
    const UnifiedFeatureFrame& a,
    const UnifiedFeatureFrame& b,
    const double timeOffsetMs,
    const float t) noexcept
{
    UnifiedFeatureFrame result{};
    result.timeOffsetMs = timeOffsetMs;
    result.centerX = lerp(a.centerX, b.centerX, t);
    result.centerY = lerp(a.centerY, b.centerY, t);
    result.palmOrientationRadians = lerpAngle(a.palmOrientationRadians, b.palmOrientationRadians, t);
    result.palmVelocity = lerp(a.palmVelocity, b.palmVelocity, t);
    for (std::size_t index = 0; index < kUnifiedFeatureVectorLength; ++index)
    {
        result.values[index] = lerp(a.values[index], b.values[index], t);
    }
    return result;
}

std::vector<UnifiedFeatureFrame> resample(
    const std::vector<UnifiedFeatureFrame>& frames, const std::size_t length) noexcept
{
    if (frames.empty() || length == 0)
    {
        return {};
    }
    if (frames.size() == 1)
    {
        return std::vector<UnifiedFeatureFrame>(length, frames.front());
    }

    const double firstTime = frames.front().timeOffsetMs;
    const double duration = (std::max)(1.0, frames.back().timeOffsetMs - firstTime);
    std::vector<UnifiedFeatureFrame> result;
    result.reserve(length);
    for (std::size_t i = 0; i < length; ++i)
    {
        const double targetTime =
            firstTime + duration * static_cast<double>(i) / static_cast<double>((std::max)(std::size_t{1}, length - 1));
        std::size_t right = 1;
        while (right < frames.size() && frames[right].timeOffsetMs < targetTime)
        {
            ++right;
        }

        const double relativeTime = targetTime - firstTime;
        if (right >= frames.size())
        {
            UnifiedFeatureFrame last = frames.back();
            last.timeOffsetMs = relativeTime;
            result.push_back(last);
            continue;
        }

        const std::size_t left = right > 0 ? right - 1 : 0;
        const UnifiedFeatureFrame& a = frames[left];
        const UnifiedFeatureFrame& b = frames[right];
        const double span = (std::max)(1.0, b.timeOffsetMs - a.timeOffsetMs);
        const float t = static_cast<float>((targetTime - a.timeOffsetMs) / span);
        result.push_back(interpolate(a, b, relativeTime, t));
    }

    return result;
}

std::vector<UnifiedFeatureFrame> buildUnifiedSequence(
    const std::vector<UnifiedFeatureFrame>& frames) noexcept
{
    return resample(frames, kUnifiedSequenceLength);
}

ActiveSegment findActiveSegment(const std::vector<UnifiedFeatureFrame>& frames) noexcept
{
    ActiveSegment result{};
    std::vector<std::size_t> activeIndexes;
    activeIndexes.reserve(frames.size());
    for (std::size_t index = 0; index < frames.size(); ++index)
    {
        if (frames[index].palmVelocity >= kActiveVelocityThreshold)
        {
            activeIndexes.push_back(index);
        }
    }
    if (activeIndexes.size() < kActiveSegmentMinimumActiveFrames)
    {
        return result;
    }

    const std::size_t start = activeIndexes.front() >= kActiveSegmentPadding
        ? activeIndexes.front() - kActiveSegmentPadding
        : 0;
    const std::size_t end =
        (std::min)(frames.size() - 1, activeIndexes.back() + kActiveSegmentPadding);

    double path = 0.0;
    for (std::size_t index = start + 1; index <= end; ++index)
    {
        const float dx = frames[index].centerX - frames[index - 1].centerX;
        const float dy = frames[index].centerY - frames[index - 1].centerY;
        path += std::hypot(dx, dy);
    }
    const double durationSeconds =
        (std::max)(0.001, (frames[end].timeOffsetMs - frames[start].timeOffsetMs) / 1000.0);

    result.startIndex = start;
    result.endIndex = end;
    result.startTimeOffsetMs = frames[start].timeOffsetMs;
    result.endTimeOffsetMs = frames[end].timeOffsetMs;
    result.pathVelocity = static_cast<float>(path / durationSeconds);
    result.valid = true;
    return result;
}

HandTopologySummary summarizeTopology(const std::vector<UnifiedFeatureFrame>& frames) noexcept
{
    HandTopologySummary summary{};
    summary.valid = true;
    summary.sampleCount = frames.size();
    if (frames.empty())
    {
        return summary;
    }

    float palmTravel = 0.0F;
    for (std::size_t index = 1; index < frames.size(); ++index)
    {
        palmTravel += std::hypot(
            frames[index].centerX - frames[index - 1].centerX,
            frames[index].centerY - frames[index - 1].centerY);
    }

    std::vector<float> orientations;
    orientations.reserve(frames.size());
    for (const auto& frame : frames)
    {
        orientations.push_back(frame.palmOrientationRadians);
    }
    const auto unwrapped = unwrapAngles(orientations);
    const auto [orientationMin, orientationMax] =
        std::minmax_element(unwrapped.begin(), unwrapped.end());
    const float orientationRange = *orientationMax - *orientationMin;

    std::vector<float> handednessValues;
    handednessValues.reserve(frames.size());
    for (const auto& frame : frames)
    {
        handednessValues.push_back(frame.values[kHandednessOffset]);
    }
    const auto [handednessMin, handednessMax] =
        std::minmax_element(handednessValues.begin(), handednessValues.end());
    const float handednessRange = *handednessMax - *handednessMin;
    const float handednessMean = std::accumulate(handednessValues.begin(), handednessValues.end(), 0.0F)
        / static_cast<float>(handednessValues.size());

    std::uint32_t transitions = 0;
    for (std::size_t index = 1; index < frames.size(); ++index)
    {
        transitions += static_cast<std::uint32_t>(std::popcount(
            readFingerStateMask(frames[index - 1].values) ^ readFingerStateMask(frames[index].values)));
    }

    float fingerStraightnessRangeMax = 0.0F;
    for (std::size_t finger = 0; finger < kFingerStraightnessLength; ++finger)
    {
        float minValue = frames.front().values[kFingerStraightnessOffset + finger];
        float maxValue = minValue;
        for (const auto& frame : frames)
        {
            const float value = frame.values[kFingerStraightnessOffset + finger];
            minValue = (std::min)(minValue, value);
            maxValue = (std::max)(maxValue, value);
        }
        fingerStraightnessRangeMax = (std::max)(fingerStraightnessRangeMax, maxValue - minValue);
    }

    std::vector<float> signedAreas;
    signedAreas.reserve(frames.size());
    for (const auto& frame : frames)
    {
        signedAreas.push_back(frame.values[kSignedPalmAreaOffset]);
    }
    const auto [areaMin, areaMax] = std::minmax_element(signedAreas.begin(), signedAreas.end());
    const float areaRange = *areaMax - *areaMin;
    const auto areaSignChanges = static_cast<std::uint32_t>(countSignChanges(signedAreas));

    std::vector<float> compressionValues;
    for (const auto& frame : frames)
    {
        const float value = frame.values[kPalmCompressionOffset];
        if (value > 0.0F)
        {
            compressionValues.push_back(value);
        }
    }
    float compressionMin = 0.0F;
    float compressionMax = 0.0F;
    if (!compressionValues.empty())
    {
        const auto [compressionMinIt, compressionMaxIt] =
            std::minmax_element(compressionValues.begin(), compressionValues.end());
        compressionMin = *compressionMinIt;
        compressionMax = *compressionMaxIt;
    }
    const float compressionDrop = compressionMax <= 0.0F
        ? 0.0F
        : std::clamp((compressionMax - compressionMin) / compressionMax, 0.0F, 1.0F);

    float depthMax = frames.front().values[kPalmDepthRangeOffset];
    for (const auto& frame : frames)
    {
        depthMax = (std::max)(depthMax, frame.values[kPalmDepthRangeOffset]);
    }

    std::vector<float> scaleRatios;
    scaleRatios.reserve(frames.size());
    for (const auto& frame : frames)
    {
        scaleRatios.push_back(frame.values[kHandScaleRatioOffset]);
    }
    const auto [scaleMin, scaleMax] = std::minmax_element(scaleRatios.begin(), scaleRatios.end());
    const float scaleRatioRange = *scaleMax - *scaleMin;
    const float scaleRatioDelta = scaleRatios.size() < 2 ? 0.0F : scaleRatios.back() - scaleRatios.front();

    std::vector<float> areaRatios;
    areaRatios.reserve(frames.size());
    for (const auto& frame : frames)
    {
        areaRatios.push_back(frame.values[kBoundingBoxAreaRatioOffset]);
    }
    const auto [areaRatioMin, areaRatioMax] = std::minmax_element(areaRatios.begin(), areaRatios.end());
    const float areaRatioRange = *areaRatioMax - *areaRatioMin;
    const float areaRatioDelta = areaRatios.size() < 2 ? 0.0F : areaRatios.back() - areaRatios.front();

    const float translationDeltaX = frames.back().centerX - frames.front().centerX;
    const float translationDeltaY = frames.back().centerY - frames.front().centerY;
    const float translationDistance = std::hypot(translationDeltaX, translationDeltaY);

    const float palmTurnScore = (areaSignChanges > 0 ? 0.38F : 0.0F)
        + (compressionDrop * 0.42F)
        + std::clamp(areaRange * 8.0F, 0.0F, 0.35F)
        + std::clamp(depthMax * 2.5F, 0.0F, 0.30F);
    const float sizeChangeScore = std::clamp(
        (std::max)(std::abs(scaleRatioDelta), std::abs(areaRatioDelta) * 0.5F), 0.0F, 1.2F);
    const float changeScore = palmTravel
        + (orientationRange * 0.6F)
        + (handednessRange * 0.8F)
        + (static_cast<float>(transitions) * 0.15F)
        + (fingerStraightnessRangeMax * 0.75F)
        + palmTurnScore
        + (sizeChangeScore * 0.55F);

    summary.startFingerStateMask = readFingerStateMask(frames.front().values);
    summary.endFingerStateMask = readFingerStateMask(frames.back().values);
    summary.fingerStateTransitionCount = transitions;
    summary.palmTravel = palmTravel;
    summary.palmOrientationRangeRadians = orientationRange;
    summary.handednessRange = handednessRange;
    summary.handednessMean = handednessMean;
    summary.fingerStraightnessRangeMax = fingerStraightnessRangeMax;
    summary.signedPalmAreaRange = areaRange;
    summary.signedPalmAreaSignChanges = areaSignChanges;
    summary.palmCompressionMin = compressionMin;
    summary.palmCompressionMax = compressionMax;
    summary.palmCompressionDrop = compressionDrop;
    summary.palmDepthRangeMax = depthMax;
    summary.palmTurnScore = palmTurnScore;
    summary.handScaleRatioRange = scaleRatioRange;
    summary.handScaleRatioDelta = scaleRatioDelta;
    summary.boundingBoxAreaRatioRange = areaRatioRange;
    summary.boundingBoxAreaRatioDelta = areaRatioDelta;
    summary.translationDeltaX = translationDeltaX;
    summary.translationDeltaY = translationDeltaY;
    summary.translationDistance = translationDistance;
    summary.topologyChangeScore = changeScore;
    summary.firstFrameId = 0;
    summary.lastFrameId = 0;
    summary.durationUs = frames.size() < 2
        ? 0
        : static_cast<std::uint64_t>(
              (std::max)(0.0, frames.back().timeOffsetMs - frames.front().timeOffsetMs) * 1000.0);
    return summary;
}

MotionSummary summarizeMotion(const std::vector<UnifiedFeatureFrame>& frames, const float palmTravel) noexcept
{
    MotionSummary summary{};
    if (frames.size() < 2)
    {
        return summary;
    }

    float velocitySum = 0.0F;
    float peak = 0.0F;
    for (std::size_t index = 1; index < frames.size(); ++index)
    {
        velocitySum += frames[index].palmVelocity;
        peak = (std::max)(peak, frames[index].palmVelocity);
    }
    const float averageVelocity = velocitySum / static_cast<float>(frames.size() - 1);
    const double durationMs = (std::max)(0.0, frames.back().timeOffsetMs - frames.front().timeOffsetMs);
    const auto durationSeconds = static_cast<float>((std::max)(0.001, durationMs / 1000.0));

    summary.averagePalmVelocity = averageVelocity;
    summary.peakPalmVelocity = peak;
    summary.motionScore = (std::max)(palmTravel / durationSeconds, averageVelocity);
    return summary;
}

GestureVisualizationSequence buildGestureVisualizationSequence(
    const std::vector<UnifiedFeatureObservation>& observations) noexcept
{
    GestureVisualizationSequence result{};
    if (observations.empty()) return result;

    float averageScale = 0.0F;
    for (const auto& observation : observations)
    {
        averageScale += distance2d(observation.imageLandmarks[0], observation.imageLandmarks[9]);
    }
    averageScale = (std::max)(1.0F, averageScale / static_cast<float>(observations.size()));

    const auto centerOf = [](const UnifiedFeatureObservation& observation) noexcept
    {
        return Landmark3f{
            0.5F * (observation.imageLandmarks[0].x + observation.imageLandmarks[9].x),
            0.5F * (observation.imageLandmarks[0].y + observation.imageLandmarks[9].y),
            0.5F * (observation.imageLandmarks[0].z + observation.imageLandmarks[9].z)};
    };
    const auto firstCenter = centerOf(observations.front());
    const auto firstTimestampUs = observations.front().timestampUs;
    std::vector<GestureVisualizationFrame> source;
    source.reserve(observations.size());
    for (const auto& observation : observations)
    {
        GestureVisualizationFrame frame{};
        frame.timeOffsetMs = observation.timestampUs >= firstTimestampUs
            ? static_cast<double>(observation.timestampUs - firstTimestampUs) / 1000.0 : 0.0;
        const auto center = centerOf(observation);
        frame.centerX = (center.x - firstCenter.x) / averageScale;
        frame.centerY = (center.y - firstCenter.y) / averageScale;
        for (std::size_t pointIndex = 0; pointIndex < kLandmarkCount; ++pointIndex)
        {
            const auto& point = observation.imageLandmarks[pointIndex];
            frame.skeleton[pointIndex] = Landmark3f{
                (point.x - firstCenter.x) / averageScale,
                (point.y - firstCenter.y) / averageScale,
                point.z / averageScale};
        }
        source.push_back(frame);
    }

    if (source.size() == 1)
    {
        result.frames.fill(source.front());
        result.valid = true;
        return result;
    }

    const double durationMs = (std::max)(1.0, source.back().timeOffsetMs);
    std::size_t right = 1;
    for (std::size_t outputIndex = 0; outputIndex < kUnifiedSequenceLength; ++outputIndex)
    {
        const double targetTime = durationMs * static_cast<double>(outputIndex)
            / static_cast<double>(kUnifiedSequenceLength - 1);
        while (right < source.size() && source[right].timeOffsetMs < targetTime) ++right;
        if (right >= source.size())
        {
            result.frames[outputIndex] = source.back();
            result.frames[outputIndex].timeOffsetMs = targetTime;
            continue;
        }
        const std::size_t left = right - 1;
        const double span = (std::max)(1.0, source[right].timeOffsetMs - source[left].timeOffsetMs);
        const float t = static_cast<float>((targetTime - source[left].timeOffsetMs) / span);
        auto& output = result.frames[outputIndex];
        output.timeOffsetMs = targetTime;
        output.centerX = lerp(source[left].centerX, source[right].centerX, t);
        output.centerY = lerp(source[left].centerY, source[right].centerY, t);
        for (std::size_t pointIndex = 0; pointIndex < kLandmarkCount; ++pointIndex)
        {
            output.skeleton[pointIndex] = Landmark3f{
                lerp(source[left].skeleton[pointIndex].x, source[right].skeleton[pointIndex].x, t),
                lerp(source[left].skeleton[pointIndex].y, source[right].skeleton[pointIndex].y, t),
                lerp(source[left].skeleton[pointIndex].z, source[right].skeleton[pointIndex].z, t)};
        }
    }
    result.valid = true;
    return result;
}
}

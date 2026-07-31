#include "HandInput/Recognition/unified_sequence_gesture_comparator.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <utility>

namespace ryoiki::hand_input::recognition
{
namespace
{
using measurements::HandTopologySummary;
using measurements::kBoundingBoxAreaRatioOffset;
using measurements::kFingerCount;
using measurements::kHandScaleRatioOffset;
using measurements::kLandmarkCount;
using measurements::kPalmCompressionOffset;
using measurements::kPalmDepthRangeOffset;
using measurements::kSignedPalmAreaOffset;
using measurements::kUnifiedFeatureVectorLength;
using measurements::readFingerStateMask;
using measurements::readFingerStraightness;
using measurements::UnifiedFeatureFrame;

std::string formatFixed(const double value, const int precision)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

std::string formatFingerMask(const std::uint32_t mask)
{
    std::string text(kFingerCount, '0');
    for (std::size_t i = 0; i < kFingerCount; ++i)
    {
        if ((mask & (1U << i)) != 0)
        {
            text[i] = '1';
        }
    }
    return text;
}

// Mirrors C#'s MathF.Sign: -1, 0, or 1.
int mathSign(const float value) noexcept
{
    if (value > 0.0F)
    {
        return 1;
    }
    if (value < 0.0F)
    {
        return -1;
    }
    return 0;
}

int fingerMaskDistance(
    const std::uint32_t a, const std::uint32_t b, const std::uint32_t strictFingerMask) noexcept
{
    return std::popcount((a ^ b) & strictFingerMask);
}

float directionDot(
    const float candidateX,
    const float candidateY,
    const float templateX,
    const float templateY) noexcept
{
    const float candidateMagnitude = std::hypot(candidateX, candidateY);
    const float templateMagnitude = std::hypot(templateX, templateY);
    if (candidateMagnitude < 0.001F || templateMagnitude < 0.001F)
    {
        return 1.0F;
    }
    return ((candidateX * templateX) + (candidateY * templateY))
        / (std::max)(0.001F, candidateMagnitude * templateMagnitude);
}

// --- UnifiedFeatureFrame flat-vector readers -------------------------------
// The PR's ReadSignedPalmArea/ReadPalmCompression/ReadPalmDepthRange/
// ReadHandScaleRatio/ReadBoundingBoxAreaRatio fall back to a parallel
// GestureFrameFeatures record when the flat Values array is too short.
// hand_unified_feature_frame.h documents that this module intentionally does
// not carry that parallel record (UnifiedFeatureFrame::values is always
// exactly kUnifiedFeatureVectorLength), so these readers only need the flat-
// vector path.
float readSignedPalmArea(const UnifiedFeatureFrame& frame) noexcept
{
    return frame.values[kSignedPalmAreaOffset];
}

float readPalmCompression(const UnifiedFeatureFrame& frame) noexcept
{
    return frame.values[kPalmCompressionOffset];
}

float readPalmDepthRange(const UnifiedFeatureFrame& frame) noexcept
{
    return frame.values[kPalmDepthRangeOffset];
}

float readHandScaleRatio(const UnifiedFeatureFrame& frame) noexcept
{
    return frame.values[kHandScaleRatioOffset];
}

float readBoundingBoxAreaRatio(const UnifiedFeatureFrame& frame) noexcept
{
    return frame.values[kBoundingBoxAreaRatioOffset];
}

// --- FeatureFrameDistance components ---------------------------------------

float pairDistance(
    const std::array<float, kUnifiedFeatureVectorLength>& candidate,
    const std::array<float, kUnifiedFeatureVectorLength>& templateValues,
    const std::size_t offset,
    const std::size_t length) noexcept
{
    const std::size_t end = (std::min)(candidate.size(), offset + length);
    float sum = 0.0F;
    std::size_t terms = 0;
    for (std::size_t i = offset; i + 1 < end; i += 2)
    {
        const float dx = candidate[i] - templateValues[i];
        const float dy = candidate[i + 1] - templateValues[i + 1];
        sum += std::hypot(dx, dy);
        ++terms;
    }
    return terms == 0 ? 0.0F : sum / static_cast<float>(terms);
}

float scalarDistance(
    const std::array<float, kUnifiedFeatureVectorLength>& candidate,
    const std::array<float, kUnifiedFeatureVectorLength>& templateValues,
    const std::size_t offset,
    const std::size_t length) noexcept
{
    const std::size_t end = (std::min)(candidate.size(), offset + length);
    float sum = 0.0F;
    std::size_t terms = 0;
    for (std::size_t i = offset; i < end; ++i)
    {
        sum += std::abs(candidate[i] - templateValues[i]);
        ++terms;
    }
    return terms == 0 ? 0.0F : sum / static_cast<float>(terms);
}

float fingerStateDistance(
    const UnifiedFeatureFrame& candidate, const UnifiedFeatureFrame& templateFrame) noexcept
{
    const auto candidateMask = readFingerStateMask(candidate.values);
    const auto templateMask = readFingerStateMask(templateFrame.values);
    if (candidateMask != 0 || templateMask != 0)
    {
        int mismatches = 0;
        for (std::size_t i = 0; i < kFingerCount; ++i)
        {
            const bool candidateOpen = (candidateMask & (1U << i)) != 0;
            const bool templateOpen = (templateMask & (1U << i)) != 0;
            if (candidateOpen != templateOpen)
            {
                ++mismatches;
            }
        }
        return static_cast<float>(mismatches) / static_cast<float>(kFingerCount);
    }

    // Both masks are exactly zero (every finger reads below the open
    // threshold): the PR still compares continuous straightness rather than
    // trivially declaring a perfect match, so a closed-fist-vs-closed-fist
    // comparison still discriminates on curl depth. maskMismatches is
    // provably 0 here (both masks are 0), matching the PR's redundant
    // recomputation exactly.
    const float straightnessDistance = scalarDistance(
        candidate.values, templateFrame.values,
        measurements::kFingerStraightnessOffset, measurements::kFingerStraightnessLength);
    return (std::max)(straightnessDistance, 0.0F);
}

float boneDistance(
    const std::array<float, kUnifiedFeatureVectorLength>& candidate,
    const std::array<float, kUnifiedFeatureVectorLength>& templateValues) noexcept
{
    static constexpr std::array<std::pair<int, int>, 21> kConnections{{
        {0, 1}, {1, 2}, {2, 3}, {3, 4},
        {0, 5}, {5, 6}, {6, 7}, {7, 8},
        {5, 9}, {9, 10}, {10, 11}, {11, 12},
        {9, 13}, {13, 14}, {14, 15}, {15, 16},
        {13, 17}, {17, 18}, {18, 19}, {19, 20},
        {0, 17}}};

    float sum = 0.0F;
    std::size_t terms = 0;
    for (const auto& connection : kConnections)
    {
        const auto a = static_cast<std::size_t>(connection.first) * 2;
        const auto b = static_cast<std::size_t>(connection.second) * 2;
        if (b + 1 >= candidate.size() || b + 1 >= templateValues.size())
        {
            continue;
        }
        const float cdx = candidate[b] - candidate[a];
        const float cdy = candidate[b + 1] - candidate[a + 1];
        const float tdx = templateValues[b] - templateValues[a];
        const float tdy = templateValues[b + 1] - templateValues[a + 1];
        sum += std::hypot(cdx - tdx, cdy - tdy);
        ++terms;
    }
    return terms == 0 ? 0.0F : sum / static_cast<float>(terms);
}

float palmTurnDistance(
    const UnifiedFeatureFrame& candidate, const UnifiedFeatureFrame& templateFrame) noexcept
{
    const float area = std::abs(readSignedPalmArea(candidate) - readSignedPalmArea(templateFrame)) * 4.0F;
    const float compression =
        std::abs(readPalmCompression(candidate) - readPalmCompression(templateFrame));
    return (area * 0.55F) + (compression * 0.45F);
}

float sizeDistance(
    const UnifiedFeatureFrame& candidate, const UnifiedFeatureFrame& templateFrame) noexcept
{
    const float scale =
        std::abs(readHandScaleRatio(candidate) - readHandScaleRatio(templateFrame));
    const float area = std::abs(
        readBoundingBoxAreaRatio(candidate) - readBoundingBoxAreaRatio(templateFrame));
    return scale + (std::clamp(area, 0.0F, 3.0F) * 0.35F);
}

ScoreBreakdown emptyBreakdown(const float total) noexcept
{
    ScoreBreakdown breakdown{};
    breakdown.totalScore = total;
    return breakdown;
}

// --- CompareUnified pre-gates -----------------------------------------------

float angleDelta(const float from, const float to) noexcept
{
    constexpr float kPi = 3.14159265358979323846F;
    constexpr float kTwoPi = kPi * 2.0F;
    float delta = to - from;
    while (delta > kPi)
    {
        delta -= kTwoPi;
    }
    while (delta < -kPi)
    {
        delta += kTwoPi;
    }
    return delta;
}

void accumulateDelta(
    const float candidateDelta,
    const float templateDelta,
    const float weight,
    float& dot,
    float& candidateEnergy,
    float& templateEnergy) noexcept
{
    const float weightedCandidate = candidateDelta * weight;
    const float weightedTemplate = templateDelta * weight;
    dot += weightedCandidate * weightedTemplate;
    candidateEnergy += weightedCandidate * weightedCandidate;
    templateEnergy += weightedTemplate * weightedTemplate;
}

std::optional<float> temporalDirectionDot(
    const std::vector<UnifiedFeatureFrame>& candidateFrames,
    const std::vector<UnifiedFeatureFrame>& templateFrames) noexcept
{
    const std::size_t count = (std::min)(candidateFrames.size(), templateFrames.size());
    if (count < 3)
    {
        return std::nullopt;
    }

    float dot = 0.0F;
    float candidateEnergy = 0.0F;
    float templateEnergy = 0.0F;
    for (std::size_t i = 1; i < count; ++i)
    {
        accumulateDelta(
            candidateFrames[i].centerX - candidateFrames[i - 1].centerX,
            templateFrames[i].centerX - templateFrames[i - 1].centerX,
            0.8F, dot, candidateEnergy, templateEnergy);
        accumulateDelta(
            candidateFrames[i].centerY - candidateFrames[i - 1].centerY,
            templateFrames[i].centerY - templateFrames[i - 1].centerY,
            0.8F, dot, candidateEnergy, templateEnergy);
        accumulateDelta(
            angleDelta(candidateFrames[i - 1].palmOrientationRadians, candidateFrames[i].palmOrientationRadians),
            angleDelta(templateFrames[i - 1].palmOrientationRadians, templateFrames[i].palmOrientationRadians),
            0.35F, dot, candidateEnergy, templateEnergy);
        accumulateDelta(
            readSignedPalmArea(candidateFrames[i]) - readSignedPalmArea(candidateFrames[i - 1]),
            readSignedPalmArea(templateFrames[i]) - readSignedPalmArea(templateFrames[i - 1]),
            0.55F, dot, candidateEnergy, templateEnergy);
        accumulateDelta(
            readPalmCompression(candidateFrames[i]) - readPalmCompression(candidateFrames[i - 1]),
            readPalmCompression(templateFrames[i]) - readPalmCompression(templateFrames[i - 1]),
            0.65F, dot, candidateEnergy, templateEnergy);
        accumulateDelta(
            readPalmDepthRange(candidateFrames[i]) - readPalmDepthRange(candidateFrames[i - 1]),
            readPalmDepthRange(templateFrames[i]) - readPalmDepthRange(templateFrames[i - 1]),
            0.45F, dot, candidateEnergy, templateEnergy);
        accumulateDelta(
            readHandScaleRatio(candidateFrames[i]) - readHandScaleRatio(candidateFrames[i - 1]),
            readHandScaleRatio(templateFrames[i]) - readHandScaleRatio(templateFrames[i - 1]),
            0.55F, dot, candidateEnergy, templateEnergy);

        const auto candidateStraightness = readFingerStraightness(candidateFrames[i].values);
        const auto previousCandidateStraightness = readFingerStraightness(candidateFrames[i - 1].values);
        const auto templateStraightness = readFingerStraightness(templateFrames[i].values);
        const auto previousTemplateStraightness = readFingerStraightness(templateFrames[i - 1].values);
        for (std::size_t finger = 0; finger < kFingerCount; ++finger)
        {
            accumulateDelta(
                candidateStraightness[finger] - previousCandidateStraightness[finger],
                templateStraightness[finger] - previousTemplateStraightness[finger],
                0.9F, dot, candidateEnergy, templateEnergy);
        }
    }

    if (candidateEnergy < 0.02F || templateEnergy < 0.02F)
    {
        return std::nullopt;
    }
    return dot / std::sqrt(candidateEnergy * templateEnergy);
}

bool hasOppositeMotionDirection(
    const std::vector<UnifiedFeatureFrame>& candidateFrames,
    const std::vector<UnifiedFeatureFrame>& templateFrames) noexcept
{
    const float candidateDx = candidateFrames.back().centerX - candidateFrames.front().centerX;
    const float candidateDy = candidateFrames.back().centerY - candidateFrames.front().centerY;
    const float templateDx = templateFrames.back().centerX - templateFrames.front().centerX;
    const float templateDy = templateFrames.back().centerY - templateFrames.front().centerY;
    const float candidateMagnitude = std::hypot(candidateDx, candidateDy);
    const float templateMagnitude = std::hypot(templateDx, templateDy);
    if (candidateMagnitude < 0.18F || templateMagnitude < 0.18F)
    {
        return false;
    }
    const float dot = ((candidateDx * templateDx) + (candidateDy * templateDy))
        / (std::max)(0.001F, candidateMagnitude * templateMagnitude);
    return dot < -0.25F;
}

std::vector<UnifiedFeatureFrame> reverseFrames(
    const std::vector<UnifiedFeatureFrame>& frames)
{
    return {frames.rbegin(), frames.rend()};
}
}

std::string topologyRejectionReason(
    const measurements::HandTopologySummary& candidate,
    const measurements::HandTopologySummary& templateSummary) noexcept
{
    if (!candidate.valid || !templateSummary.valid
        || templateSummary.topologyChangeScore < kMinimumTopologyChangeScore)
    {
        return {};
    }

    if (templateSummary.palmTurnScore >= kMinimumPalmTurnScore)
    {
        if (candidate.startFingerStateMask != templateSummary.startFingerStateMask
            || candidate.endFingerStateMask != templateSummary.endFingerStateMask)
        {
            return "Finger state mismatch candidate "
                + formatFingerMask(candidate.startFingerStateMask) + "->"
                + formatFingerMask(candidate.endFingerStateMask) + " template "
                + formatFingerMask(templateSummary.startFingerStateMask) + "->"
                + formatFingerMask(templateSummary.endFingerStateMask);
        }

        if (candidate.palmTurnScore < templateSummary.palmTurnScore * 0.55F)
        {
            return "Palm turn too weak " + formatFixed(candidate.palmTurnScore, 3)
                + " < template " + formatFixed(templateSummary.palmTurnScore, 3);
        }

        if (templateSummary.signedPalmAreaSignChanges > 0 && candidate.signedPalmAreaSignChanges == 0)
        {
            return "Palm area did not cross sides";
        }

        if (templateSummary.palmCompressionDrop >= kMinimumPalmCompressionDrop
            && candidate.palmCompressionDrop < templateSummary.palmCompressionDrop * 0.55F)
        {
            return "Palm compression too small " + formatFixed(candidate.palmCompressionDrop, 3)
                + " < template " + formatFixed(templateSummary.palmCompressionDrop, 3);
        }
    }

    if (templateSummary.fingerStateTransitionCount > 0
        && templateSummary.startFingerStateMask != templateSummary.endFingerStateMask)
    {
        if (candidate.startFingerStateMask == candidate.endFingerStateMask)
        {
            return "Finger final state did not change candidate "
                + formatFingerMask(candidate.startFingerStateMask) + "->"
                + formatFingerMask(candidate.endFingerStateMask) + " template "
                + formatFingerMask(templateSummary.startFingerStateMask) + "->"
                + formatFingerMask(templateSummary.endFingerStateMask);
        }

        const int strictStartDistance = fingerMaskDistance(
            candidate.startFingerStateMask, templateSummary.startFingerStateMask, 0b00011U);
        const int strictEndDistance = fingerMaskDistance(
            candidate.endFingerStateMask, templateSummary.endFingerStateMask, 0b00011U);
        if (strictStartDistance > 0 || strictEndDistance > 0)
        {
            return "Finger 0/1 state mismatch candidate "
                + formatFingerMask(candidate.startFingerStateMask) + "->"
                + formatFingerMask(candidate.endFingerStateMask) + " template "
                + formatFingerMask(templateSummary.startFingerStateMask) + "->"
                + formatFingerMask(templateSummary.endFingerStateMask);
        }

        const int startDistance = fingerMaskDistance(
            candidate.startFingerStateMask, templateSummary.startFingerStateMask, 0b11100U);
        const int endDistance = fingerMaskDistance(
            candidate.endFingerStateMask, templateSummary.endFingerStateMask, 0b11100U);
        if (startDistance > 1 || endDistance > 1)
        {
            return "Finger state mismatch candidate "
                + formatFingerMask(candidate.startFingerStateMask) + "->"
                + formatFingerMask(candidate.endFingerStateMask) + " template "
                + formatFingerMask(templateSummary.startFingerStateMask) + "->"
                + formatFingerMask(templateSummary.endFingerStateMask);
        }
    }

    if (templateSummary.handScaleRatioRange >= kMinimumHandScaleRatioRange)
    {
        if (candidate.handScaleRatioRange < templateSummary.handScaleRatioRange * 0.55F)
        {
            return "Hand size change too small " + formatFixed(candidate.handScaleRatioRange, 3)
                + " < template " + formatFixed(templateSummary.handScaleRatioRange, 3);
        }

        if (std::abs(templateSummary.handScaleRatioDelta) >= kMinimumHandScaleRatioRange
            && mathSign(candidate.handScaleRatioDelta) != mathSign(templateSummary.handScaleRatioDelta))
        {
            return "Hand size moved opposite direction candidate "
                + formatFixed(candidate.handScaleRatioDelta, 3) + " template "
                + formatFixed(templateSummary.handScaleRatioDelta, 3);
        }
    }

    if (templateSummary.translationDistance >= kMinimumTranslationDistance)
    {
        if (candidate.translationDistance < templateSummary.translationDistance * 0.45F)
        {
            return "Translation too small " + formatFixed(candidate.translationDistance, 3)
                + " < template " + formatFixed(templateSummary.translationDistance, 3);
        }

        const float dot = directionDot(
            candidate.translationDeltaX, candidate.translationDeltaY,
            templateSummary.translationDeltaX, templateSummary.translationDeltaY);
        if (dot < -0.25F)
        {
            return "Translation moved opposite direction dot=" + formatFixed(dot, 3);
        }
    }

    if (candidate.topologyChangeScore < templateSummary.topologyChangeScore * 0.55F)
    {
        return "Topology change too small " + formatFixed(candidate.topologyChangeScore, 3)
            + " < template " + formatFixed(templateSummary.topologyChangeScore, 3);
    }

    if (templateSummary.palmTravel >= kMinimumPalmTravel
        && candidate.palmTravel < templateSummary.palmTravel * 0.45F)
    {
        return "Palm travel too small " + formatFixed(candidate.palmTravel, 3)
            + " < template " + formatFixed(templateSummary.palmTravel, 3);
    }

    if (templateSummary.palmOrientationRangeRadians >= kMinimumPalmOrientationRangeRadians
        && candidate.palmOrientationRangeRadians < templateSummary.palmOrientationRangeRadians * 0.50F)
    {
        constexpr float kRadiansToDegrees = 180.0F / 3.14159265358979323846F;
        return "Palm angle change too small "
            + formatFixed(candidate.palmOrientationRangeRadians * kRadiansToDegrees, 1) + " deg < template "
            + formatFixed(templateSummary.palmOrientationRangeRadians * kRadiansToDegrees, 1) + " deg";
    }

    if (templateSummary.fingerStateTransitionCount > 0 && candidate.fingerStateTransitionCount == 0)
    {
        return "Finger topology did not change";
    }

    return {};
}

ScoreBreakdown featureFrameDistance(
    const measurements::UnifiedFeatureFrame& candidate,
    const measurements::UnifiedFeatureFrame& templateFrame) noexcept
{
    constexpr std::size_t jointOffset = 0;
    constexpr std::size_t jointLength = kLandmarkCount * 2;
    constexpr std::size_t curlOffset = jointOffset + jointLength;
    constexpr std::size_t curlLength = kFingerCount;
    constexpr std::size_t spacingOffset = curlOffset + curlLength;
    constexpr std::size_t spacingLength = kFingerCount - 1;

    const float joint = pairDistance(candidate.values, templateFrame.values, jointOffset, jointLength);
    const float bone = boneDistance(candidate.values, templateFrame.values);
    const float curl = scalarDistance(candidate.values, templateFrame.values, curlOffset, curlLength);
    const float fingerState = fingerStateDistance(candidate, templateFrame);
    const float spacing = scalarDistance(candidate.values, templateFrame.values, spacingOffset, spacingLength);
    const float palmTurn = palmTurnDistance(candidate, templateFrame);
    const float depth = std::abs(readPalmDepthRange(candidate) - readPalmDepthRange(templateFrame));
    constexpr float handedness = 0.0F; // Dead in the PR: preserved, see header note.
    const float size = sizeDistance(candidate, templateFrame);
    const float velocity = candidate.palmVelocity - templateFrame.palmVelocity;
    constexpr float translation = 0.0F; // Dead in the PR: preserved, see header note.
    const float motion = std::abs(velocity) * 0.12F;

    ScoreBreakdown breakdown{};
    breakdown.jointScore = joint;
    breakdown.boneScore = bone;
    breakdown.curlScore = curl;
    breakdown.fingerStateScore = fingerState;
    breakdown.spacingScore = spacing;
    breakdown.motionScore = motion;
    breakdown.palmTurnScore = palmTurn;
    breakdown.depthScore = depth;
    breakdown.handednessScore = handedness;
    breakdown.sizeScore = size;
    breakdown.translationScore = translation;
    breakdown.totalScore = (joint * 0.18F)
        + (bone * 0.12F)
        + (curl * 0.08F)
        + (fingerState * 0.20F)
        + (spacing * 0.04F)
        + (motion * 0.10F)
        + (palmTurn * 0.12F)
        + (depth * 0.04F)
        + (handedness * 0.06F)
        + (size * 0.04F)
        + (translation * 0.02F);
    return breakdown;
}

DtwResult boundedDtw(
    const std::vector<measurements::UnifiedFeatureFrame>& candidate,
    const std::vector<measurements::UnifiedFeatureFrame>& templateFrames,
    const std::size_t bandRadius) noexcept
{
    const std::size_t n = candidate.size();
    const std::size_t m = templateFrames.size();
    if (n == 0 || m == 0)
    {
        return DtwResult{
            (std::numeric_limits<float>::max)(), {}, emptyBreakdown((std::numeric_limits<float>::max)())};
    }

    const std::size_t nmDiff = n > m ? n - m : m - n;
    const std::size_t radius = (std::max)(bandRadius, nmDiff);
    const auto index = [m](const std::size_t i, const std::size_t j) noexcept
    {
        return (i * (m + 1)) + j;
    };
    std::vector<float> costs((n + 1) * (m + 1), (std::numeric_limits<float>::infinity)());
    costs[index(0, 0)] = 0.0F;

    for (std::size_t i = 1; i <= n; ++i)
    {
        const std::size_t start = i > radius ? i - radius : 1;
        const std::size_t end = (std::min)(m, i + radius);
        for (std::size_t j = start; j <= end; ++j)
        {
            const float distance = featureFrameDistance(candidate[i - 1], templateFrames[j - 1]).totalScore;
            const float previous = (std::min)(
                costs[index(i - 1, j)], (std::min)(costs[index(i, j - 1)], costs[index(i - 1, j - 1)]));
            costs[index(i, j)] = distance + previous;
        }
    }

    if (!std::isfinite(costs[index(n, m)]))
    {
        return DtwResult{
            (std::numeric_limits<float>::max)(), {}, emptyBreakdown((std::numeric_limits<float>::max)())};
    }

    std::vector<DtwPoint> path;
    std::size_t x = n;
    std::size_t y = m;
    while (x > 0 && y > 0)
    {
        path.push_back(DtwPoint{x - 1, y - 1});
        const float diagonal = costs[index(x - 1, y - 1)];
        const float up = costs[index(x - 1, y)];
        const float left = costs[index(x, y - 1)];
        if (diagonal <= up && diagonal <= left)
        {
            --x;
            --y;
        }
        else if (up <= left)
        {
            --x;
        }
        else
        {
            --y;
        }
    }
    std::reverse(path.begin(), path.end());

    ScoreBreakdown accumulated{};
    for (const auto& point : path)
    {
        const auto step = featureFrameDistance(candidate[point.candidateIndex], templateFrames[point.templateIndex]);
        accumulated.jointScore += step.jointScore;
        accumulated.boneScore += step.boneScore;
        accumulated.curlScore += step.curlScore;
        accumulated.fingerStateScore += step.fingerStateScore;
        accumulated.spacingScore += step.spacingScore;
        accumulated.motionScore += step.motionScore;
        accumulated.palmTurnScore += step.palmTurnScore;
        accumulated.depthScore += step.depthScore;
        accumulated.handednessScore += step.handednessScore;
        accumulated.sizeScore += step.sizeScore;
        accumulated.translationScore += step.translationScore;
        accumulated.totalScore += step.totalScore;
    }
    const auto count = static_cast<float>(path.size());
    ScoreBreakdown breakdown{};
    breakdown.jointScore = accumulated.jointScore / count;
    breakdown.boneScore = accumulated.boneScore / count;
    breakdown.curlScore = accumulated.curlScore / count;
    breakdown.fingerStateScore = accumulated.fingerStateScore / count;
    breakdown.spacingScore = accumulated.spacingScore / count;
    breakdown.motionScore = accumulated.motionScore / count;
    breakdown.palmTurnScore = accumulated.palmTurnScore / count;
    breakdown.depthScore = accumulated.depthScore / count;
    breakdown.handednessScore = accumulated.handednessScore / count;
    breakdown.sizeScore = accumulated.sizeScore / count;
    breakdown.translationScore = accumulated.translationScore / count;
    breakdown.totalScore = accumulated.totalScore / count;

    const float score = costs[index(n, m)] / static_cast<float>((std::max)(std::size_t{1}, path.size()));
    return DtwResult{score, std::move(path), breakdown};
}

float scoreToConfidence(const float score) noexcept
{
    return std::clamp(1.0F - (score / 0.9F), 0.0F, 1.0F);
}

float confidenceToScore(const float confidence) noexcept
{
    return (1.0F - std::clamp(confidence, 0.0F, 1.0F)) * 0.9F;
}

ComparisonResult compareUnifiedSequences(
    const std::vector<measurements::UnifiedFeatureFrame>& candidateFrames,
    const std::vector<measurements::UnifiedFeatureFrame>& templateFrames) noexcept
{
    if (candidateFrames.size() < 3 || templateFrames.size() < 3)
    {
        ComparisonResult result{};
        result.score = (std::numeric_limits<float>::max)();
        result.eligible = false;
        result.reason = "Too few feature frames";
        return result;
    }

    const auto directionDotValue = temporalDirectionDot(candidateFrames, templateFrames);
    if (directionDotValue.has_value() && *directionDotValue < -0.18F)
    {
        ComparisonResult result{};
        result.score = (std::numeric_limits<float>::max)();
        result.eligible = false;
        result.reason = "Opposite time direction dot=" + formatFixed(*directionDotValue, 3);
        return result;
    }

    if (hasOppositeMotionDirection(candidateFrames, templateFrames))
    {
        ComparisonResult result{};
        result.score = (std::numeric_limits<float>::max)();
        result.eligible = false;
        result.reason = "Opposite motion direction";
        return result;
    }

    const std::size_t bandRadius = (std::max)(std::size_t{5}, candidateFrames.size() / 4);
    const auto dtw = boundedDtw(candidateFrames, templateFrames, bandRadius);
    const auto reversedTemplate = reverseFrames(templateFrames);
    const auto reversedDtw = boundedDtw(candidateFrames, reversedTemplate, bandRadius);
    if (reversedDtw.score <= dtw.score * kReverseDtwMargin)
    {
        ComparisonResult result{};
        result.score = dtw.score;
        result.eligible = false;
        result.reason = "Reversed time direction fits better forward=" + formatFixed(dtw.score, 3)
            + " reverse=" + formatFixed(reversedDtw.score, 3);
        result.path = dtw.path;
        result.breakdown = dtw.breakdown;
        result.reverseScore = reversedDtw.score;
        result.hasReverseScore = true;
        return result;
    }

    const float warpRatio = static_cast<float>(dtw.path.size())
        / static_cast<float>((std::max)(candidateFrames.size(), templateFrames.size()));
    if (warpRatio > kWarpRatioLimit)
    {
        ComparisonResult result{};
        result.score = dtw.score;
        result.eligible = false;
        result.reason = "Excessive DTW warp " + formatFixed(warpRatio, 2);
        result.warpRatio = warpRatio;
        result.hasWarpRatio = true;
        result.path = dtw.path;
        result.breakdown = dtw.breakdown;
        result.reverseScore = reversedDtw.score;
        result.hasReverseScore = true;
        return result;
    }

    const float thresholdScore = confidenceToScore(kConfidenceThreshold);
    const bool eligible = dtw.score <= thresholdScore;
    ComparisonResult result{};
    result.score = dtw.score;
    result.eligible = eligible;
    result.reason = eligible ? "Eligible" : "Below unified threshold";
    result.warpRatio = warpRatio;
    result.hasWarpRatio = true;
    result.path = dtw.path;
    result.breakdown = dtw.breakdown;
    result.reverseScore = reversedDtw.score;
    result.hasReverseScore = true;
    return result;
}

RecognitionOutcome compareCandidateToTemplate(
    const measurements::GestureCandidateWindow& candidate,
    const UnifiedSequenceTemplate& templateSequence) noexcept
{
    const auto reason = topologyRejectionReason(candidate.topology, templateSequence.topology);
    if (!reason.empty())
    {
        ComparisonResult rejected{};
        rejected.score = (std::numeric_limits<float>::max)();
        rejected.eligible = false;
        rejected.reason = reason;
        RecognitionOutcome outcome{};
        outcome.topologyRejectionReason = reason;
        outcome.comparison = rejected;
        return outcome;
    }

    const std::vector<measurements::UnifiedFeatureFrame> candidateFrames(
        candidate.unifiedSequence.begin(), candidate.unifiedSequence.end());
    const std::vector<measurements::UnifiedFeatureFrame> templateFrames(
        templateSequence.frames.begin(), templateSequence.frames.end());

    RecognitionOutcome outcome{};
    outcome.comparison = compareUnifiedSequences(candidateFrames, templateFrames);
    return outcome;
}
}

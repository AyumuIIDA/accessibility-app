#pragma once

#include "HandInput/Measurements/hand_unified_feature_frame.h"

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace ryoiki::hand_input::recognition
{
inline constexpr float kTwoHandMinimumConfidence = 0.35F;
inline constexpr float kTwoHandMinimumCoverageRatio = 0.70F;
inline constexpr float kTwoHandMinimumRelativeDistance = 0.65F;
inline constexpr std::size_t kTwoHandFeatureLength =
    measurements::kUnifiedFeatureVectorLength * 2 + 4;

struct TwoHandFrameSetObservation
{
    std::uint64_t timestampUs{0};
    std::array<measurements::UnifiedFeatureObservation, 2> hands{};
    std::array<float, 2> confidences{};
    std::size_t handCount{0};
};

class TwoHandFrameSetBuffer final
{
public:
    void push(TwoHandFrameSetObservation observation);
    void reset() noexcept;
    [[nodiscard]] std::vector<TwoHandFrameSetObservation> window(
        std::uint64_t endTimestampUs, std::uint64_t durationUs) const;
    [[nodiscard]] std::size_t size() const noexcept { return observations_.size(); }
private:
    std::deque<TwoHandFrameSetObservation> observations_;
};

struct TwoHandFeatureFrame
{
    double timeOffsetMs{0.0};
    float centerX{0.0F};
    float centerY{0.0F};
    float relativeAngleRadians{0.0F};
    float palmVelocity{0.0F};
    std::array<float, kTwoHandFeatureLength> values{};
};

struct TwoHandFeatureSummary
{
    float relativeTranslationDistance{0.0F};
    float relativeDistanceRange{0.0F};
    float relativeDistanceDelta{0.0F};
    float relativeAngleRangeRadians{0.0F};
    float lowHandednessMean{0.0F};
    float highHandednessMean{0.0F};
    float topologyChangeScore{0.0F};
    float meanRelativeDistance{0.0F};
};

struct TwoHandSequence
{
    std::vector<TwoHandFeatureFrame> frames;
    float motionScore{0.0F};
    float averageVelocity{0.0F};
    float peakVelocity{0.0F};
    TwoHandFeatureSummary summary{};
};

struct TwoHandTemplate
{
    std::array<TwoHandFeatureFrame, measurements::kUnifiedSequenceLength> frames{};
    TwoHandFeatureSummary summary{};
    std::size_t sourceFrameCount{0};
    double durationMs{0.0};
    bool valid{false};
};

struct TwoHandTemplateCreationResult
{
    TwoHandTemplate value{};
    std::string rejectionReason;
};

struct TwoHandComparisonResult
{
    float score{0.0F};
    float confidence{0.0F};
    float warpRatio{0.0F};
    bool eligible{false};
    std::string reason;
};

[[nodiscard]] std::size_t countUsableTwoHandFrames(
    const std::vector<TwoHandFrameSetObservation>& source) noexcept;
[[nodiscard]] bool hasPredominantTwoHandCoverage(
    const std::vector<TwoHandFrameSetObservation>& source,
    std::size_t capturedFrameCount) noexcept;
[[nodiscard]] TwoHandSequence extractTwoHandSequence(
    const std::vector<TwoHandFrameSetObservation>& source);
[[nodiscard]] TwoHandTemplateCreationResult createTwoHandTemplate(
    const std::vector<TwoHandFrameSetObservation>& source);
[[nodiscard]] TwoHandTemplateCreationResult createTwoHandCandidate(
    const std::vector<TwoHandFrameSetObservation>& source);
[[nodiscard]] TwoHandComparisonResult compareTwoHand(
    const TwoHandTemplate& candidate, const TwoHandTemplate& reference) noexcept;
}

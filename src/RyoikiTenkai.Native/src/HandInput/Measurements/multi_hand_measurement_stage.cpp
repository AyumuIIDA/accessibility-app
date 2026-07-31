#include "HandInput/Measurements/multi_hand_measurement_stage.h"

#include <algorithm>
#include <cmath>

namespace ryoiki::hand_input::measurements
{
namespace
{
constexpr std::size_t kWristIndex = 0;
constexpr std::size_t kMiddleMcpIndex = 9;

float palmCenterX(const hand_perception::HandLandmarkResult& hand) noexcept
{
    return 0.5F
        * (hand.landmarks[kWristIndex].x + hand.landmarks[kMiddleMcpIndex].x);
}

float palmCenterY(const hand_perception::HandLandmarkResult& hand) noexcept
{
    return 0.5F
        * (hand.landmarks[kWristIndex].y + hand.landmarks[kMiddleMcpIndex].y);
}

float palmAxisLength(const hand_perception::HandLandmarkResult& hand) noexcept
{
    return std::hypot(
        hand.landmarks[kMiddleMcpIndex].x - hand.landmarks[kWristIndex].x,
        hand.landmarks[kMiddleMcpIndex].y - hand.landmarks[kWristIndex].y);
}
}

MultiHandMeasurementStage::ExtractorSlot& MultiHandMeasurementStage::slotFor(
    const hand_perception::HandTrackId trackId,
    const std::uint64_t frameId) noexcept
{
    for (auto& slot : slots_)
    {
        if (slot.active && slot.trackId == trackId)
        {
            slot.lastSeenFrameId = frameId;
            slot.missingFrameCount = 0;
            return slot;
        }
    }

    auto* selected = &slots_[0];
    for (auto& slot : slots_)
    {
        if (!slot.active)
        {
            selected = &slot;
            break;
        }
        if (slot.lastSeenFrameId < selected->lastSeenFrameId)
        {
            selected = &slot;
        }
    }
    selected->extractor.reset();
    selected->topologyHistory.reset();
    selected->gestureHistory.reset();
    selected->trackId = trackId;
    selected->lastSeenFrameId = frameId;
    selected->missingFrameCount = 0;
    selected->active = true;
    return *selected;
}

void MultiHandMeasurementStage::ageUnobservedSlots(
    const std::uint64_t frameId) noexcept
{
    for (auto& slot : slots_)
    {
        if (!slot.active || slot.lastSeenFrameId == frameId)
        {
            continue;
        }
        // A dropped detection is a gap, not the end of a segment. The history
        // keeps its samples until the grace window is exhausted.
        ++slot.missingFrameCount;
        slot.topologyHistory.noteMissingObservation();
        slot.gestureHistory.noteMissingObservation();
        if (slot.missingFrameCount > kHandTopologyGapToleranceFrames)
        {
            slot.extractor.reset();
            slot.topologyHistory.reset();
            slot.gestureHistory.reset();
            slot.active = false;
            slot.trackId = 0;
            slot.missingFrameCount = 0;
        }
    }
}

MultiHandMeasurementFrame MultiHandMeasurementStage::extract(
    const hand_perception::HandPerceptionResult& observations,
    const std::uint64_t frameId,
    const std::uint64_t timestampUs,
    const std::uint32_t uprightWidth,
    const std::uint32_t uprightHeight) noexcept
{
    MultiHandMeasurementFrame result{};
    result.frameId = frameId;
    result.timestampUs = timestampUs;
    result.handCount = (std::min)(
        observations.handCount,
        hand_perception::kMaxPerceivedHands);
    for (std::size_t index = 0; index < result.handCount; ++index)
    {
        const auto trackId = observations.trackIds[index];
        auto& slot = slotFor(trackId, frameId);
        const auto measurement = slot.extractor.extract(
            observations.hands[index],
            frameId,
            timestampUs,
            uprightWidth,
            uprightHeight);
        slot.topologyHistory.push(measurement.topology);
        const bool usableObservation = observations.hands[index].detected
            && observations.hands[index].confidence >= kGestureMinimumUsableConfidence;
        slot.gestureHistory.push(
            makeUnifiedFeatureObservation(observations.hands[index], timestampUs),
            usableObservation);
        result.hands[index] = {
            trackId,
            measurement.hand,
            measurement.screenPalm,
            measurement.topology,
            slot.topologyHistory.summarize()};
    }

    if (result.handCount == 2
        && observations.hands[0].detected
        && observations.hands[1].detected)
    {
        // Ordering is a deterministic presentation of the pair, never an
        // identity. Prefer the temporally filtered handedness when the two
        // hands disagree clearly; otherwise fall back to screen position so a
        // noisy handedness score cannot flip the sign of the relation.
        const float firstHandedness = observations.filteredHandedness[0];
        const float secondHandedness = observations.filteredHandedness[1];
        const bool handednessReliable =
            std::isfinite(firstHandedness)
            && std::isfinite(secondHandedness)
            && std::abs(firstHandedness - secondHandedness)
                >= kReliableHandednessGap;

        std::size_t lowIndex = 0;
        std::size_t highIndex = 1;
        if (handednessReliable)
        {
            if (secondHandedness < firstHandedness)
            {
                lowIndex = 1;
                highIndex = 0;
            }
        }
        else if (palmCenterX(observations.hands[1])
            < palmCenterX(observations.hands[0]))
        {
            lowIndex = 1;
            highIndex = 0;
        }

        const auto& lowHand = observations.hands[lowIndex];
        const auto& highHand = observations.hands[highIndex];
        const float lowScale = palmAxisLength(lowHand);
        const float highScale = palmAxisLength(highHand);
        const float averageScale =
            (std::max)(1.0F, 0.5F * (lowScale + highScale));

        auto& relation = result.relation;
        relation.frameId = frameId;
        relation.timestampUs = timestampUs;
        relation.firstTrackId = result.hands[lowIndex].trackId;
        relation.secondTrackId = result.hands[highIndex].trackId;
        relation.ordering = handednessReliable
            ? TwoHandOrdering::Handedness
            : TwoHandOrdering::ScreenPosition;
        relation.deltaX =
            (palmCenterX(highHand) - palmCenterX(lowHand)) / averageScale;
        relation.deltaY =
            (palmCenterY(highHand) - palmCenterY(lowHand)) / averageScale;
        relation.distance = std::hypot(relation.deltaX, relation.deltaY);
        relation.angleRadians =
            std::atan2(relation.deltaY, relation.deltaX);
        relation.scaleRatio = lowScale > 1.0e-5F
            ? highScale / lowScale
            : 0.0F;
        relation.quality = (std::min)(
            result.hands[lowIndex].hand.trackingQuality,
            result.hands[highIndex].hand.trackingQuality);
        relation.valid = std::isfinite(relation.deltaX)
            && std::isfinite(relation.deltaY)
            && std::isfinite(relation.distance)
            && std::isfinite(relation.angleRadians)
            && std::isfinite(relation.scaleRatio)
            && relation.scaleRatio > 0.0F;
    }

    ageUnobservedSlots(frameId);
    return result;
}

void MultiHandMeasurementStage::reset() noexcept
{
    slots_ = {};
}

const GestureCandidateWindowHistory* MultiHandMeasurementStage::gestureCandidateHistory(
    const hand_perception::HandTrackId trackId) const noexcept
{
    for (const auto& slot : slots_)
    {
        if (slot.active && slot.trackId == trackId)
        {
            return &slot.gestureHistory;
        }
    }
    return nullptr;
}
}

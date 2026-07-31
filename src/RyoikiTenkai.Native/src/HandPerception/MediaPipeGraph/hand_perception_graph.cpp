#include "HandPerception/MediaPipeGraph/hand_perception_graph.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace ryoiki::hand_perception
{
namespace
{
float boxWidth(const BoundingBox& box) noexcept
{
    return (std::max)(0.0F, box.right - box.left);
}

float boxHeight(const BoundingBox& box) noexcept
{
    return (std::max)(0.0F, box.bottom - box.top);
}

float intersectionOverUnion(
    const BoundingBox& first,
    const BoundingBox& second) noexcept
{
    const float intersectionWidth = (std::max)(
        0.0F,
        (std::min)(first.right, second.right)
            - (std::max)(first.left, second.left));
    const float intersectionHeight = (std::max)(
        0.0F,
        (std::min)(first.bottom, second.bottom)
            - (std::max)(first.top, second.top));
    const float intersection = intersectionWidth * intersectionHeight;
    const float unionArea = boxWidth(first) * boxHeight(first)
        + boxWidth(second) * boxHeight(second) - intersection;
    return unionArea > 0.0F ? intersection / unionArea : 0.0F;
}

float normalizedCenterDistance(
    const BoundingBox& first,
    const BoundingBox& second) noexcept
{
    const float firstCenterX = 0.5F * (first.left + first.right);
    const float firstCenterY = 0.5F * (first.top + first.bottom);
    const float secondCenterX = 0.5F * (second.left + second.right);
    const float secondCenterY = 0.5F * (second.top + second.bottom);
    const float dx = firstCenterX - secondCenterX;
    const float dy = firstCenterY - secondCenterY;
    const float referenceDiagonal = (std::max)(
        std::hypot(boxWidth(first), boxHeight(first)),
        std::hypot(boxWidth(second), boxHeight(second)));
    return referenceDiagonal > 1.0e-5F
        ? std::hypot(dx, dy) / referenceDiagonal
        : std::numeric_limits<float>::infinity();
}

bool representsSameHand(
    const BoundingBox& first,
    const BoundingBox& second) noexcept
{
    return intersectionOverUnion(first, second) >= 0.35F
        || normalizedCenterDistance(first, second) <= 0.25F;
}

BoundingBox predictBox(
    const BoundingBox& last,
    const BoundingBox& previous,
    const bool hasPrevious) noexcept
{
    if (!hasPrevious)
    {
        return last;
    }

    const float deltaX = 0.5F * (
        last.left + last.right - previous.left - previous.right);
    const float deltaY = 0.5F * (
        last.top + last.bottom - previous.top - previous.bottom);
    return {
        last.left + deltaX,
        last.top + deltaY,
        last.right + deltaX,
        last.bottom + deltaY};
}

float associationCost(
    const BoundingBox& candidate,
    const BoundingBox& last,
    const BoundingBox& previous,
    const bool hasPrevious) noexcept
{
    const BoundingBox predicted = predictBox(last, previous, hasPrevious);
    return normalizedCenterDistance(candidate, predicted)
        + 0.25F * (1.0F - intersectionOverUnion(candidate, predicted));
}

float normalizedLandmarkDistance(
    const HandLandmarkResult& first,
    const HandLandmarkResult& second) noexcept
{
    const float referenceDiagonal = (std::max)(
        std::hypot(boxWidth(first.box), boxHeight(first.box)),
        std::hypot(boxWidth(second.box), boxHeight(second.box)));
    if (referenceDiagonal <= 1.0e-5F)
    {
        return std::numeric_limits<float>::infinity();
    }

    float squaredDistance = 0.0F;
    for (std::size_t index = 0; index < first.landmarks.size(); ++index)
    {
        const float dx = first.landmarks[index].x - second.landmarks[index].x;
        const float dy = first.landmarks[index].y - second.landmarks[index].y;
        squaredDistance += dx * dx + dy * dy;
    }
    return std::sqrt(
        squaredDistance / static_cast<float>(first.landmarks.size()))
        / referenceDiagonal;
}

bool duplicatesCandidate(
    const HandLandmarkResult& first,
    const HandLandmarkResult& second) noexcept
{
    return intersectionOverUnion(first.box, second.box) >= 0.55F
        && normalizedCenterDistance(first.box, second.box) <= 0.25F
        && normalizedLandmarkDistance(first, second) <= 0.18F;
}

void accumulateMetrics(
    HandLandmarkGraphMetrics& total,
    const HandLandmarkGraphMetrics& value) noexcept
{
    total.roiCropWarpMs += value.roiCropWarpMs;
    total.inferenceMs += value.inferenceMs;
    total.postprocessMs += value.postprocessMs;
}

void addObservation(
    HandPerceptionResult& result,
    const HandTrackId trackId,
    const geometry::RotatedRegion& region,
    const HandLandmarkResult& hand,
    const bool usedTracking) noexcept
{
    if (!hand.detected || result.handCount >= kMaxPerceivedHands)
    {
        return;
    }

    const std::size_t index = result.handCount++;
    result.hands[index] = hand;
    result.handRegions[index] = region;
    result.trackIds[index] = trackId;
    result.handFlags[index] = usedTracking ? 1U : 0U;
    if (!result.hand.detected)
    {
        result.hand = hand;
        result.handRegion = region;
    }
}

struct HandCandidate
{
    HandLandmarkResult hand{};
    geometry::RotatedRegion inferenceRegion{};
    geometry::RotatedRegion nextRegion{};
    HandTrackId sourceTrackId{0};
    bool usedTracking{false};
};

enum class ReliableHandedness
{
    Unknown,
    Low,
    High
};

ReliableHandedness classifyReliableHandedness(const float value) noexcept
{
    constexpr float kLowMaximum = 0.20F;
    constexpr float kHighMinimum = 0.80F;
    if (value <= kLowMaximum)
    {
        return ReliableHandedness::Low;
    }
    if (value >= kHighMinimum)
    {
        return ReliableHandedness::High;
    }
    return ReliableHandedness::Unknown;
}

template <typename Track>
ReliableHandedness trackHandedness(const Track& track) noexcept
{
    return track.hasHandedness
        ? classifyReliableHandedness(track.filteredHandedness)
        : ReliableHandedness::Unknown;
}

ReliableHandedness candidateHandedness(
    const HandCandidate& candidate) noexcept
{
    return classifyReliableHandedness(candidate.hand.handedness);
}

template <typename Track>
std::optional<std::array<int, kMaxPerceivedHands>>
tryAssignByReliableHandedness(
    const std::span<const Track> tracks,
    const std::span<const HandCandidate> candidates) noexcept
{
    if (tracks.size() != kMaxPerceivedHands
        || candidates.size() != kMaxPerceivedHands)
    {
        return std::nullopt;
    }

    const ReliableHandedness firstTrack = trackHandedness(tracks[0]);
    const ReliableHandedness secondTrack = trackHandedness(tracks[1]);
    const ReliableHandedness firstCandidate =
        candidateHandedness(candidates[0]);
    const ReliableHandedness secondCandidate =
        candidateHandedness(candidates[1]);
    const bool tracksAreComplementary =
        firstTrack != ReliableHandedness::Unknown
        && secondTrack != ReliableHandedness::Unknown
        && firstTrack != secondTrack;
    const bool candidatesAreComplementary =
        firstCandidate != ReliableHandedness::Unknown
        && secondCandidate != ReliableHandedness::Unknown
        && firstCandidate != secondCandidate;
    if (!tracksAreComplementary || !candidatesAreComplementary)
    {
        return std::nullopt;
    }

    return firstTrack == firstCandidate
        ? std::array<int, kMaxPerceivedHands>{0, 1}
        : std::array<int, kMaxPerceivedHands>{1, 0};
}

template <typename Track>
std::array<int, kMaxPerceivedHands> assignCandidates(
    const std::span<const Track> tracks,
    const std::span<const HandCandidate> candidates) noexcept
{
    std::array<int, kMaxPerceivedHands> candidateForTrack{-1, -1};
    if (tracks.empty() || candidates.empty())
    {
        return candidateForTrack;
    }

    const auto cost = [&tracks, &candidates](
        const std::size_t trackIndex,
        const std::size_t candidateIndex)
    {
        const auto& track = tracks[trackIndex];
        float value = associationCost(
            candidates[candidateIndex].hand.box,
            track.lastBox,
            track.previousBox,
            track.hasPreviousBox);
        if (candidates[candidateIndex].sourceTrackId != 0
            && candidates[candidateIndex].sourceTrackId != track.id)
        {
            value += track.missingFrameCount > 0 ? 10.0F : 0.05F;
        }
        if (track.hasHandedness)
        {
            value += 0.20F * std::abs(
                candidates[candidateIndex].hand.handedness
                - track.filteredHandedness);
        }
        return value;
    };

    if (tracks.size() == 1)
    {
        candidateForTrack[0] = candidates.size() == 1 || cost(0, 0) <= cost(0, 1)
            ? 0
            : 1;
        return candidateForTrack;
    }
    if (candidates.size() == 1)
    {
        candidateForTrack[cost(0, 0) <= cost(1, 0) ? 0 : 1] = 0;
        return candidateForTrack;
    }

    if (const auto handednessAssignment =
            tryAssignByReliableHandedness(tracks, candidates))
    {
        return *handednessAssignment;
    }

    const float directCost = cost(0, 0) + cost(1, 1);
    const float crossedCost = cost(0, 1) + cost(1, 0);
    if (directCost <= crossedCost)
    {
        candidateForTrack = {0, 1};
    }
    else
    {
        candidateForTrack = {1, 0};
    }
    return candidateForTrack;
}

template <typename Track>
std::size_t findTrackIndex(
    const std::span<const Track> tracks,
    const HandTrackId id) noexcept
{
    for (std::size_t index = 0; index < tracks.size(); ++index)
    {
        if (tracks[index].id == id)
        {
            return index;
        }
    }
    return tracks.size();
}

template <typename Track>
std::pair<std::size_t, std::size_t> chooseAmbiguousOwner(
    const std::span<const Track> tracks,
    const std::span<const HandCandidate> candidates,
    const HandTrackId lockedOwnerId) noexcept
{
    std::size_t ownerTrackIndex = findTrackIndex(tracks, lockedOwnerId);
    float bestCost = std::numeric_limits<float>::infinity();
    std::size_t bestCandidateIndex = 0;

    if (tracks.size() == kMaxPerceivedHands)
    {
        for (std::size_t candidateIndex = 0;
            candidateIndex < candidates.size();
            ++candidateIndex)
        {
            const ReliableHandedness candidateClass =
                candidateHandedness(candidates[candidateIndex]);
            if (candidateClass == ReliableHandedness::Unknown)
            {
                continue;
            }

            std::size_t matchingTrackIndex = tracks.size();
            std::size_t matchingTrackCount = 0;
            for (std::size_t trackIndex = 0;
                trackIndex < tracks.size();
                ++trackIndex)
            {
                if (trackHandedness(tracks[trackIndex]) == candidateClass)
                {
                    matchingTrackIndex = trackIndex;
                    ++matchingTrackCount;
                }
            }
            if (matchingTrackCount == 1)
            {
                return {matchingTrackIndex, candidateIndex};
            }
        }
    }

    const auto pairCost = [&tracks, &candidates](
        const std::size_t trackIndex,
        const std::size_t candidateIndex)
    {
        const auto& track = tracks[trackIndex];
        const auto& candidate = candidates[candidateIndex];
        float value = associationCost(
            candidate.hand.box,
            track.lastBox,
            track.previousBox,
            track.hasPreviousBox);
        if (track.hasHandedness)
        {
            value += 0.45F * std::abs(
                candidate.hand.handedness - track.filteredHandedness);
        }
        if (candidate.sourceTrackId == track.id)
        {
            value -= 0.10F;
        }
        value -= 0.05F * candidate.hand.confidence;
        return value;
    };

    if (ownerTrackIndex < tracks.size())
    {
        for (std::size_t candidateIndex = 0;
            candidateIndex < candidates.size();
            ++candidateIndex)
        {
            const float value = pairCost(
                ownerTrackIndex, candidateIndex);
            if (value < bestCost)
            {
                bestCost = value;
                bestCandidateIndex = candidateIndex;
            }
        }
        return {ownerTrackIndex, bestCandidateIndex};
    }

    ownerTrackIndex = 0;
    for (std::size_t trackIndex = 0;
        trackIndex < tracks.size();
        ++trackIndex)
    {
        for (std::size_t candidateIndex = 0;
            candidateIndex < candidates.size();
            ++candidateIndex)
        {
            const float value = pairCost(trackIndex, candidateIndex);
            if (value < bestCost)
            {
                bestCost = value;
                ownerTrackIndex = trackIndex;
                bestCandidateIndex = candidateIndex;
            }
        }
    }
    return {ownerTrackIndex, bestCandidateIndex};
}
}

HandPerceptionGraph::HandPerceptionGraph(
    std::unique_ptr<IPalmDetectionRunner> palmRunner,
    std::unique_ptr<IHandLandmarkRunner> handRunner,
    const HandPerceptionGraphOptions options)
    : palmGraph_{std::move(palmRunner)}, handGraph_{std::move(handRunner)}, options_{options}
{
}

HandPerceptionGraph::HandPerceptionGraph(
    std::unique_ptr<IPalmDetectionRunner> palmRunner,
    std::unique_ptr<IHandLandmarkRunner> handRunner,
    std::unique_ptr<geometry::IGeometryProcessor> palmGeometryProcessor,
    std::unique_ptr<geometry::IGeometryProcessor> handGeometryProcessor,
    const HandPerceptionGraphOptions options)
    : palmGraph_{std::move(palmRunner), std::move(palmGeometryProcessor)},
      handGraph_{std::move(handRunner), std::move(handGeometryProcessor)},
      options_{options}
{
}

bool HandPerceptionGraph::process(
    const buffers::FrameBuffer& frame,
    HandPerceptionResult& result,
    HandPerceptionGraphMetrics& metrics,
    std::string& error)
{
    result = {};
    metrics = {};
    ++processedFrameCount_;

    std::array<TrackedHandSlot, kMaxPerceivedHands> nextTrackedHands{};
    std::size_t nextTrackedHandCount = 0;
    const std::size_t previousTrackedHandCount = trackedHandCount_;
    const bool missingTrackWasPending = std::ranges::any_of(
        std::span{trackedHands_}.first(trackedHandCount_),
        [](const TrackedHandSlot& tracked)
        {
            return tracked.missingFrameCount > 0;
        });

    std::array<HandCandidate, kMaxPerceivedHands> trackingCandidates{};
    std::size_t trackingCandidateCount = 0;
    if (trackedHandCount_ > 0)
    {
        for (std::size_t index = 0; index < trackedHandCount_; ++index)
        {
            const TrackedHandSlot& tracked = trackedHands_[index];
            if (tracked.missingFrameCount > 0)
            {
                continue;
            }

            result.usedTracking = true;
            HandLandmarkResult trackedHand{};
            HandLandmarkGraphMetrics trackedMetrics{};
            if (!handGraph_.process(
                    frame,
                    tracked.region,
                    0.5F,
                    trackedHand,
                    trackedMetrics,
                    error))
            {
                return false;
            }
            accumulateMetrics(metrics.hand, trackedMetrics);

            if (trackedHand.detected)
            {
                const auto trackingStarted = std::chrono::steady_clock::now();
                const auto nextRegion = landmarksToRoi_.calculate(trackedHand);
                metrics.trackingUpdateMs += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - trackingStarted).count();
                HandCandidate candidate{
                    trackedHand,
                    tracked.region,
                    nextRegion,
                    tracked.id,
                    true};
                trackingCandidates[trackingCandidateCount++] =
                    std::move(candidate);
                continue;
            }
            error.clear();
        }
    }

    const auto trackedSpan =
        std::span<const TrackedHandSlot>{trackedHands_}.first(
            trackedHandCount_);
    const auto trackingCandidateSpan =
        std::span<const HandCandidate>{trackingCandidates}.first(
            trackingCandidateCount);
    const bool duplicateTrackingCandidates =
        trackingCandidateCount == 2
        && duplicatesCandidate(
            trackingCandidates[0].hand,
            trackingCandidates[1].hand);
    const bool ambiguousTracking =
        trackedHandCount_ == kMaxPerceivedHands
        && (trackingCandidateCount == 1
            || duplicateTrackingCandidates);

    std::array<int, kMaxPerceivedHands> trackingAssignments{-1, -1};
    if (trackedHandCount_ == kMaxPerceivedHands
        && trackingCandidateCount == 0)
    {
        crossing_.ageFrames = crossing_.active
            ? crossing_.ageFrames + 1
            : 1;
        crossing_.active = true;
    }
    else if (ambiguousTracking)
    {
        const auto [ownerTrackIndex, ownerCandidateIndex] =
            chooseAmbiguousOwner(
                trackedSpan,
                trackingCandidateSpan,
                crossing_.active ? crossing_.visibleOwnerId : 0);
        trackingAssignments[ownerTrackIndex] =
            static_cast<int>(ownerCandidateIndex);
        crossing_.visibleOwnerId = trackedHands_[ownerTrackIndex].id;
        crossing_.ageFrames = crossing_.active
            ? crossing_.ageFrames + 1
            : 1;
        crossing_.active = true;
    }
    else
    {
        trackingAssignments = assignCandidates(
            trackedSpan, trackingCandidateSpan);
    }

    for (std::size_t trackIndex = 0;
        trackIndex < trackedHandCount_;
        ++trackIndex)
    {
        const TrackedHandSlot& tracked = trackedHands_[trackIndex];
        const int candidateIndex = trackingAssignments[trackIndex];
        if (candidateIndex >= 0)
        {
            const HandCandidate& candidate =
                trackingCandidates[static_cast<std::size_t>(candidateIndex)];
            TrackedHandSlot updated = tracked;
            updated.region = candidate.nextRegion;
            updated.previousBox = tracked.lastBox;
            updated.lastBox = candidate.hand.box;
            updated.missingFrameCount = 0;
            updated.hasPreviousBox = true;
            updated.active = true;
            if (!ambiguousTracking)
            {
                updated.filteredHandedness = updated.hasHandedness
                    ? 0.85F * updated.filteredHandedness
                        + 0.15F * candidate.hand.handedness
                    : candidate.hand.handedness;
                updated.hasHandedness = true;
            }
            nextTrackedHands[nextTrackedHandCount++] = updated;
            addObservation(
                result,
                tracked.id,
                candidate.inferenceRegion,
                candidate.hand,
                candidate.usedTracking);
            continue;
        }

        const std::uint32_t missingFrameCount =
            tracked.missingFrameCount + 1;
        if (missingFrameCount <= options_.missingTrackGraceFrames)
        {
            TrackedHandSlot missing = tracked;
            missing.missingFrameCount = missingFrameCount;
            nextTrackedHands[nextTrackedHandCount++] = missing;
        }
    }

    const bool rediscoveryDue =
        options_.palmRediscoveryIntervalFrames > 0
        && ++framesSincePalmDiscovery_
            >= options_.palmRediscoveryIntervalFrames;
    const bool shouldDiscoverPalms =
        result.handCount < kMaxPerceivedHands
        && (previousTrackedHandCount == 0
            || missingTrackWasPending
            || crossing_.active
            || rediscoveryDue);
    const bool calibrationProbeDue =
        options_.calibrationPalmIntervalFrames > 0
        && processedFrameCount_ % options_.calibrationPalmIntervalFrames == 0;

    PalmDetectionResult calibrationPalms{};
    PalmDetectionResult& detectedPalms =
        shouldDiscoverPalms ? result.palms : calibrationPalms;
    if ((shouldDiscoverPalms || calibrationProbeDue)
        && !palmGraph_.process(frame, detectedPalms, metrics.palm, error))
    {
        return false;
    }
    if (shouldDiscoverPalms)
    {
        framesSincePalmDiscovery_ = 0;
    }

    std::array<HandCandidate, kMaxPerceivedHands> palmCandidates{};
    std::size_t palmCandidateCount = 0;
    for (std::size_t palmIndex = 0;
        shouldDiscoverPalms
            && palmIndex < result.palms.size()
            && palmCandidateCount < kMaxPerceivedHands;
        ++palmIndex)
    {
        const PalmDetection& detection = result.palms[palmIndex];
        bool overlapsVisibleHand = false;
        for (std::size_t handIndex = 0;
            handIndex < result.handCount;
            ++handIndex)
        {
            if (representsSameHand(
                    detection.box,
                    result.hands[handIndex].box))
            {
                overlapsVisibleHand = true;
                break;
            }
        }
        if (overlapsVisibleHand)
        {
            continue;
        }

        const auto candidateRegion = detectionToRoi_.calculate(detection);
        HandLandmarkResult candidateHand{};
        HandLandmarkGraphMetrics candidateMetrics{};
        if (!handGraph_.process(
                frame,
                candidateRegion,
                0.7F,
                candidateHand,
                candidateMetrics,
                error))
        {
            return false;
        }
        accumulateMetrics(metrics.hand, candidateMetrics);
        if (!candidateHand.detected)
        {
            error.clear();
            continue;
        }

        bool duplicateCandidate = false;
        for (std::size_t handIndex = 0;
            handIndex < result.handCount;
            ++handIndex)
        {
            if (duplicatesCandidate(
                    candidateHand,
                    result.hands[handIndex]))
            {
                duplicateCandidate = true;
                break;
            }
        }
        for (std::size_t candidateIndex = 0;
            !duplicateCandidate && candidateIndex < palmCandidateCount;
            ++candidateIndex)
        {
            duplicateCandidate = duplicatesCandidate(
                candidateHand, palmCandidates[candidateIndex].hand);
        }
        if (duplicateCandidate)
        {
            continue;
        }

        const auto trackingStarted = std::chrono::steady_clock::now();
        const auto nextRegion = landmarksToRoi_.calculate(candidateHand);
        metrics.trackingUpdateMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - trackingStarted).count();

        palmCandidates[palmCandidateCount++] = {
            candidateHand,
            candidateRegion,
            nextRegion,
            0,
            false};
    }

    std::array<TrackedHandSlot, kMaxPerceivedHands> missingTracks{};
    std::array<std::size_t, kMaxPerceivedHands> missingTrackIndices{};
    std::size_t missingTrackCount = 0;
    for (std::size_t index = 0; index < nextTrackedHandCount; ++index)
    {
        if (nextTrackedHands[index].missingFrameCount > 0)
        {
            missingTracks[missingTrackCount] = nextTrackedHands[index];
            missingTrackIndices[missingTrackCount++] = index;
        }
    }

    const auto palmAssignments = assignCandidates(
        std::span<const TrackedHandSlot>{missingTracks}.first(
            missingTrackCount),
        std::span<const HandCandidate>{palmCandidates}.first(
            palmCandidateCount));
    std::array<bool, kMaxPerceivedHands> palmCandidateAssigned{};
    for (std::size_t missingIndex = 0;
        missingIndex < missingTrackCount;
        ++missingIndex)
    {
        const int candidateIndex = palmAssignments[missingIndex];
        if (candidateIndex < 0)
        {
            continue;
        }

        const std::size_t assignedCandidateIndex =
            static_cast<std::size_t>(candidateIndex);
        palmCandidateAssigned[assignedCandidateIndex] = true;
        const HandCandidate& candidate =
            palmCandidates[assignedCandidateIndex];
        TrackedHandSlot& matched =
            nextTrackedHands[missingTrackIndices[missingIndex]];
        const HandTrackId trackId = matched.id;
        matched.region = candidate.nextRegion;
        matched.previousBox = matched.lastBox;
        matched.lastBox = candidate.hand.box;
        matched.missingFrameCount = 0;
        matched.hasPreviousBox = true;
        matched.active = true;
        if (!crossing_.active)
        {
            matched.filteredHandedness = matched.hasHandedness
                ? 0.85F * matched.filteredHandedness
                    + 0.15F * candidate.hand.handedness
                : candidate.hand.handedness;
            matched.hasHandedness = true;
        }
        addObservation(
            result,
            trackId,
            candidate.inferenceRegion,
            candidate.hand,
            false);
    }

    for (std::size_t candidateIndex = 0;
        candidateIndex < palmCandidateCount
            && nextTrackedHandCount < kMaxPerceivedHands;
        ++candidateIndex)
    {
        if (palmCandidateAssigned[candidateIndex])
        {
            continue;
        }

        const HandCandidate& candidate = palmCandidates[candidateIndex];
        HandTrackId trackId = nextTrackId_++;
        if (nextTrackId_ == 0)
        {
            nextTrackId_ = 1;
        }
        TrackedHandSlot created{};
        created.id = trackId;
        created.region = candidate.nextRegion;
        created.lastBox = candidate.hand.box;
        created.filteredHandedness = candidate.hand.handedness;
        created.hasHandedness = true;
        created.active = true;
        nextTrackedHands[nextTrackedHandCount++] = created;
        addObservation(
            result,
            trackId,
            candidate.inferenceRegion,
            candidate.hand,
            false);
    }

    if (result.handCount == kMaxPerceivedHands)
    {
        crossing_ = {};
    }
    else if (nextTrackedHandCount < kMaxPerceivedHands)
    {
        crossing_ = {};
    }

    result.crossingActive = crossing_.active;
    result.crossingOwnerTrackId = crossing_.visibleOwnerId;
    for (std::size_t handIndex = 0;
        handIndex < result.handCount;
        ++handIndex)
    {
        for (std::size_t trackIndex = 0;
            trackIndex < nextTrackedHandCount;
            ++trackIndex)
        {
            if (nextTrackedHands[trackIndex].id
                == result.trackIds[handIndex])
            {
                result.filteredHandedness[handIndex] =
                    nextTrackedHands[trackIndex].hasHandedness
                    ? nextTrackedHands[trackIndex].filteredHandedness
                    : result.hands[handIndex].handedness;
                break;
            }
        }
    }

    trackedHands_ = nextTrackedHands;
    trackedHandCount_ = nextTrackedHandCount;
    error.clear();
    return true;
}

std::string HandPerceptionGraph::providerSummary() const
{
    return "palm=" + std::string{palmGraph_.providerName()}
        + ", hand=" + std::string{handGraph_.providerName()};
}
}

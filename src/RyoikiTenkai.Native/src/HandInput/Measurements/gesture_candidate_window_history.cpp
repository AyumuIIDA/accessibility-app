#include "HandInput/Measurements/gesture_candidate_window_history.h"

#include <algorithm>
#include <vector>

namespace ryoiki::hand_input::measurements
{
void GestureCandidateWindowHistory::push(
    const UnifiedFeatureObservation& observation, const bool usable) noexcept
{
    if (size_ > 0 && observation.timestampUs < at(size_ - 1).observation.timestampUs)
    {
        // Non-monotonic timestamp: reject rather than corrupt the
        // chronological ordering buildCandidateWindows relies on.
        return;
    }

    missingObservations_ = 0;
    if (size_ == samples_.size())
    {
        popOldest();
    }
    samples_[(start_ + size_) % samples_.size()] = StoredObservation{observation, usable};
    ++size_;
    pruneToWindow();
}

void GestureCandidateWindowHistory::noteMissingObservation() noexcept
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

void GestureCandidateWindowHistory::reset() noexcept
{
    start_ = 0;
    size_ = 0;
    missingObservations_ = 0;
}

void GestureCandidateWindowHistory::popOldest() noexcept
{
    if (size_ == 0)
    {
        return;
    }
    start_ = (start_ + 1) % samples_.size();
    --size_;
}

void GestureCandidateWindowHistory::pruneToWindow() noexcept
{
    while (size_ > 1)
    {
        const auto& newest = at(size_ - 1).observation;
        const auto& oldest = at(0).observation;
        if (newest.timestampUs < oldest.timestampUs
            || newest.timestampUs - oldest.timestampUs <= kHandTopologyWindowUs)
        {
            return;
        }
        popOldest();
    }
}

const GestureCandidateWindowHistory::StoredObservation&
GestureCandidateWindowHistory::at(const std::size_t chronologicalIndex) const noexcept
{
    return samples_[(start_ + chronologicalIndex) % samples_.size()];
}

std::size_t GestureCandidateWindowHistory::size() const noexcept
{
    return size_;
}

std::uint32_t GestureCandidateWindowHistory::missingObservationCount() const noexcept
{
    return missingObservations_;
}

std::size_t GestureCandidateWindowHistory::buildCandidateWindows(
    std::array<GestureCandidateWindow, kGestureCandidateDurationsMs.size()>& outWindows) const noexcept
{
    std::size_t written = 0;
    if (size_ == 0)
    {
        return 0;
    }

    const std::uint64_t now = at(size_ - 1).observation.timestampUs;
    for (const std::uint32_t durationMs : kGestureCandidateDurationsMs)
    {
        const std::uint64_t durationUs = static_cast<std::uint64_t>(durationMs) * 1000ULL;
        const std::uint64_t start = now >= durationUs ? now - durationUs : 0;

        std::size_t firstIndex = size_;
        for (std::size_t index = 0; index < size_; ++index)
        {
            if (at(index).observation.timestampUs >= start)
            {
                firstIndex = index;
                break;
            }
        }
        if (firstIndex == size_)
        {
            continue;
        }

        const auto& firstStored = at(firstIndex);
        const auto& lastStored = at(size_ - 1);
        const std::size_t count = size_ - firstIndex;
        const double durationMsActual = count < 2
            ? 0.0
            : (static_cast<double>(lastStored.observation.timestampUs)
                  - static_cast<double>(firstStored.observation.timestampUs))
                / 1000.0;
        if (durationMsActual < kGestureMinimumCandidateDurationMs)
        {
            continue;
        }

        std::size_t usableCount = 0;
        for (std::size_t index = firstIndex; index < size_; ++index)
        {
            if (at(index).usable)
            {
                ++usableCount;
            }
        }
        if (usableCount < kGestureMinimumCandidateUsableFrames)
        {
            continue;
        }

        std::vector<UnifiedFeatureObservation> usableObservations;
        usableObservations.reserve(usableCount);
        for (std::size_t index = firstIndex; index < size_; ++index)
        {
            const auto& stored = at(index);
            if (stored.usable)
            {
                usableObservations.push_back(stored.observation);
            }
        }

        const auto frames = buildFeatureSequence(usableObservations);
        const auto unified = buildUnifiedSequence(frames);
        if (unified.size() < 3)
        {
            continue;
        }

        // Candidate-duration-scoped: summarizeTopology/summarizeMotion see
        // only this candidate's own `frames` (this duration's usable
        // observations), never the full rolling history, so a shorter
        // candidate cannot observe motion or topology change from outside
        // its own [startTimestampUs, endTimestampUs] span.
        const auto topology = summarizeTopology(frames);
        const auto motion = summarizeMotion(frames, topology.palmTravel);

        GestureCandidateWindow window{};
        window.startTimestampUs = firstStored.observation.timestampUs;
        window.endTimestampUs = lastStored.observation.timestampUs;
        window.durationMs = durationMsActual;
        window.sourceFrameCount = count;
        window.usableFrameCount = usableCount;
        window.activeSegment = findActiveSegment(frames);
        window.visualization = buildGestureVisualizationSequence(usableObservations);
        window.topology = topology;
        window.motionScore = motion.motionScore;
        window.averagePalmVelocity = motion.averagePalmVelocity;
        window.peakPalmVelocity = motion.peakPalmVelocity;
        for (std::size_t index = 0; index < kUnifiedSequenceLength && index < unified.size(); ++index)
        {
            window.unifiedSequence[index] = unified[index];
        }
        window.valid = true;
        outWindows[written++] = window;
    }

    return written;
}
}

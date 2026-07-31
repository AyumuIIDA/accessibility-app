#include "HandInput/Measurements/hand_unified_feature_frame.h"
#include "Rendering/gesture_dtw_debug_render_packet_adapter.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace measurements = ryoiki::hand_input::measurements;
namespace recognition = ryoiki::hand_input::recognition;
namespace rendering = ryoiki::rendering;

namespace
{
void require(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error{message};
}

bool approximatelyEqual(const float left, const float right, const float epsilon = 1.0e-5F)
{
    return std::abs(left - right) <= epsilon;
}

measurements::UnifiedFeatureObservation observation(
    const std::uint64_t timestampUs, const float centerX, const float centerY)
{
    measurements::UnifiedFeatureObservation value{};
    value.timestampUs = timestampUs;
    for (std::size_t index = 0; index < value.imageLandmarks.size(); ++index)
    {
        value.imageLandmarks[index] = {
            centerX + static_cast<float>(index) - 5.0F,
            centerY + static_cast<float>(index) * 0.25F,
            static_cast<float>(index) * 0.5F};
    }
    value.imageLandmarks[0] = {centerX - 5.0F, centerY, 0.0F};
    value.imageLandmarks[9] = {centerX + 5.0F, centerY, 4.5F};
    return value;
}

void testPrNormalizationAndResampling()
{
    const std::vector source{
        observation(1'000'000, 100.0F, 200.0F),
        observation(2'000'000, 110.0F, 195.0F),
        observation(4'100'000, 130.0F, 210.0F)};
    const auto sequence = measurements::buildGestureVisualizationSequence(source);
    require(sequence.valid, "A non-empty recording must produce visualization data.");
    require(sequence.frames.size() == 32, "Visualization must use the DTW sequence length.");
    require(approximatelyEqual(sequence.frames.front().centerX, 0.0F)
            && approximatelyEqual(sequence.frames.front().centerY, 0.0F),
        "Palm trajectory must start at the PR-normalized origin.");
    require(approximatelyEqual(sequence.frames.back().centerX, 3.0F)
            && approximatelyEqual(sequence.frames.back().centerY, 1.0F),
        "Palm trajectory must be divided by the average wrist-middle scale.");
    require(approximatelyEqual(sequence.frames.front().skeleton[0].x, -0.5F)
            && approximatelyEqual(sequence.frames.front().skeleton[9].x, 0.5F),
        "Skeleton XY must be relative to the first palm centre.");
    require(approximatelyEqual(sequence.frames.front().skeleton[9].z, 0.45F),
        "Skeleton Z must be scaled but not translated, matching the PR.");
    require(std::abs(sequence.frames.back().timeOffsetMs - 3100.0) < 1.0e-6,
        "Resampling must preserve the source duration endpoint.");
}

void testEmptyAndSingleObservation()
{
    require(!measurements::buildGestureVisualizationSequence({}).valid,
        "An empty capture must not claim visualization data.");
    const auto sequence = measurements::buildGestureVisualizationSequence(
        {observation(7, 20.0F, 30.0F)});
    require(sequence.valid, "A single observation must be repeatable for diagnostics.");
    for (const auto& frame : sequence.frames)
    {
        require(approximatelyEqual(frame.centerX, 0.0F) && approximatelyEqual(frame.centerY, 0.0F),
            "Repeated single-frame centres must stay at the origin.");
    }
}

void testRenderPacketAdapterIsBoundedValueCopy()
{
    const auto visual = measurements::buildGestureVisualizationSequence({
        observation(0, 0.0F, 0.0F), observation(2'500'000, 20.0F, 0.0F)});
    measurements::GestureCandidateWindow candidate{};
    candidate.valid = true;
    candidate.durationMs = 2500.0;
    candidate.visualization = visual;
    candidate.unifiedSequence[7].palmVelocity = 1.25F;
    recognition::UnifiedSequenceTemplate templateSequence{};
    templateSequence.visualization = visual;
    templateSequence.frames[11].palmVelocity = 2.5F;
    recognition::ComparisonResult comparison{};
    comparison.eligible = true;
    for (std::size_t index = 0; index < 80; ++index)
    {
        comparison.path.push_back({index % 32, index % 32});
    }

    auto packet = rendering::buildGestureDtwDebugRenderPacket(
        44, 55, 6, 66, candidate, templateSequence, comparison);
    require(packet.eligible && packet.candidateFrameCount == 32
            && packet.templateFrameCount == 32,
        "Valid visualization operands must populate both render sequences.");
    require(packet.pathCount == rendering::kGestureDtwDebugPathCapacity,
        "An oversized diagnostic path must be bounded without allocation in the packet.");
    candidate.visualization.frames[0].centerX = 99.0F;
    require(approximatelyEqual(packet.candidateFrames[0].palmCenterX, 0.0F),
        "The packet must own a value copy independent of candidate lifetime.");
    require(approximatelyEqual(packet.candidateFrames.back().palmCenterX, 2.0F),
        "The renderer packet must preserve the PR-normalized palm trajectory.");
    require(approximatelyEqual(packet.candidateFrames[7].palmVelocity, 1.25F)
            && approximatelyEqual(packet.templateFrames[11].palmVelocity, 2.5F),
        "The adapter must preserve the palm-velocity scalar used by the Fig. 7 alignment view.");
}
}

int main()
{
    try
    {
        testPrNormalizationAndResampling();
        testEmptyAndSingleObservation();
        testRenderPacketAdapterIsBoundedValueCopy();
        std::cout << "Gesture DTW visualization packet tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

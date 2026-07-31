#include "Rendering/gesture_dtw_debug_render_packet_adapter.h"

#include <algorithm>

namespace ryoiki::rendering
{
namespace
{
void copySequence(
    const hand_input::measurements::GestureVisualizationSequence& visualization,
    const std::array<hand_input::measurements::UnifiedFeatureFrame,
        hand_input::measurements::kUnifiedSequenceLength>& features,
    std::array<GestureDtwDebugSequenceFrame, kGestureDtwDebugFrameCapacity>& destination) noexcept
{
    if (!visualization.valid) return;
    for (std::size_t frameIndex = 0; frameIndex < destination.size(); ++frameIndex)
    {
        const auto& source = visualization.frames[frameIndex];
        auto& target = destination[frameIndex];
        target.timeOffsetMs = source.timeOffsetMs;
        target.palmCenterX = source.centerX;
        target.palmCenterY = source.centerY;
        target.palmOrientationRadians = features[frameIndex].palmOrientationRadians;
        target.palmVelocity = features[frameIndex].palmVelocity;
        for (std::size_t pointIndex = 0; pointIndex < target.landmarks.size(); ++pointIndex)
        {
            const auto& point = source.skeleton[pointIndex];
            target.landmarks[pointIndex] = {point.x, point.y, point.z};
        }
    }
}
}

GestureDtwDebugRenderPacket buildGestureDtwDebugRenderPacket(
    const std::uint64_t frameId,
    const std::uint64_t timestampUs,
    const std::uint32_t trackId,
    const std::uint32_t templateId,
    const hand_input::measurements::GestureCandidateWindow& candidate,
    const hand_input::recognition::UnifiedSequenceTemplate& templateSequence,
    const hand_input::recognition::ComparisonResult& comparison) noexcept
{
    GestureDtwDebugRenderPacket packet{};
    packet.frameId = frameId;
    packet.timestampUs = timestampUs;
    packet.trackId = trackId;
    packet.templateId = templateId;
    packet.eligible = comparison.eligible;
    packet.score = comparison.score;
    packet.confidence = hand_input::recognition::scoreToConfidence(comparison.score);
    packet.candidateFrameCount = candidate.visualization.valid
        ? static_cast<std::uint32_t>(kGestureDtwDebugFrameCapacity) : 0;
    packet.templateFrameCount = templateSequence.visualization.valid
        ? static_cast<std::uint32_t>(kGestureDtwDebugFrameCapacity) : 0;
    copySequence(candidate.visualization, candidate.unifiedSequence, packet.candidateFrames);
    copySequence(templateSequence.visualization, templateSequence.frames, packet.templateFrames);

    const auto& breakdown = comparison.breakdown;
    packet.scoreBreakdown = {breakdown.jointScore, breakdown.boneScore,
        breakdown.curlScore, breakdown.fingerStateScore, breakdown.spacingScore,
        breakdown.motionScore, breakdown.palmTurnScore, breakdown.depthScore,
        breakdown.handednessScore, breakdown.sizeScore, breakdown.translationScore,
        breakdown.totalScore};
    packet.pathCount = static_cast<std::uint32_t>((std::min)(
        comparison.path.size(), packet.path.size()));
    for (std::size_t index = 0; index < packet.pathCount; ++index)
    {
        packet.path[index] = {
            static_cast<std::uint8_t>(comparison.path[index].candidateIndex),
            static_cast<std::uint8_t>(comparison.path[index].templateIndex)};
    }
    return packet;
}
}

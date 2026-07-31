#pragma once

#include "HandInput/Measurements/gesture_candidate_window_history.h"
#include "HandInput/Recognition/unified_sequence_gesture_comparator.h"
#include "Rendering/gesture_dtw_debug_render_packet.h"

#include <cstdint>

namespace ryoiki::rendering
{
// Converts recognition-owned values into the renderer's bounded wire type.
// The packet owns all data; no history/registry pointer crosses to the render
// thread. Call only when a debug renderer is attached so normal recognition
// does not pay the ~17 KiB copy cost.
[[nodiscard]] GestureDtwDebugRenderPacket buildGestureDtwDebugRenderPacket(
    std::uint64_t frameId,
    std::uint64_t timestampUs,
    std::uint32_t trackId,
    std::uint32_t templateId,
    const hand_input::measurements::GestureCandidateWindow& candidate,
    const hand_input::recognition::UnifiedSequenceTemplate& templateSequence,
    const hand_input::recognition::ComparisonResult& comparison) noexcept;
}

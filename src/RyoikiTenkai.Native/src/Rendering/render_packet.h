#pragma once

#include "Buffers/frame_buffer.h"
#include "HandInput/Recognition/hand_state.h"
#include "HandPerception/MediaPipeGraph/hand_perception_graph.h"

#include <cstdint>
#include <memory>

namespace ryoiki::rendering
{
struct RenderPacket
{
    std::shared_ptr<const buffers::FrameBuffer> frame;
    hand_perception::HandPerceptionResult perception;
    std::uint64_t perceptionFrameId{0};
    hand_input::recognition::HandStateResult domainSignState{};
};
}

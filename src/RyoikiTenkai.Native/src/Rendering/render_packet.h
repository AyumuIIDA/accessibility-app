#pragma once

#include "Buffers/frame_buffer.h"
#include "HandPerception/MediaPipeGraph/hand_perception_graph.h"

#include <memory>

namespace ryoiki::rendering
{
struct RenderPacket
{
    std::shared_ptr<const buffers::FrameBuffer> frame;
    hand_perception::HandPerceptionResult perception;
};
}

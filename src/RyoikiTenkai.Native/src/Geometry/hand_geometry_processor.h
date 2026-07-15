#pragma once

#include "Buffers/frame_buffer.h"
#include "Buffers/tensor_buffer.h"
#include "Geometry/geometry_types.h"

#include <memory>

namespace ryoiki::geometry
{
class HandGeometryProcessor final
{
public:
    static constexpr int kPalmInputSize = 192;
    static constexpr int kHandInputSize = 224;

    HandGeometryProcessor();
    ~HandGeometryProcessor();

    HandGeometryProcessor(const HandGeometryProcessor&) = delete;
    HandGeometryProcessor& operator=(const HandGeometryProcessor&) = delete;

    bool preprocessPalm(
        const buffers::FrameBuffer& frame,
        buffers::FloatTensorBuffer& tensor,
        PalmPreprocessResult& result);

    bool preprocessHand(
        const buffers::FrameBuffer& frame,
        const RotatedRegion& region,
        buffers::FloatTensorBuffer& tensor,
        HandPreprocessResult& result);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

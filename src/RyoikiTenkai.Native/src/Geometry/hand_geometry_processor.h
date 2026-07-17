#pragma once

#include "Buffers/frame_buffer.h"
#include "Buffers/tensor_buffer.h"
#include "Geometry/geometry_types.h"

#include <memory>

namespace ryoiki::geometry
{
class IGeometryProcessor
{
public:
    virtual ~IGeometryProcessor() = default;

    [[nodiscard]] virtual buffers::MemoryLocation inputMemoryLocation() const noexcept = 0;
    [[nodiscard]] virtual buffers::MemoryLocation outputMemoryLocation() const noexcept = 0;

    virtual bool preprocessPalm(
        const buffers::FrameBuffer& frame,
        buffers::FloatTensorBuffer& tensor,
        PalmPreprocessResult& result) = 0;

    virtual bool preprocessHand(
        const buffers::FrameBuffer& frame,
        const RotatedRegion& region,
        buffers::FloatTensorBuffer& tensor,
        HandPreprocessResult& result) = 0;
};

class HandGeometryProcessor final : public IGeometryProcessor
{
public:
    static constexpr int kPalmInputSize = 192;
    static constexpr int kHandInputSize = 224;

    HandGeometryProcessor();
    ~HandGeometryProcessor();

    HandGeometryProcessor(const HandGeometryProcessor&) = delete;
    HandGeometryProcessor& operator=(const HandGeometryProcessor&) = delete;

    [[nodiscard]] buffers::MemoryLocation inputMemoryLocation() const noexcept override;
    [[nodiscard]] buffers::MemoryLocation outputMemoryLocation() const noexcept override;

    bool preprocessPalm(
        const buffers::FrameBuffer& frame,
        buffers::FloatTensorBuffer& tensor,
        PalmPreprocessResult& result) override;

    bool preprocessHand(
        const buffers::FrameBuffer& frame,
        const RotatedRegion& region,
        buffers::FloatTensorBuffer& tensor,
        HandPreprocessResult& result) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

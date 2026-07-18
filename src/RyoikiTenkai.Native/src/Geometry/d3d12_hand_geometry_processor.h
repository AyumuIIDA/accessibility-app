#pragma once

#include "Geometry/hand_geometry_processor.h"
#include "Runtime/directml_runtime.h"

#include <memory>
#include <string>

namespace ryoiki::geometry
{
class D3d12HandGeometryProcessor final : public IGeometryProcessor
{
public:
    static std::unique_ptr<D3d12HandGeometryProcessor> create(
        std::shared_ptr<runtime::DirectMlRuntime> runtime,
        std::string& error);
    ~D3d12HandGeometryProcessor() override;

    [[nodiscard]] buffers::MemoryLocation inputMemoryLocation() const noexcept override;
    [[nodiscard]] buffers::MemoryLocation outputMemoryLocation() const noexcept override;
    bool preprocessPalm(const buffers::FrameBuffer& frame,
        buffers::FloatTensorBuffer& tensor, PalmPreprocessResult& result) override;
    bool preprocessHand(const buffers::FrameBuffer& frame, const RotatedRegion& region,
        buffers::FloatTensorBuffer& tensor, HandPreprocessResult& result) override;

private:
    class Impl;
    explicit D3d12HandGeometryProcessor(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
}

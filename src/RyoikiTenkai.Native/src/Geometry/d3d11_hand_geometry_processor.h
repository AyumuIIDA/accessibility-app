#pragma once

#include "Geometry/hand_geometry_processor.h"
#include "Runtime/d3d11_device.h"

#include <memory>
#include <string>

namespace ryoiki::runtime { class DirectMlRuntime; }

namespace ryoiki::geometry
{
class D3d11HandGeometryProcessor final : public IGeometryProcessor
{
public:
    static std::unique_ptr<D3d11HandGeometryProcessor> create(
        std::shared_ptr<runtime::D3d11Device> device,
        std::string& error);
    static std::unique_ptr<D3d11HandGeometryProcessor> create(
        std::shared_ptr<runtime::D3d11Device> device,
        std::shared_ptr<runtime::DirectMlRuntime> directMlRuntime,
        std::string& error);

    ~D3d11HandGeometryProcessor() override;

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
    [[nodiscard]] const std::string& lastError() const noexcept;

private:
    class Impl;
    explicit D3d11HandGeometryProcessor(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    std::string lastError_;
};
}

#pragma once

#include "Rendering/hand_3d_plot.h"
#include "Rendering/render_packet.h"
#include "Runtime/d3d11_device.h"

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <string>

namespace ryoiki::rendering
{
struct RenderPresentation
{
    std::uint64_t frameId{0};
    std::uint64_t captureTimestampUs{0};
    double cameraUploadMs{0.0};
    double cameraDrawMs{0.0};
    double overlayDrawMs{0.0};
    double hand3dDrawMs{0.0};
    double endDrawMs{0.0};
    double presentWaitMs{0.0};
    double renderMs{0.0};
    bool usedGpuCameraSurface{false};
};

class D3d11D2dRenderer final
{
public:
    D3d11D2dRenderer();
    ~D3d11D2dRenderer();
    D3d11D2dRenderer(const D3d11D2dRenderer&) = delete;
    D3d11D2dRenderer& operator=(const D3d11D2dRenderer&) = delete;

    [[nodiscard]] bool initialize(
        std::shared_ptr<runtime::D3d11Device> d3dDevice,
        HWND hwnd,
        std::uint32_t width,
        std::uint32_t height,
        std::string& error);
    [[nodiscard]] bool resize(
        std::uint32_t width,
        std::uint32_t height,
        std::string& error);
    void setHand3dView(Hand3dView view) noexcept;
    [[nodiscard]] bool render(
        const RenderPacket& packet,
        RenderPresentation& presentation,
        std::string& error);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}

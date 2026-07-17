#pragma once

#include "Rendering/render_packet.h"

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
    double renderMs{0.0};
};

class D3d11D2dRenderer final
{
public:
    D3d11D2dRenderer();
    ~D3d11D2dRenderer();
    D3d11D2dRenderer(const D3d11D2dRenderer&) = delete;
    D3d11D2dRenderer& operator=(const D3d11D2dRenderer&) = delete;

    [[nodiscard]] bool initialize(
        HWND hwnd,
        std::uint32_t width,
        std::uint32_t height,
        std::string& error);
    [[nodiscard]] bool resize(
        std::uint32_t width,
        std::uint32_t height,
        std::string& error);
    [[nodiscard]] bool render(
        const RenderPacket& packet,
        RenderPresentation& presentation,
        std::string& error);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}

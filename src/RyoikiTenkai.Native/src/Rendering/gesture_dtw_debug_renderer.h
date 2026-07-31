#pragma once

#include "Rendering/gesture_dtw_debug_render_packet.h"
#include "Runtime/d3d11_device.h"

#include <Windows.h>
#include <cstdint>
#include <memory>
#include <string>

namespace ryoiki::rendering
{
class GestureDtwDebugRenderer final
{
public:
    GestureDtwDebugRenderer();
    ~GestureDtwDebugRenderer();
    GestureDtwDebugRenderer(const GestureDtwDebugRenderer&) = delete;
    GestureDtwDebugRenderer& operator=(const GestureDtwDebugRenderer&) = delete;

    [[nodiscard]] bool initialize(std::shared_ptr<runtime::D3d11Device> device,
        HWND hwnd, std::uint32_t width, std::uint32_t height, std::string& error);
    [[nodiscard]] bool resize(std::uint32_t width, std::uint32_t height, std::string& error);
    [[nodiscard]] bool render(const GestureDtwDebugRenderPacket& packet, std::string& error);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}

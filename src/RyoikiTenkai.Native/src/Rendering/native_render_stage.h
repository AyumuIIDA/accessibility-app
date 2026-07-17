#pragma once

#include "Rendering/d3d11_d2d_renderer.h"
#include "Rendering/render_packet.h"

#include <Windows.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace ryoiki::rendering
{
class NativeRenderStage final
{
public:
    using PresentationCallback = std::function<void(const RenderPresentation&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    NativeRenderStage() = default;
    ~NativeRenderStage();
    NativeRenderStage(const NativeRenderStage&) = delete;
    NativeRenderStage& operator=(const NativeRenderStage&) = delete;

    [[nodiscard]] bool start(
        HWND hwnd,
        std::uint32_t width,
        std::uint32_t height,
        PresentationCallback presentationCallback,
        ErrorCallback errorCallback,
        std::string& error);
    void stop();
    void publish(RenderPacket packet);
    void resize(std::uint32_t width, std::uint32_t height);
    void requestRedraw();

private:
    struct PixelSize
    {
        std::uint32_t width{0};
        std::uint32_t height{0};
    };

    void run(HWND hwnd, std::uint32_t width, std::uint32_t height);

    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<RenderPacket> latestPacket_;
    std::optional<RenderPacket> lastPresentedPacket_;
    std::optional<PixelSize> pendingResize_;
    PresentationCallback presentationCallback_;
    ErrorCallback errorCallback_;
    std::thread worker_;
    bool redrawRequested_{false};
    bool stopping_{false};
    bool initialized_{false};
    std::string initializationError_;
};
}

#pragma once

#include "Rendering/d3d11_d2d_renderer.h"
#include "Rendering/render_packet.h"
#include "Runtime/d3d11_device.h"

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
        std::shared_ptr<runtime::D3d11Device> d3dDevice,
        HWND hwnd,
        std::uint32_t width,
        std::uint32_t height,
        PresentationCallback presentationCallback,
        ErrorCallback errorCallback,
        std::string& error);
    void stop();
    void publish(RenderPacket packet);
    void publishFrame(std::shared_ptr<const buffers::FrameBuffer> frame);
    void publishPerception(
        hand_perception::HandPerceptionResult perception,
        std::uint64_t sourceFrameId,
        hand_input::recognition::HandStateResult domainSignState);
    void resize(std::uint32_t width, std::uint32_t height);
    void updateHand3dView(Hand3dView view);
    void requestRedraw();

private:
    struct PixelSize
    {
        std::uint32_t width{0};
        std::uint32_t height{0};
    };

    void run(
        std::shared_ptr<runtime::D3d11Device> d3dDevice,
        HWND hwnd,
        std::uint32_t width,
        std::uint32_t height);

    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<RenderPacket> latestPacket_;
    std::optional<RenderPacket> lastPresentedPacket_;
    std::shared_ptr<const buffers::FrameBuffer> latestFrame_;
    hand_perception::HandPerceptionResult latestPerception_{};
    std::uint64_t latestPerceptionFrameId_{0};
    hand_input::recognition::HandStateResult latestDomainSignState_{};
    std::optional<PixelSize> pendingResize_;
    std::optional<Hand3dView> pendingHand3dView_;
    PresentationCallback presentationCallback_;
    ErrorCallback errorCallback_;
    std::thread worker_;
    bool redrawRequested_{false};
    bool stopping_{false};
    bool initialized_{false};
    std::string initializationError_;
};
}

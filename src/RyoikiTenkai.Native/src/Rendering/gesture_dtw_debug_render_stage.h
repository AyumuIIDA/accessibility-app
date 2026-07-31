#pragma once
#include "Rendering/gesture_dtw_debug_renderer.h"
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace ryoiki::rendering
{
class GestureDtwDebugRenderStage final
{
public:
    ~GestureDtwDebugRenderStage();
    [[nodiscard]] bool start(std::shared_ptr<runtime::D3d11Device> device, HWND hwnd,
        std::uint32_t width, std::uint32_t height, std::string& error);
    void stop();
    void publish(GestureDtwDebugRenderPacket packet);
    void resize(std::uint32_t width, std::uint32_t height);
    void redraw();
private:
    struct Size { std::uint32_t width; std::uint32_t height; };
    void run(std::shared_ptr<runtime::D3d11Device> device, HWND hwnd, Size size);
    std::mutex mutex_; std::condition_variable condition_;
    std::optional<GestureDtwDebugRenderPacket> latest_, last_;
    std::optional<Size> pendingSize_; std::thread worker_;
    bool redraw_{false}, stopping_{false}, initialized_{false}; std::string initializationError_;
};
}

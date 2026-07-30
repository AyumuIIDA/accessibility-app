#pragma once

#include "Rendering/cad_renderer.h"
#include "Runtime/d3d11_device.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace ryoiki::rendering
{
class CadRenderStage final
{
public:
    CadRenderStage() = default;
    ~CadRenderStage();
    CadRenderStage(const CadRenderStage&) = delete;
    CadRenderStage& operator=(const CadRenderStage&) = delete;

    [[nodiscard]] bool start(
        std::shared_ptr<runtime::D3d11Device> device,
        HWND hwnd,
        std::uint32_t width,
        std::uint32_t height,
        std::string& error);
    void stop();
    void resize(std::uint32_t width, std::uint32_t height);
    void setView(CadView view);
    void redraw();

private:
    struct Size { std::uint32_t width; std::uint32_t height; };
    void run(std::shared_ptr<runtime::D3d11Device> device, HWND hwnd, Size size);

    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<Size> pendingSize_;
    std::optional<CadView> pendingView_;
    std::thread worker_;
    bool redraw_{false};
    bool stopping_{false};
    bool initialized_{false};
    std::string initializationError_;
};
}

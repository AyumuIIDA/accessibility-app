#include "Rendering/native_render_stage.h"

#include <utility>

namespace ryoiki::rendering
{
NativeRenderStage::~NativeRenderStage()
{
    stop();
}

bool NativeRenderStage::start(
    const HWND hwnd,
    const std::uint32_t width,
    const std::uint32_t height,
    PresentationCallback presentationCallback,
    ErrorCallback errorCallback,
    std::string& error)
{
    stop();
    {
        std::lock_guard lock{mutex_};
        stopping_ = false;
        initialized_ = false;
        initializationError_.clear();
        latestPacket_.reset();
        lastPresentedPacket_.reset();
        pendingResize_.reset();
        redrawRequested_ = false;
        presentationCallback_ = std::move(presentationCallback);
        errorCallback_ = std::move(errorCallback);
    }
    worker_ = std::thread{&NativeRenderStage::run, this, hwnd, width, height};

    std::unique_lock lock{mutex_};
    condition_.wait(lock, [this]
    {
        return initialized_ || !initializationError_.empty();
    });
    if (!initializationError_.empty())
    {
        error = initializationError_;
        lock.unlock();
        stop();
        return false;
    }
    return true;
}

void NativeRenderStage::stop()
{
    {
        std::lock_guard lock{mutex_};
        stopping_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
    std::lock_guard lock{mutex_};
    latestPacket_.reset();
    lastPresentedPacket_.reset();
    pendingResize_.reset();
    presentationCallback_ = {};
    errorCallback_ = {};
    initialized_ = false;
}

void NativeRenderStage::publish(RenderPacket packet)
{
    {
        std::lock_guard lock{mutex_};
        if (stopping_ || !initialized_)
        {
            return;
        }
        latestPacket_ = std::move(packet);
    }
    condition_.notify_one();
}

void NativeRenderStage::resize(const std::uint32_t width, const std::uint32_t height)
{
    {
        std::lock_guard lock{mutex_};
        if (stopping_ || !initialized_)
        {
            return;
        }
        pendingResize_ = PixelSize{width, height};
        redrawRequested_ = true;
    }
    condition_.notify_one();
}

void NativeRenderStage::requestRedraw()
{
    {
        std::lock_guard lock{mutex_};
        if (stopping_ || !initialized_)
        {
            return;
        }
        redrawRequested_ = true;
    }
    condition_.notify_one();
}

void NativeRenderStage::run(
    const HWND hwnd,
    const std::uint32_t width,
    const std::uint32_t height)
{
    D3d11D2dRenderer renderer;
    std::string error;
    if (!renderer.initialize(hwnd, width, height, error))
    {
        std::lock_guard lock{mutex_};
        initializationError_ = error.empty()
            ? "DirectX renderer initialization failed." : error;
        condition_.notify_all();
        return;
    }
    {
        std::lock_guard lock{mutex_};
        initialized_ = true;
    }
    condition_.notify_all();

    bool suspended = width == 0 || height == 0;
    while (true)
    {
        std::optional<RenderPacket> packet;
        std::optional<PixelSize> resizeCommand;
        PresentationCallback presentationCallback;
        ErrorCallback errorCallback;
        {
            std::unique_lock lock{mutex_};
            condition_.wait(lock, [this]
            {
                return stopping_ || latestPacket_.has_value()
                    || pendingResize_.has_value() || redrawRequested_;
            });
            if (stopping_)
            {
                break;
            }

            resizeCommand = pendingResize_;
            pendingResize_.reset();
            if (latestPacket_.has_value())
            {
                lastPresentedPacket_ = std::move(latestPacket_);
                latestPacket_.reset();
            }
            if (lastPresentedPacket_.has_value()
                && (redrawRequested_ || !resizeCommand.has_value()
                    || lastPresentedPacket_->frame != nullptr))
            {
                packet = lastPresentedPacket_;
            }
            redrawRequested_ = false;
            presentationCallback = presentationCallback_;
            errorCallback = errorCallback_;
        }

        error.clear();
        if (resizeCommand.has_value())
        {
            suspended = resizeCommand->width == 0 || resizeCommand->height == 0;
            if (suspended)
            {
                continue;
            }
            if (!renderer.resize(resizeCommand->width, resizeCommand->height, error))
            {
                if (errorCallback)
                {
                    errorCallback(error);
                }
                continue;
            }
        }
        if (suspended || !packet.has_value())
        {
            continue;
        }

        RenderPresentation presentation{};
        if (!renderer.render(*packet, presentation, error))
        {
            if (errorCallback)
            {
                errorCallback(error);
            }
            continue;
        }
        if (presentationCallback)
        {
            presentationCallback(presentation);
        }
    }
}
}

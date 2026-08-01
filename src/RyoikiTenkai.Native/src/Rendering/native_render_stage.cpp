#include "Rendering/native_render_stage.h"

#include <utility>

namespace ryoiki::rendering
{
NativeRenderStage::~NativeRenderStage()
{
    stop();
}

bool NativeRenderStage::start(
    std::shared_ptr<runtime::D3d11Device> d3dDevice,
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
        latestFrame_.reset();
        latestPerception_ = {};
        latestPerceptionFrameId_ = 0;
        latestDomainSignState_ = {};
        pendingResize_.reset();
        pendingHand3dView_.reset();
        redrawRequested_ = false;
        presentationCallback_ = std::move(presentationCallback);
        errorCallback_ = std::move(errorCallback);
    }
    worker_ = std::thread{
        &NativeRenderStage::run, this, std::move(d3dDevice), hwnd, width, height};

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
    latestFrame_.reset();
    latestPerception_ = {};
    latestPerceptionFrameId_ = 0;
    latestDomainSignState_ = {};
    pendingResize_.reset();
    pendingHand3dView_.reset();
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
        latestFrame_ = packet.frame;
        latestPerception_ = packet.perception;
        latestPerceptionFrameId_ = packet.perceptionFrameId;
        latestDomainSignState_ = packet.domainSignState;
        latestPacket_ = std::move(packet);
    }
    condition_.notify_one();
}

void NativeRenderStage::publishFrame(
    std::shared_ptr<const buffers::FrameBuffer> frame)
{
    {
        std::lock_guard lock{mutex_};
        if (stopping_ || !initialized_ || frame == nullptr)
        {
            return;
        }
        latestFrame_ = std::move(frame);
        latestPacket_ = RenderPacket{
            latestFrame_,
            latestPerception_,
            latestPerceptionFrameId_,
            latestDomainSignState_};
    }
    condition_.notify_one();
}

void NativeRenderStage::publishPerception(
    hand_perception::HandPerceptionResult perception,
    const std::uint64_t sourceFrameId,
    const hand_input::recognition::HandStateResult domainSignState)
{
    {
        std::lock_guard lock{mutex_};
        if (stopping_ || !initialized_)
        {
            return;
        }
        latestPerception_ = std::move(perception);
        latestPerceptionFrameId_ = sourceFrameId;
        latestDomainSignState_ = domainSignState;
        if (latestFrame_ != nullptr)
        {
            latestPacket_ = RenderPacket{
                latestFrame_,
                latestPerception_,
                latestPerceptionFrameId_,
                latestDomainSignState_};
        }
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

void NativeRenderStage::updateHand3dView(const Hand3dView view)
{
    {
        std::lock_guard lock{mutex_};
        if (stopping_ || !initialized_)
        {
            return;
        }
        pendingHand3dView_ = view;
        redrawRequested_ = true;
    }
    condition_.notify_one();
}

void NativeRenderStage::run(
    std::shared_ptr<runtime::D3d11Device> d3dDevice,
    const HWND hwnd,
    const std::uint32_t width,
    const std::uint32_t height)
{
    D3d11D2dRenderer renderer;
    std::string error;
    if (!renderer.initialize(std::move(d3dDevice), hwnd, width, height, error))
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
        std::optional<Hand3dView> hand3dViewCommand;
        PresentationCallback presentationCallback;
        ErrorCallback errorCallback;
        {
            std::unique_lock lock{mutex_};
            condition_.wait(lock, [this]
            {
                return stopping_ || latestPacket_.has_value()
                    || pendingResize_.has_value() || pendingHand3dView_.has_value()
                    || redrawRequested_;
            });
            if (stopping_)
            {
                break;
            }

            resizeCommand = pendingResize_;
            pendingResize_.reset();
            hand3dViewCommand = pendingHand3dView_;
            pendingHand3dView_.reset();
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
        if (hand3dViewCommand.has_value())
        {
            renderer.setHand3dView(*hand3dViewCommand);
        }
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

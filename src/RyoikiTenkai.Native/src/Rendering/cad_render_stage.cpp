#include "Rendering/cad_render_stage.h"

#include <chrono>

namespace ryoiki::rendering
{
CadRenderStage::~CadRenderStage() { stop(); }

bool CadRenderStage::start(
    std::shared_ptr<runtime::D3d11Device> device,
    const HWND hwnd,
    const std::uint32_t width,
    const std::uint32_t height,
    std::string& error)
{
    stop();
    {
        std::lock_guard lock{mutex_};
        stopping_ = false;
        initialized_ = false;
        initializationError_.clear();
        pendingSize_.reset();
        pendingView_.reset();
        redraw_ = true;
    }
    worker_ = std::thread{&CadRenderStage::run, this, std::move(device), hwnd, Size{width, height}};
    std::unique_lock lock{mutex_};
    if (!condition_.wait_for(lock, std::chrono::seconds{5}, [this]
        { return initialized_ || !initializationError_.empty(); }))
    {
        error = "CAD renderer initialization timed out.";
        lock.unlock();
        stop();
        return false;
    }
    if (!initializationError_.empty())
    {
        error = initializationError_;
        lock.unlock();
        stop();
        return false;
    }
    return true;
}

void CadRenderStage::stop()
{
    {
        std::lock_guard lock{mutex_};
        stopping_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock{mutex_};
    initialized_ = false;
    pendingSize_.reset();
    pendingView_.reset();
}

void CadRenderStage::resize(const std::uint32_t width, const std::uint32_t height)
{
    {
        std::lock_guard lock{mutex_};
        if (!initialized_ || stopping_) return;
        pendingSize_ = Size{width, height};
        redraw_ = true;
    }
    condition_.notify_one();
}

void CadRenderStage::setView(const CadView view)
{
    {
        std::lock_guard lock{mutex_};
        if (!initialized_ || stopping_) return;
        pendingView_ = view;
        redraw_ = true;
    }
    condition_.notify_one();
}

void CadRenderStage::redraw()
{
    {
        std::lock_guard lock{mutex_};
        if (!initialized_ || stopping_) return;
        redraw_ = true;
    }
    condition_.notify_one();
}

void CadRenderStage::run(
    std::shared_ptr<runtime::D3d11Device> device,
    const HWND hwnd,
    const Size size)
{
    CadRenderer renderer;
    std::string error;
    try
    {
        if (!renderer.initialize(std::move(device), hwnd, size.width, size.height, error))
        {
            std::lock_guard lock{mutex_};
            initializationError_ = error.empty() ? "CAD renderer initialization failed." : error;
            condition_.notify_all();
            return;
        }
    }
    catch (const std::exception& exception)
    {
        std::lock_guard lock{mutex_};
        initializationError_ = exception.what();
        condition_.notify_all();
        return;
    }
    catch (...)
    {
        std::lock_guard lock{mutex_};
        initializationError_ = "Unknown CAD renderer initialization failure.";
        condition_.notify_all();
        return;
    }
    {
        std::lock_guard lock{mutex_};
        initialized_ = true;
    }
    condition_.notify_all();

    bool suspended = size.width == 0 || size.height == 0;
    while (true)
    {
        std::optional<Size> resizeCommand;
        std::optional<CadView> viewCommand;
        {
            std::unique_lock lock{mutex_};
            condition_.wait(lock, [this]
            {
                return stopping_ || redraw_ || pendingSize_.has_value() || pendingView_.has_value();
            });
            if (stopping_) break;
            resizeCommand = pendingSize_;
            viewCommand = pendingView_;
            pendingSize_.reset();
            pendingView_.reset();
            redraw_ = false;
        }
        if (viewCommand.has_value()) renderer.setView(*viewCommand);
        if (resizeCommand.has_value())
        {
            suspended = resizeCommand->width == 0 || resizeCommand->height == 0;
            if (!suspended && !renderer.resize(resizeCommand->width, resizeCommand->height, error))
            {
                continue;
            }
        }
        if (!suspended && !renderer.render(error))
        {
            continue;
        }
    }
}
}

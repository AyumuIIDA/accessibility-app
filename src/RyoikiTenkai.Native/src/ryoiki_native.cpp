#include "ryoiki_native.h"
#include "Buffers/frame_pool.h"
#include "HandPerception/MediaPipeGraph/hand_perception_graph.h"
#include "HandPerception/ModelRunners/cpu_hand_landmark_runner.h"
#include "HandPerception/ModelRunners/cpu_palm_detection_runner.h"
#include "Pipeline/perception_mailbox.h"
#include "Rendering/native_render_stage.h"
#include "camera_capture.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static_assert(sizeof(RyoikiMetrics) == 168);
static_assert(sizeof(RyoikiPalmResult) == 96);
static_assert(sizeof(RyoikiHandResult) == 296);

namespace
{
constexpr wchar_t kWindowClassName[] = L"RyoikiTenkaiNativeView";
void requestNativeRedraw(RyoikiHandle& handle);
void resizeNativeRenderer(RyoikiHandle& handle, std::uint32_t width, std::uint32_t height);

LRESULT CALLBACK NativeWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_NCCREATE:
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(
            hwnd,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        auto* handle = reinterpret_cast<RyoikiHandle*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (handle != nullptr)
        {
            requestNativeRedraw(*handle);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_SIZE:
    {
        auto* handle = reinterpret_cast<RyoikiHandle*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (handle != nullptr)
        {
            resizeNativeRenderer(
                *handle,
                static_cast<std::uint32_t>(LOWORD(lparam)),
                static_cast<std::uint32_t>(HIWORD(lparam)));
        }
        return 0;
    }
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, message, wparam, lparam);
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

bool registerWindowClass()
{
    static std::once_flag once;
    static bool registered = false;
    std::call_once(once, []
    {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = NativeWindowProc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = kWindowClassName;
        window_class.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        registered = RegisterClassW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    });

    return registered;
}

void copyString(const std::string& source, char* buffer, const std::int32_t buffer_length)
{
    if (buffer == nullptr || buffer_length <= 0)
    {
        return;
    }

    const auto max_length = static_cast<std::size_t>(buffer_length - 1);
    const auto copy_length = (std::min)(source.size(), max_length);
    std::memcpy(buffer, source.data(), copy_length);
    buffer[copy_length] = '\0';
}
}

struct RyoikiHandle
{
    explicit RyoikiHandle(HWND parent) : parentHwnd{parent}
    {
        metrics.abi_version = kRyoikiAbiVersion;
        metrics.struct_size = static_cast<std::uint32_t>(sizeof(RyoikiMetrics));
        palm.abi_version = kRyoikiAbiVersion;
        palm.struct_size = static_cast<std::uint32_t>(sizeof(RyoikiPalmResult));
        hand.abi_version = kRyoikiAbiVersion;
        hand.struct_size = static_cast<std::uint32_t>(sizeof(RyoikiHandResult));
    }

    HWND parentHwnd{};
    HWND childHwnd{};
    std::atomic<bool> running{false};
    std::thread captureWorker;
    std::thread perceptionWorker;
    mutable std::mutex stateMutex;
    mutable std::mutex captureMutex;
    RyoikiMetrics metrics{};
    RyoikiPalmResult palm{};
    RyoikiHandResult hand{};
    ryoiki::buffers::FramePool framePool{4};
    ryoiki::pipeline::PerceptionMailbox perceptionMailbox;
    ryoiki::rendering::NativeRenderStage renderStage;
    CameraCapture* activeCapture{nullptr};
    std::string lastError;
    std::chrono::steady_clock::time_point startedAt{};
    std::chrono::steady_clock::time_point lastDisplayAt{};
    std::uint64_t lastPresentedFrameId{0};

    void setError(const std::string& message)
    {
        std::lock_guard lock{stateMutex};
        lastError = message;
    }
};

namespace
{
void requestNativeRedraw(RyoikiHandle& handle)
{
    handle.renderStage.requestRedraw();
}

void resizeNativeRenderer(
    RyoikiHandle& handle,
    const std::uint32_t width,
    const std::uint32_t height)
{
    handle.renderStage.resize(width, height);
}

void recordPresentation(
    RyoikiHandle& handle,
    const ryoiki::rendering::RenderPresentation& presentation)
{
    using clock = std::chrono::steady_clock;
    if (presentation.frameId == handle.lastPresentedFrameId)
    {
        return;
    }

    const auto now = clock::now();
    const auto renderTimestampUs = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    handle.lastPresentedFrameId = presentation.frameId;

    std::lock_guard lock{handle.stateMutex};
    if (handle.lastDisplayAt != clock::time_point{})
    {
        const double displayDelta = std::chrono::duration<double>(
            now - handle.lastDisplayAt).count();
        handle.metrics.display_fps = displayDelta > 0.0 ? 1.0 / displayDelta : 0.0;
    }
    handle.lastDisplayAt = now;
    handle.metrics.frame_id = presentation.frameId;
    handle.metrics.capture_timestamp_us = presentation.captureTimestampUs;
    handle.metrics.overlay_render_ms = presentation.renderMs;
    if (renderTimestampUs >= static_cast<std::int64_t>(presentation.captureTimestampUs))
    {
        handle.metrics.end_to_end_latency_ms =
            (renderTimestampUs - static_cast<std::int64_t>(presentation.captureTimestampUs))
            / 1000.0;
    }
}

class ActiveCaptureRegistration final
{
public:
    ActiveCaptureRegistration(RyoikiHandle& handle, CameraCapture& capture) : handle_{handle}
    {
        std::lock_guard lock{handle_.captureMutex};
        handle_.activeCapture = &capture;
    }

    ~ActiveCaptureRegistration()
    {
        std::lock_guard lock{handle_.captureMutex};
        handle_.activeCapture = nullptr;
    }

    ActiveCaptureRegistration(const ActiveCaptureRegistration&) = delete;
    ActiveCaptureRegistration& operator=(const ActiveCaptureRegistration&) = delete;

private:
    RyoikiHandle& handle_;
};

void requestCaptureStop(RyoikiHandle& handle)
{
    std::lock_guard lock{handle.captureMutex};
    if (handle.activeCapture != nullptr)
    {
        handle.activeCapture->requestStop();
    }
}

std::filesystem::path getModelPath(const wchar_t* fileName)
{
    std::wstring executablePath(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr,
        executablePath.data(),
        static_cast<DWORD>(executablePath.size()));
    if (length == 0 || length >= executablePath.size())
    {
        return {};
    }
    executablePath.resize(length);
    return std::filesystem::path{executablePath}.parent_path()
        / L"models" / fileName;
}

void runCaptureLoop(RyoikiHandle& handle)
{
    using clock = std::chrono::steady_clock;
    CameraCapture camera;
    ActiveCaptureRegistration registration{handle, camera};
    std::string error;
    if (!camera.initialize(error))
    {
        handle.setError(error);
        handle.running.store(false);
        handle.renderStage.requestRedraw();
        return;
    }

    handle.startedAt = clock::now();
    auto lastCaptureAt = clock::time_point{};
    std::vector<std::uint8_t> droppedFrameBuffer;
    std::uint64_t frameId = 0;

    while (handle.running.load())
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        auto orientation = ryoiki::runtime::FrameRotation::None;
        double cameraWaitMs = 0.0;
        double frameCopyMs = 0.0;
        error.clear();
        auto frame = handle.framePool.tryAcquire();
        auto& captureBuffer = frame != nullptr
            ? frame->writablePixels()
            : droppedFrameBuffer;
        if (!camera.readFrame(
                captureBuffer,
                width,
                height,
                orientation,
                cameraWaitMs,
                frameCopyMs,
                error))
        {
            if (handle.running.load())
            {
                handle.setError(error);
            }
            break;
        }

        const auto now = clock::now();
        const auto runtime = std::chrono::duration<double>(now - handle.startedAt).count();
        const auto captureTimestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
        const double cameraFps = lastCaptureAt == clock::time_point{}
            ? 0.0
            : 1.0 / std::chrono::duration<double>(now - lastCaptureAt).count();
        lastCaptureAt = now;
        ++frameId;

        if (frame != nullptr
            && frame->prepare(
                width,
                height,
                frameId,
                static_cast<std::uint64_t>(captureTimestamp),
                orientation))
        {
            std::shared_ptr<const ryoiki::buffers::FrameBuffer> publishedFrame = frame;
            handle.perceptionMailbox.publish(std::move(publishedFrame));
        }

        const auto perceptionDrops = handle.perceptionMailbox.droppedFrames();
        {
            std::lock_guard lock{handle.stateMutex};
            handle.metrics.runtime_seconds = runtime;
            handle.metrics.camera_fps = cameraFps;
            handle.metrics.camera_wait_ms = cameraWaitMs;
            handle.metrics.frame_copy_ms = frameCopyMs;
            handle.metrics.native_overhead_ms = 0.0;
            handle.metrics.frame_pool_dropped_frames = handle.framePool.droppedAcquisitions();
            handle.metrics.perception_dropped_frames = perceptionDrops;

        }

    }

    handle.perceptionMailbox.stop();
    handle.running.store(false);
}

void runPerceptionLoop(RyoikiHandle& handle)
{
    using clock = std::chrono::steady_clock;
    std::string error;
    auto palmRunner = ryoiki::hand_perception::CpuPalmDetectionRunner::create(
        getModelPath(L"palm_detection.onnx"),
        error);
    if (palmRunner == nullptr)
    {
        handle.setError(error);
        handle.perceptionMailbox.stop();
        return;
    }
    auto handRunner = ryoiki::hand_perception::CpuHandLandmarkRunner::create(
        getModelPath(L"hand_landmark.onnx"),
        error);
    if (handRunner == nullptr)
    {
        handle.setError(error);
        handle.perceptionMailbox.stop();
        return;
    }

    ryoiki::hand_perception::HandPerceptionGraph graph{
        std::move(palmRunner),
        std::move(handRunner)};
    ryoiki::hand_perception::HandPerceptionResult perceptionResult;
    ryoiki::hand_perception::HandPerceptionGraphMetrics graphMetrics{};
    auto lastPerceptionAt = clock::time_point{};
    const std::string providerLog = "Hand perception runners selected: "
        + graph.providerSummary() + "\n";
    OutputDebugStringA(providerLog.c_str());

    while (handle.running.load())
    {
        const auto frame = handle.perceptionMailbox.waitForLatest();
        if (frame == nullptr)
        {
            break;
        }

        error.clear();
        if (!graph.process(*frame, perceptionResult, graphMetrics, error))
        {
            handle.setError(error);
            const std::string diagnostic = "Recoverable perception frame failure: "
                + error + "\n";
            OutputDebugStringA(diagnostic.c_str());
        }

        const auto completed = clock::now();
        const double perceptionFps = lastPerceptionAt == clock::time_point{}
            ? 0.0
            : 1.0 / std::chrono::duration<double>(completed - lastPerceptionAt).count();
        lastPerceptionAt = completed;
        const auto perceptionDrops = handle.perceptionMailbox.droppedFrames();
        {
            std::lock_guard lock{handle.stateMutex};
            handle.metrics.perception_fps = perceptionFps;
            handle.metrics.preprocess_ms = graphMetrics.palm.preprocessMs;
            handle.metrics.palm_inference_ms = graphMetrics.palm.inferenceMs;
            handle.metrics.palm_postprocess_ms = graphMetrics.palm.postprocessMs;
            handle.metrics.roi_crop_warp_ms = graphMetrics.hand.roiCropWarpMs;
            handle.metrics.hand_inference_ms = graphMetrics.hand.inferenceMs;
            handle.metrics.landmark_postprocess_ms = graphMetrics.hand.postprocessMs;
            handle.metrics.tracking_update_ms = graphMetrics.trackingUpdateMs;
            handle.metrics.perception_dropped_frames = perceptionDrops;
            handle.palm.frame_id = frame->frameId();
            handle.palm.palm_count = static_cast<std::int32_t>(perceptionResult.palms.size());
            handle.palm.confidence = 0.0F;
            std::memset(handle.palm.bbox, 0, sizeof(handle.palm.bbox));
            std::memset(handle.palm.keypoints, 0, sizeof(handle.palm.keypoints));
            const float inverseWidth = 1.0F / static_cast<float>(frame->uprightWidth());
            const float inverseHeight = 1.0F / static_cast<float>(frame->uprightHeight());
            if (perceptionResult.palms.size() > 0)
            {
                const auto& detection = perceptionResult.palms[0];
                handle.palm.confidence = detection.score;
                handle.palm.bbox[0] = std::clamp(detection.box.left * inverseWidth, 0.0F, 1.0F);
                handle.palm.bbox[1] = std::clamp(detection.box.top * inverseHeight, 0.0F, 1.0F);
                handle.palm.bbox[2] = std::clamp(detection.box.right * inverseWidth, 0.0F, 1.0F);
                handle.palm.bbox[3] = std::clamp(detection.box.bottom * inverseHeight, 0.0F, 1.0F);
                for (std::size_t index = 0; index < detection.keypoints.size(); ++index)
                {
                    handle.palm.keypoints[index * 2] = std::clamp(
                        detection.keypoints[index].x * inverseWidth, 0.0F, 1.0F);
                    handle.palm.keypoints[index * 2 + 1] = std::clamp(
                        detection.keypoints[index].y * inverseHeight, 0.0F, 1.0F);
                }
            }

            handle.hand.frame_id = frame->frameId();
            handle.hand.hand_count = perceptionResult.hand.detected ? 1 : 0;
            handle.hand.confidence = perceptionResult.hand.confidence;
            handle.hand.handedness = perceptionResult.hand.detected
                ? perceptionResult.hand.handedness : -1.0F;
            std::memset(handle.hand.bbox, 0, sizeof(handle.hand.bbox));
            std::memset(handle.hand.landmarks, 0, sizeof(handle.hand.landmarks));
            if (perceptionResult.hand.detected)
            {
                handle.hand.bbox[0] = std::clamp(
                    perceptionResult.hand.box.left * inverseWidth, 0.0F, 1.0F);
                handle.hand.bbox[1] = std::clamp(
                    perceptionResult.hand.box.top * inverseHeight, 0.0F, 1.0F);
                handle.hand.bbox[2] = std::clamp(
                    perceptionResult.hand.box.right * inverseWidth, 0.0F, 1.0F);
                handle.hand.bbox[3] = std::clamp(
                    perceptionResult.hand.box.bottom * inverseHeight, 0.0F, 1.0F);
                for (std::size_t index = 0; index < perceptionResult.hand.landmarks.size(); ++index)
                {
                    const auto& landmark = perceptionResult.hand.landmarks[index];
                    handle.hand.landmarks[index * 3] = std::clamp(
                        landmark.x * inverseWidth, 0.0F, 1.0F);
                    handle.hand.landmarks[index * 3 + 1] = std::clamp(
                        landmark.y * inverseHeight, 0.0F, 1.0F);
                    handle.hand.landmarks[index * 3 + 2] = landmark.z * inverseWidth;
                }
            }
        }

        handle.renderStage.publish({frame, perceptionResult});
    }
}
}

RYOIKI_EXPORT std::uint32_t ryoiki_get_abi_version()
{
    return kRyoikiAbiVersion;
}

RYOIKI_EXPORT RyoikiHandle* ryoiki_create(void* parent_hwnd)
{
    try
    {
        if (parent_hwnd == nullptr)
        {
            return nullptr;
        }

        if (!registerWindowClass())
        {
            return nullptr;
        }

        const auto parent = static_cast<HWND>(parent_hwnd);
        auto* handle = new RyoikiHandle{parent};
        RECT parentRect{};
        GetClientRect(parent, &parentRect);
        handle->childHwnd = CreateWindowExW(
            0,
            kWindowClassName,
            L"",
            WS_CHILD | WS_VISIBLE,
            0,
            0,
            (std::max)(1L, parentRect.right - parentRect.left),
            (std::max)(1L, parentRect.bottom - parentRect.top),
            parent,
            nullptr,
            GetModuleHandleW(nullptr),
            handle);

        if (handle->childHwnd == nullptr)
        {
            handle->setError("Failed to create native child window.");
            delete handle;
            return nullptr;
        }

        return handle;
    }
    catch (...)
    {
        return nullptr;
    }
}

RYOIKI_EXPORT std::int32_t ryoiki_start(RyoikiHandle* handle)
{
    if (handle == nullptr)
    {
        return kRyoikiStatusFailure;
    }

    try
    {
        bool expected = false;
        if (!handle->running.compare_exchange_strong(expected, true))
        {
            return kRyoikiStatusSuccess;
        }

        if (handle->captureWorker.joinable())
        {
            handle->captureWorker.join();
        }
        if (handle->perceptionWorker.joinable())
        {
            handle->perceptionWorker.join();
        }
        handle->renderStage.stop();

        {
            std::lock_guard lock{handle->stateMutex};
            handle->metrics = {};
            handle->metrics.abi_version = kRyoikiAbiVersion;
            handle->metrics.struct_size = static_cast<std::uint32_t>(sizeof(RyoikiMetrics));
            handle->palm = {};
            handle->palm.abi_version = kRyoikiAbiVersion;
            handle->palm.struct_size = static_cast<std::uint32_t>(sizeof(RyoikiPalmResult));
            handle->hand = {};
            handle->hand.abi_version = kRyoikiAbiVersion;
            handle->hand.struct_size = static_cast<std::uint32_t>(sizeof(RyoikiHandResult));
            handle->lastError.clear();
            handle->lastDisplayAt = {};
            handle->lastPresentedFrameId = 0;
        }
        handle->framePool.resetStatistics();
        handle->perceptionMailbox.reset();

        RECT childRect{};
        GetClientRect(handle->childHwnd, &childRect);
        std::string renderError;
        if (!handle->renderStage.start(
                handle->childHwnd,
                static_cast<std::uint32_t>((std::max)(1L, childRect.right - childRect.left)),
                static_cast<std::uint32_t>((std::max)(1L, childRect.bottom - childRect.top)),
                [handle](const ryoiki::rendering::RenderPresentation& presentation)
                {
                    recordPresentation(*handle, presentation);
                },
                [handle](const std::string& error)
                {
                    handle->setError(error);
                },
                renderError))
        {
            handle->running.store(false);
            handle->setError(renderError);
            return kRyoikiStatusFailure;
        }

        handle->perceptionWorker = std::thread{[handle]
        {
            try
            {
                runPerceptionLoop(*handle);
            }
            catch (const std::exception& exception)
            {
                handle->setError(exception.what());
                handle->running.store(false);
                handle->perceptionMailbox.stop();
                requestCaptureStop(*handle);
            }
            catch (...)
            {
                handle->setError("Unknown native perception worker failure.");
                handle->running.store(false);
                handle->perceptionMailbox.stop();
                requestCaptureStop(*handle);
            }
        }};
        handle->captureWorker = std::thread{[handle]
        {
            try
            {
                runCaptureLoop(*handle);
            }
            catch (const std::exception& exception)
            {
                handle->setError(exception.what());
                handle->running.store(false);
                handle->perceptionMailbox.stop();
                handle->renderStage.requestRedraw();
            }
            catch (...)
            {
                handle->setError("Unknown native camera worker failure.");
                handle->running.store(false);
                handle->perceptionMailbox.stop();
                handle->renderStage.requestRedraw();
            }
        }};
        return kRyoikiStatusSuccess;
    }
    catch (const std::exception& exception)
    {
        handle->running.store(false);
        handle->perceptionMailbox.stop();
        requestCaptureStop(*handle);
        if (handle->perceptionWorker.joinable())
        {
            handle->perceptionWorker.join();
        }
        handle->renderStage.stop();
        handle->setError(exception.what());
        return kRyoikiStatusFailure;
    }
    catch (...)
    {
        handle->running.store(false);
        handle->perceptionMailbox.stop();
        requestCaptureStop(*handle);
        if (handle->perceptionWorker.joinable())
        {
            handle->perceptionWorker.join();
        }
        handle->renderStage.stop();
        handle->setError("Unknown native start failure.");
        return kRyoikiStatusFailure;
    }
}

RYOIKI_EXPORT void ryoiki_stop(RyoikiHandle* handle)
{
    if (handle == nullptr)
    {
        return;
    }

    try
    {
        handle->running.store(false);
        handle->perceptionMailbox.stop();
        requestCaptureStop(*handle);
        if (handle->captureWorker.joinable())
        {
            handle->captureWorker.join();
        }
        if (handle->perceptionWorker.joinable())
        {
            handle->perceptionWorker.join();
        }
        handle->renderStage.stop();
    }
    catch (...)
    {
        handle->running.store(false);
    }
}

RYOIKI_EXPORT std::int32_t ryoiki_resize(
    RyoikiHandle* handle,
    const std::int32_t width,
    const std::int32_t height)
{
    if (handle == nullptr || handle->childHwnd == nullptr || width <= 0 || height <= 0)
    {
        return kRyoikiStatusFailure;
    }

    return MoveWindow(handle->childHwnd, 0, 0, width, height, TRUE) != FALSE
        ? kRyoikiStatusSuccess
        : kRyoikiStatusFailure;
}

RYOIKI_EXPORT void ryoiki_destroy(RyoikiHandle* handle)
{
    if (handle == nullptr)
    {
        return;
    }

    ryoiki_stop(handle);
    if (handle->childHwnd != nullptr)
    {
        DestroyWindow(handle->childHwnd);
        handle->childHwnd = nullptr;
    }

    delete handle;
}

RYOIKI_EXPORT std::int32_t ryoiki_get_latest_metrics(
    RyoikiHandle* handle,
    RyoikiMetrics* out_metrics)
{
    if (handle == nullptr || out_metrics == nullptr)
    {
        return kRyoikiStatusFailure;
    }

    try
    {
        std::lock_guard lock{handle->stateMutex};
        *out_metrics = handle->metrics;
        return kRyoikiStatusSuccess;
    }
    catch (...)
    {
        return kRyoikiStatusFailure;
    }
}

RYOIKI_EXPORT std::int32_t ryoiki_get_latest_hand(
    RyoikiHandle* handle,
    RyoikiHandResult* out_result)
{
    if (handle == nullptr || out_result == nullptr)
    {
        return kRyoikiStatusFailure;
    }

    try
    {
        std::lock_guard lock{handle->stateMutex};
        *out_result = handle->hand;
        return kRyoikiStatusSuccess;
    }
    catch (...)
    {
        return kRyoikiStatusFailure;
    }
}

RYOIKI_EXPORT std::int32_t ryoiki_get_latest_palm(
    RyoikiHandle* handle,
    RyoikiPalmResult* out_result)
{
    if (handle == nullptr || out_result == nullptr)
    {
        return kRyoikiStatusFailure;
    }

    try
    {
        std::lock_guard lock{handle->stateMutex};
        *out_result = handle->palm;
        return kRyoikiStatusSuccess;
    }
    catch (...)
    {
        return kRyoikiStatusFailure;
    }
}

RYOIKI_EXPORT std::int32_t ryoiki_get_last_error(
    RyoikiHandle* handle,
    char* buffer,
    const std::int32_t buffer_length)
{
    if (handle == nullptr || buffer == nullptr || buffer_length <= 0)
    {
        return kRyoikiStatusFailure;
    }

    try
    {
        std::lock_guard lock{handle->stateMutex};
        copyString(handle->lastError, buffer, buffer_length);
        return kRyoikiStatusSuccess;
    }
    catch (...)
    {
        return kRyoikiStatusFailure;
    }
}

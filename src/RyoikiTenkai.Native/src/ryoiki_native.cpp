#include "ryoiki_native.h"
#include "Buffers/frame_pool.h"
#include "HandPerception/MediaPipeGraph/hand_perception_graph.h"
#include "HandPerception/ModelRunners/cpu_hand_landmark_runner.h"
#include "HandPerception/ModelRunners/cpu_palm_detection_runner.h"
#include "Pipeline/perception_mailbox.h"
#include "Rendering/latest_render_packet_slot.h"
#include "camera_capture.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
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
void paintNativeWindow(RyoikiHandle& handle, HDC dc, const RECT& rect);

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
        const HDC dc = BeginPaint(hwnd, &paint);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        auto* handle = reinterpret_cast<RyoikiHandle*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (handle != nullptr)
        {
            paintNativeWindow(*handle, dc, rect);
        }
        else
        {
            FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }

        EndPaint(hwnd, &paint);
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

namespace
{
class GdiBackBuffer final
{
public:
    GdiBackBuffer() = default;
    ~GdiBackBuffer()
    {
        if (dc_ != nullptr && defaultBitmap_ != nullptr)
        {
            SelectObject(dc_, defaultBitmap_);
        }
        if (bitmap_ != nullptr)
        {
            DeleteObject(bitmap_);
        }
        if (dc_ != nullptr)
        {
            DeleteDC(dc_);
        }
    }

    GdiBackBuffer(const GdiBackBuffer&) = delete;
    GdiBackBuffer& operator=(const GdiBackBuffer&) = delete;

    [[nodiscard]] bool ensure(const HDC referenceDc, const int width, const int height)
    {
        if (width <= 0 || height <= 0)
        {
            return false;
        }
        if (dc_ != nullptr && bitmap_ != nullptr && width_ == width && height_ == height)
        {
            return true;
        }
        if (dc_ == nullptr)
        {
            dc_ = CreateCompatibleDC(referenceDc);
            if (dc_ == nullptr)
            {
                return false;
            }
        }

        const HBITMAP replacement = CreateCompatibleBitmap(referenceDc, width, height);
        if (replacement == nullptr)
        {
            return false;
        }
        const auto previous = SelectObject(dc_, replacement);
        if (defaultBitmap_ == nullptr)
        {
            defaultBitmap_ = previous;
        }
        if (bitmap_ != nullptr)
        {
            DeleteObject(bitmap_);
        }
        bitmap_ = replacement;
        width_ = width;
        height_ = height;
        return true;
    }

    [[nodiscard]] HDC dc() const noexcept { return dc_; }

private:
    HDC dc_{nullptr};
    HBITMAP bitmap_{nullptr};
    HGDIOBJ defaultBitmap_{nullptr};
    int width_{0};
    int height_{0};
};
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
    ryoiki::rendering::LatestRenderPacketSlot renderPackets;
    GdiBackBuffer backBuffer;
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

void drawPalmOverlay(
    const HDC dc,
    const int targetX,
    const int targetY,
    const int targetWidth,
    const int targetHeight,
    const std::uint32_t sourceWidth,
    const std::uint32_t sourceHeight,
    const ryoiki::hand_perception::PalmDetectionResult& palms)
{
    if (palms.size() == 0 || sourceWidth == 0 || sourceHeight == 0)
    {
        return;
    }

    const auto& palm = palms[0];
    const auto mapX = [targetX, targetWidth, sourceWidth](const float sourceX)
    {
        const float normalizedX = sourceX / static_cast<float>(sourceWidth);
        return targetX + static_cast<int>(
            (1.0F - std::clamp(normalizedX, 0.0F, 1.0F)) * targetWidth);
    };
    const auto mapY = [targetY, targetHeight, sourceHeight](const float sourceY)
    {
        const float normalizedY = sourceY / static_cast<float>(sourceHeight);
        return targetY + static_cast<int>(
            std::clamp(normalizedY, 0.0F, 1.0F) * targetHeight);
    };

    const int left = mapX(palm.box.right);
    const int right = mapX(palm.box.left);
    const int top = mapY(palm.box.top);
    const int bottom = mapY(palm.box.bottom);
    const auto pen = CreatePen(PS_SOLID, 3, RGB(0, 255, 180));
    const auto oldPen = SelectObject(dc, pen);
    const auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, left, top, right, bottom);

    for (std::size_t index = 0; index < 7; ++index)
    {
        const int x = mapX(palm.keypoints[index].x);
        const int y = mapY(palm.keypoints[index].y);
        Ellipse(dc, x - 3, y - 3, x + 4, y + 4);
    }

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void drawHandOverlay(
    const HDC dc,
    const int targetX,
    const int targetY,
    const int targetWidth,
    const int targetHeight,
    const std::uint32_t sourceWidth,
    const std::uint32_t sourceHeight,
    const ryoiki::hand_perception::HandLandmarkResult& hand)
{
    if (!hand.detected || sourceWidth == 0 || sourceHeight == 0)
    {
        return;
    }

    const auto mapPoint = [
        targetX,
        targetY,
        targetWidth,
        targetHeight,
        sourceWidth,
        sourceHeight,
        &hand](const int index)
    {
        const float x = std::clamp(
            hand.landmarks[index].x / static_cast<float>(sourceWidth), 0.0F, 1.0F);
        const float y = std::clamp(
            hand.landmarks[index].y / static_cast<float>(sourceHeight), 0.0F, 1.0F);
        return POINT{
            targetX + static_cast<LONG>((1.0F - x) * targetWidth),
            targetY + static_cast<LONG>(y * targetHeight)};
    };
    constexpr std::array<std::array<int, 2>, 23> kConnections{{
        {0, 1}, {1, 2}, {2, 3}, {3, 4},
        {0, 5}, {5, 6}, {6, 7}, {7, 8},
        {0, 9}, {9, 10}, {10, 11}, {11, 12},
        {0, 13}, {13, 14}, {14, 15}, {15, 16},
        {0, 17}, {17, 18}, {18, 19}, {19, 20},
        {5, 9}, {9, 13}, {13, 17}
    }};

    const auto pen = CreatePen(PS_SOLID, 2, RGB(255, 210, 70));
    const auto oldPen = SelectObject(dc, pen);
    const auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    for (const auto& connection : kConnections)
    {
        const auto start = mapPoint(connection[0]);
        const auto end = mapPoint(connection[1]);
        MoveToEx(dc, start.x, start.y, nullptr);
        LineTo(dc, end.x, end.y);
    }
    for (int index = 0; index < 21; ++index)
    {
        const auto point = mapPoint(index);
        Ellipse(dc, point.x - 3, point.y - 3, point.x + 4, point.y + 4);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void paintNativeWindow(RyoikiHandle& handle, const HDC dc, const RECT& rect)
{
    using clock = std::chrono::steady_clock;
    const auto renderStarted = clock::now();
    const int viewWidth = rect.right - rect.left;
    const int viewHeight = rect.bottom - rect.top;
    if (viewWidth <= 0 || viewHeight <= 0)
    {
        return;
    }

    const bool buffered = handle.backBuffer.ensure(dc, viewWidth, viewHeight);
    const HDC renderDc = buffered ? handle.backBuffer.dc() : dc;
    RECT renderRect{0, 0, viewWidth, viewHeight};
    SetDCBrushColor(renderDc, RGB(5, 6, 8));
    FillRect(renderDc, &renderRect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));

    std::uint64_t frameId = 0;
    std::uint64_t captureTimestampUs = 0;
    bool frameDrawn = false;

    const auto packet = handle.renderPackets.snapshot();
    if (packet.has_value() && packet->frame != nullptr)
    {
        const auto& frame = packet->frame;
        const auto& pixels = frame->pixels();
        if (!pixels.empty() && frame->width() > 0 && frame->height() > 0)
        {
            const double scale = (std::min)(
                viewWidth / static_cast<double>(frame->width()),
                viewHeight / static_cast<double>(frame->height()));
            const int targetWidth = (std::max)(1, static_cast<int>(frame->width() * scale));
            const int targetHeight = (std::max)(1, static_cast<int>(frame->height() * scale));
            const int targetX = (viewWidth - targetWidth) / 2;
            const int targetY = (viewHeight - targetHeight) / 2;

            BITMAPINFO bitmapInfo{};
            bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(frame->width());
            bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(frame->height());
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;

            SetStretchBltMode(renderDc, HALFTONE);
            SetBrushOrgEx(renderDc, 0, 0, nullptr);
            StretchDIBits(
                renderDc,
                targetX + targetWidth,
                targetY,
                -targetWidth,
                targetHeight,
                0,
                0,
                static_cast<int>(frame->width()),
                static_cast<int>(frame->height()),
                pixels.data(),
                &bitmapInfo,
                DIB_RGB_COLORS,
                SRCCOPY);

            drawPalmOverlay(
                renderDc,
                targetX,
                targetY,
                targetWidth,
                targetHeight,
                frame->width(),
                frame->height(),
                packet->perception.palms);
            drawHandOverlay(
                renderDc,
                targetX,
                targetY,
                targetWidth,
                targetHeight,
                frame->width(),
                frame->height(),
                packet->perception.hand);

            frameId = frame->frameId();
            captureTimestampUs = frame->captureTimestampUs();
            frameDrawn = true;
        }
    }

    if (!frameDrawn)
    {
        std::string error;
        {
            std::lock_guard lock{handle.stateMutex};
            error = handle.lastError;
        }

        SetBkMode(renderDc, TRANSPARENT);
        SetTextColor(renderDc, error.empty() ? RGB(0, 255, 180) : RGB(255, 120, 120));
        const wchar_t* message = error.empty()
            ? L"Starting native camera..."
            : L"Native camera unavailable. See application log.";
        DrawTextW(renderDc, message, -1, &renderRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (buffered)
    {
        BitBlt(dc, 0, 0, viewWidth, viewHeight, renderDc, 0, 0, SRCCOPY);
    }

    const auto renderCompleted = clock::now();
    const auto renderMs = std::chrono::duration<double, std::milli>(
        renderCompleted - renderStarted).count();
    const auto renderTimestampUs = std::chrono::duration_cast<std::chrono::microseconds>(
        renderCompleted.time_since_epoch()).count();

    if (!frameDrawn || frameId == handle.lastPresentedFrameId)
    {
        return;
    }
    handle.lastPresentedFrameId = frameId;

    std::lock_guard lock{handle.stateMutex};
    if (handle.lastDisplayAt != clock::time_point{})
    {
        const double displayDelta = std::chrono::duration<double>(
            renderCompleted - handle.lastDisplayAt).count();
        handle.metrics.display_fps = displayDelta > 0.0 ? 1.0 / displayDelta : 0.0;
    }
    handle.lastDisplayAt = renderCompleted;
    handle.metrics.frame_id = frameId;
    handle.metrics.capture_timestamp_us = captureTimestampUs;
    handle.metrics.overlay_render_ms = renderMs;
    if (renderTimestampUs >= static_cast<std::int64_t>(captureTimestampUs))
    {
        handle.metrics.end_to_end_latency_ms =
            (renderTimestampUs - static_cast<std::int64_t>(captureTimestampUs)) / 1000.0;
    }
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
        InvalidateRect(handle.childHwnd, nullptr, FALSE);
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
                static_cast<std::uint64_t>(captureTimestamp)))
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
            const float inverseWidth = 1.0F / static_cast<float>(frame->width());
            const float inverseHeight = 1.0F / static_cast<float>(frame->height());
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

        handle.renderPackets.publish({frame, perceptionResult});
        if (handle.childHwnd != nullptr)
        {
            InvalidateRect(handle.childHwnd, nullptr, FALSE);
        }
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
        handle->renderPackets.clear();
        handle->perceptionMailbox.reset();

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
                InvalidateRect(handle->childHwnd, nullptr, FALSE);
            }
            catch (...)
            {
                handle->setError("Unknown native camera worker failure.");
                handle->running.store(false);
                handle->perceptionMailbox.stop();
                InvalidateRect(handle->childHwnd, nullptr, FALSE);
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
        handle->renderPackets.clear();
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

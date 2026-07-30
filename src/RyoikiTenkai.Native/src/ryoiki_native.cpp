#include "ryoiki_native.h"
#include "Buffers/frame_pool.h"
#include "Geometry/d3d11_hand_geometry_processor.h"
#include "Features/Cad/cad_interaction_endpoint.h"
#include "HandPerception/MediaPipeGraph/hand_perception_graph.h"
#include "HandPerception/ModelRunners/ort_hand_landmark_runner.h"
#include "HandPerception/ModelRunners/ort_palm_detection_runner.h"
#include "HandInput/Measurements/hand_measurement_extractor.h"
#include "HandInput/Measurements/palm_basis_rotation_tracker.h"
#include "HandInput/Measurements/palm_rotation_eskf.h"
#include "HandInput/Measurements/weighted_palm_rotation_tracker.h"
#include "HandInput/Publication/latest_hand_state_slot.h"
#include "HandInput/Publication/ordered_hand_event_ring.h"
#include "HandInput/Recognition/domain_expansion_state_recognizer.h"
#include "HandInput/Recognition/open_palm_state_recognizer.h"
#include "HandInput/Recognition/swipe_event_recognizer.h"
#include "Pipeline/perception_mailbox.h"
#include "Rendering/hand_3d_plot.h"
#include "Rendering/native_render_stage.h"
#if defined(RYOIKI_ORT_DIRECTML)
#include "Geometry/d3d12_hand_geometry_processor.h"
#include "Runtime/directml_runtime.h"
#endif
#include "camera_capture.h"

#include <Windows.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static_assert(sizeof(RyoikiMetrics) == 240);
static_assert(sizeof(RyoikiPalmResult) == 96);
static_assert(sizeof(RyoikiHandResult) == 720);
static_assert(sizeof(RyoikiCadHandInteractionResult) == 56);
static_assert(sizeof(RyoikiHandEventBatch) == 1312);

namespace
{
constexpr wchar_t kWindowClassName[] = L"RyoikiTenkaiNativeView";
void requestNativeRedraw(RyoikiHandle& handle);
void resizeNativeRenderer(RyoikiHandle& handle, std::uint32_t width, std::uint32_t height);
bool beginHand3dViewDrag(RyoikiHandle& handle, HWND hwnd, int x, int y);
bool updateHand3dViewDrag(RyoikiHandle& handle, int x, int y);
void endHand3dViewDrag(RyoikiHandle& handle, HWND hwnd);
bool zoomHand3dView(RyoikiHandle& handle, HWND hwnd, int x, int y, int wheelDelta);
bool resetHand3dView(RyoikiHandle& handle, HWND hwnd, int x, int y);

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
    case WM_LBUTTONDOWN:
    {
        auto* handle = reinterpret_cast<RyoikiHandle*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (handle != nullptr && beginHand3dViewDrag(
                *handle, hwnd, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)))
        {
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    case WM_MOUSEMOVE:
    {
        auto* handle = reinterpret_cast<RyoikiHandle*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (handle != nullptr && updateHand3dViewDrag(
                *handle, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)))
        {
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
    {
        auto* handle = reinterpret_cast<RyoikiHandle*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (handle != nullptr)
        {
            endHand3dViewDrag(*handle, hwnd);
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
    {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd, &point);
        auto* handle = reinterpret_cast<RyoikiHandle*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (handle != nullptr && zoomHand3dView(
                *handle, hwnd, point.x, point.y, GET_WHEEL_DELTA_WPARAM(wparam)))
        {
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    case WM_LBUTTONDBLCLK:
    {
        auto* handle = reinterpret_cast<RyoikiHandle*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (handle != nullptr && resetHand3dView(
                *handle, hwnd, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)))
        {
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
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
        window_class.style = CS_DBLCLKS;
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

std::array<float, 9> relativeRotationDifference(
    const std::array<float, 9>& reference,
    const std::array<float, 9>& comparison) noexcept
{
    std::array<float, 9> result{};
    for (std::size_t column = 0; column < 3; ++column)
    {
        for (std::size_t row = 0; row < 3; ++row)
        {
            for (std::size_t inner = 0; inner < 3; ++inner)
            {
                result[column * 3 + row] +=
                    reference[row * 3 + inner] * comparison[column * 3 + inner];
            }
        }
    }
    return result;
}

std::array<float, 3> rotationVectorDegrees(
    const std::array<float, 9>& rotation) noexcept
{
    constexpr float kRadiansToDegrees = 180.0F / 3.14159265358979323846F;
    const float cosine = std::clamp(
        (rotation[0] + rotation[4] + rotation[8] - 1.0F) * 0.5F, -1.0F, 1.0F);
    const float angle = std::acos(cosine);
    const std::array<float, 3> skew{
        rotation[5] - rotation[7],
        rotation[6] - rotation[2],
        rotation[1] - rotation[3]};
    float scale = 0.5F;
    if (angle >= 1.0e-4F)
    {
        const float sine = std::sin(angle);
        if (std::abs(sine) <= 1.0e-5F) return {0.0F, 0.0F, angle * kRadiansToDegrees};
        scale = angle / (2.0F * sine);
    }
    return {
        skew[0] * scale * kRadiansToDegrees,
        skew[1] * scale * kRadiansToDegrees,
        skew[2] * scale * kRadiansToDegrees};
}

float rotationAngleDegrees(const std::array<float, 9>& rotation) noexcept
{
    constexpr float kRadiansToDegrees = 180.0F / 3.14159265358979323846F;
    return std::acos(std::clamp(
        (rotation[0] + rotation[4] + rotation[8] - 1.0F) * 0.5F, -1.0F, 1.0F))
        * kRadiansToDegrees;
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
    mutable std::mutex cadInteractionMutex;
    ryoiki::features::cad::HandInteractionMode cadInteractionMode{
        ryoiki::features::cad::HandInteractionMode::None};
    RyoikiMetrics metrics{};
    RyoikiPalmResult palm{};
    RyoikiHandResult hand{};
    ryoiki::buffers::FramePool framePool{4};
    ryoiki::buffers::FramePool renderFramePool{4};
    ryoiki::pipeline::PerceptionMailbox perceptionMailbox;
    ryoiki::hand_input::measurements::HandMeasurementExtractor handMeasurementExtractor;
    ryoiki::hand_input::measurements::WeightedPalmRotationTracker palmRotationTracker;
    ryoiki::hand_input::measurements::WeightedPalmRotationTracker
        incrementalPalmRotationTracker;
    ryoiki::hand_input::measurements::PalmRotationEskf palmRotationEskf;
    ryoiki::hand_input::recognition::DomainExpansionStateRecognizer domainSignRecognizer;
    ryoiki::hand_input::recognition::OpenPalmStateRecognizer openPalmRecognizer;
    ryoiki::hand_input::publication::LatestHandStateSlot handStateSlot;
    ryoiki::hand_input::recognition::SwipeEventRecognizer swipeRecognizer;
    ryoiki::hand_input::publication::OrderedHandEventRing handEventRing;
    std::shared_ptr<ryoiki::features::cad::CadInteractionEndpoint> cadInteraction;
    std::atomic<bool> capturePalmRotationReference{false};
    ryoiki::rendering::NativeRenderStage renderStage;
    std::shared_ptr<ryoiki::runtime::D3d11Device> d3dDevice;
    CameraCapture* activeCapture{nullptr};
    std::string lastError;
    std::chrono::steady_clock::time_point startedAt{};
    std::chrono::steady_clock::time_point lastDisplayAt{};
    std::uint64_t lastPresentedFrameId{0};
    bool asynchronousGpuRendering{false};
    ryoiki::rendering::Hand3dView hand3dView{};
    std::atomic<ryoiki::presentation::HandPresentationMode> handPresentationMode{
        ryoiki::presentation::HandPresentationMode::MirrorDirect};
    POINT lastHand3dDragPoint{};
    bool hand3dDragging{false};

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

bool isPointInHand3dViewport(const HWND hwnd, const int x, const int y)
{
    RECT client{};
    if (!GetClientRect(hwnd, &client))
    {
        return false;
    }
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    constexpr int kMinimumSplitWidth = 480;
    const int plotLeft = static_cast<int>(static_cast<double>(width) * 0.70);
    return width >= kMinimumSplitWidth && x >= plotLeft && x < width && y >= 0 && y < height;
}

bool beginHand3dViewDrag(
    RyoikiHandle& handle,
    const HWND hwnd,
    const int x,
    const int y)
{
    if (!isPointInHand3dViewport(hwnd, x, y))
    {
        return false;
    }
    handle.hand3dDragging = true;
    handle.lastHand3dDragPoint = {x, y};
    SetCapture(hwnd);
    return true;
}

bool updateHand3dViewDrag(RyoikiHandle& handle, const int x, const int y)
{
    if (!handle.hand3dDragging)
    {
        return false;
    }
    constexpr float kRadiansPerPixel = 0.008F;
    constexpr float kPitchLimit = 1.45F;
    const int deltaX = x - handle.lastHand3dDragPoint.x;
    const int deltaY = y - handle.lastHand3dDragPoint.y;
    handle.lastHand3dDragPoint = {x, y};
    handle.hand3dView.yawRadians += static_cast<float>(deltaX) * kRadiansPerPixel;
    handle.hand3dView.pitchRadians = std::clamp(
        handle.hand3dView.pitchRadians + static_cast<float>(deltaY) * kRadiansPerPixel,
        -kPitchLimit,
        kPitchLimit);
    handle.renderStage.updateHand3dView(handle.hand3dView);
    return true;
}

void endHand3dViewDrag(RyoikiHandle& handle, const HWND hwnd)
{
    if (!handle.hand3dDragging)
    {
        return;
    }
    handle.hand3dDragging = false;
    if (GetCapture() == hwnd)
    {
        ReleaseCapture();
    }
}

bool zoomHand3dView(
    RyoikiHandle& handle,
    const HWND hwnd,
    const int x,
    const int y,
    const int wheelDelta)
{
    if (!isPointInHand3dViewport(hwnd, x, y))
    {
        return false;
    }
    const float wheelSteps = static_cast<float>(wheelDelta) / WHEEL_DELTA;
    handle.hand3dView.halfExtent = std::clamp(
        handle.hand3dView.halfExtent * std::pow(0.88F, wheelSteps),
        0.04F,
        0.30F);
    handle.renderStage.updateHand3dView(handle.hand3dView);
    return true;
}

bool resetHand3dView(
    RyoikiHandle& handle,
    const HWND hwnd,
    const int x,
    const int y)
{
    if (!isPointInHand3dViewport(hwnd, x, y))
    {
        return false;
    }
    const auto presentationMode = handle.handPresentationMode.load();
    handle.hand3dView = {};
    handle.hand3dView.presentationMode = presentationMode;
    handle.renderStage.updateHand3dView(handle.hand3dView);
    return true;
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
    handle.metrics.camera_upload_ms = presentation.cameraUploadMs;
    if (presentation.usedGpuCameraSurface)
    {
        ++handle.metrics.gpu_rendered_frames;
    }
    handle.metrics.camera_draw_ms = presentation.cameraDrawMs;
    handle.metrics.overlay_draw_ms = presentation.overlayDrawMs;
    handle.metrics.hand_3d_draw_ms = presentation.hand3dDrawMs;
    handle.metrics.end_draw_ms = presentation.endDrawMs;
    handle.metrics.present_wait_ms = presentation.presentWaitMs;
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

std::uint64_t getCalibrationPalmIntervalFrames()
{
    if (GetEnvironmentVariableW(L"RYOIKI_CALIBRATION_DIR", nullptr, 0) <= 1)
    {
        return 0;
    }

    std::array<wchar_t, 32> value{};
    const DWORD length = GetEnvironmentVariableW(
        L"RYOIKI_CALIBRATION_PALM_INTERVAL",
        value.data(),
        static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size())
    {
        return 0;
    }

    wchar_t* end = nullptr;
    const unsigned long long parsed = std::wcstoull(value.data(), &end, 10);
    if (end == value.data() || *end != L'\0' || parsed == 0)
    {
        return 0;
    }
    return static_cast<std::uint64_t>(parsed);
}

struct HandRuntimeConfiguration
{
    ryoiki::hand_perception::OrtSessionConfiguration session;
    const wchar_t* palmModelFileName;
    const wchar_t* handModelFileName;
};

std::optional<HandRuntimeConfiguration> getHandRuntimeConfiguration(std::string& error)
{
    std::array<wchar_t, 64> value{};
    const DWORD length = GetEnvironmentVariableW(
        L"RYOIKI_EXECUTION_PROVIDER",
        value.data(),
        static_cast<DWORD>(value.size()));
    if (length == 0)
    {
        error.clear();
        return HandRuntimeConfiguration{
            ryoiki::hand_perception::OrtSessionConfiguration::cpu(),
            L"palm_detection.onnx",
            L"hand_landmark.onnx"};
    }
    if (length >= value.size())
    {
        error = "RYOIKI_EXECUTION_PROVIDER value is too long.";
        return std::nullopt;
    }

    std::wstring provider{value.data(), length};
    std::transform(provider.begin(), provider.end(), provider.begin(),
        [](const wchar_t character)
        {
            return static_cast<wchar_t>(std::towlower(character));
        });
    if (provider == L"cpu")
    {
        error.clear();
        return HandRuntimeConfiguration{
            ryoiki::hand_perception::OrtSessionConfiguration::cpu(),
            L"palm_detection.onnx",
            L"hand_landmark.onnx"};
    }
    if (provider == L"qnn-htp")
    {
        error.clear();
        return HandRuntimeConfiguration{
            ryoiki::hand_perception::OrtSessionConfiguration::qnnHtp(),
            L"palm_detection_qdq.onnx",
            L"hand_landmark_qdq.onnx"};
    }
    if (provider == L"directml")
    {
        error.clear();
        return HandRuntimeConfiguration{
            ryoiki::hand_perception::OrtSessionConfiguration::directMl(),
            L"palm_detection.onnx",
            L"hand_landmark.onnx"};
    }

    error = "RYOIKI_EXECUTION_PROVIDER must be 'cpu', 'directml', or 'qnn-htp'.";
    return std::nullopt;
}

void runCaptureLoop(RyoikiHandle& handle)
{
    using clock = std::chrono::steady_clock;
    CameraCapture camera;
    ActiveCaptureRegistration registration{handle, camera};
    std::string error;
    if (!camera.initialize(handle.d3dDevice, error))
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
        Microsoft::WRL::ComPtr<ID3D11Texture2D> gpuTexture;
        std::uint32_t gpuTextureSubresource = 0;
        Microsoft::WRL::ComPtr<IUnknown> gpuSampleOwner;
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
                gpuTexture,
                gpuTextureSubresource,
                gpuSampleOwner,
                false,
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

        bool prepared = false;
        if (frame != nullptr && gpuTexture != nullptr)
        {
            prepared = frame->prepareGpu(
                width,
                height,
                frameId,
                static_cast<std::uint64_t>(captureTimestamp),
                std::move(gpuTexture),
                gpuTextureSubresource,
                std::move(gpuSampleOwner),
                orientation);
        }
        else if (frame != nullptr)
        {
            prepared = frame->prepare(
                width,
                height,
                frameId,
                static_cast<std::uint64_t>(captureTimestamp),
                orientation);
        }
        if (prepared)
        {
            std::shared_ptr<const ryoiki::buffers::FrameBuffer> publishedFrame = frame;
            handle.perceptionMailbox.publish(publishedFrame);
            if (handle.asynchronousGpuRendering)
            {
                auto renderFrame = handle.renderFramePool.tryAcquire();
                if (renderFrame != nullptr && frame->gpuTexture() != nullptr)
                {
                    D3D11_TEXTURE2D_DESC sourceDescription{};
                    frame->gpuTexture()->GetDesc(&sourceDescription);
                    Microsoft::WRL::ComPtr<ID3D11Texture2D> renderTexture;
                    if (renderFrame->gpuTexture() != nullptr)
                    {
                        renderTexture = renderFrame->gpuTexture();
                        D3D11_TEXTURE2D_DESC renderDescription{};
                        renderTexture->GetDesc(&renderDescription);
                        if (renderDescription.Width != sourceDescription.Width
                            || renderDescription.Height != sourceDescription.Height
                            || renderDescription.Format != sourceDescription.Format)
                        {
                            renderTexture.Reset();
                        }
                    }
                    const auto copyStarted = clock::now();
                    HRESULT copyResult = S_OK;
                    if (renderTexture == nullptr)
                    {
                        sourceDescription.Usage = D3D11_USAGE_DEFAULT;
                        sourceDescription.CPUAccessFlags = 0;
                        sourceDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE
                            | D3D11_BIND_RENDER_TARGET;
                        sourceDescription.MiscFlags = 0;
                        copyResult = handle.d3dDevice->device()->CreateTexture2D(
                            &sourceDescription, nullptr, &renderTexture);
                    }
                    if (SUCCEEDED(copyResult))
                    {
                        std::lock_guard contextLock{
                            handle.d3dDevice->immediateContextMutex()};
                        handle.d3dDevice->immediateContext()->CopySubresourceRegion(
                            renderTexture.Get(), 0, 0, 0, 0,
                            frame->gpuTexture(), frame->gpuTextureSubresource(), nullptr);
                    }
                    frameCopyMs += std::chrono::duration<double, std::milli>(
                        clock::now() - copyStarted).count();
                    if (SUCCEEDED(copyResult)
                        && renderFrame->prepareGpu(
                            width, height, frameId,
                            static_cast<std::uint64_t>(captureTimestamp),
                            std::move(renderTexture), 0, {}, orientation))
                    {
                        std::shared_ptr<const ryoiki::buffers::FrameBuffer>
                            publishedRenderFrame = renderFrame;
                        handle.renderStage.publishFrame(
                            std::move(publishedRenderFrame));
                    }
                }
            }
        }

        const auto perceptionDrops = handle.perceptionMailbox.droppedFrames();
        {
            std::lock_guard lock{handle.stateMutex};
            handle.metrics.runtime_seconds = runtime;
            handle.metrics.camera_fps = cameraFps;
            handle.metrics.camera_wait_ms = cameraWaitMs;
            handle.metrics.frame_copy_ms = frameCopyMs;
            handle.metrics.native_overhead_ms = 0.0;
            handle.metrics.frame_pool_dropped_frames =
                handle.framePool.droppedAcquisitions()
                + handle.renderFramePool.droppedAcquisitions();
            handle.metrics.perception_dropped_frames = perceptionDrops;
            if (gpuTexture != nullptr || frame != nullptr
                && frame->gpuTexture() != nullptr)
            {
                ++handle.metrics.gpu_camera_frames;
                ID3D11Texture2D* capturedTexture = gpuTexture != nullptr
                    ? gpuTexture.Get() : frame->gpuTexture();
                D3D11_TEXTURE2D_DESC description{};
                capturedTexture->GetDesc(&description);
                handle.metrics.gpu_camera_dxgi_format =
                    static_cast<std::uint32_t>(description.Format);
                handle.metrics.gpu_camera_subresource = frame != nullptr
                    ? frame->gpuTextureSubresource() : gpuTextureSubresource;
            }

        }

    }

    handle.perceptionMailbox.stop();
    handle.running.store(false);
}

void runPerceptionLoop(RyoikiHandle& handle)
{
    using clock = std::chrono::steady_clock;
    std::string error;
    auto runtimeConfiguration = getHandRuntimeConfiguration(error);
    if (!runtimeConfiguration.has_value())
    {
        handle.setError(error);
        handle.perceptionMailbox.stop();
        return;
    }
#if defined(RYOIKI_ORT_DIRECTML)
    std::shared_ptr<ryoiki::runtime::DirectMlRuntime> directMlRuntime;
    wchar_t gpuTensorValue[2]{};
    const bool useDirectMlGpuTensor = GetEnvironmentVariableW(
        L"RYOIKI_DIRECTML_GPU_TENSOR", gpuTensorValue, 2) > 0;
    if (runtimeConfiguration->session.executionProvider()
        == ryoiki::hand_perception::ExecutionProvider::DirectMl)
    {
        directMlRuntime = ryoiki::runtime::DirectMlRuntime::create(handle.d3dDevice, error);
        if (directMlRuntime == nullptr)
        {
            handle.setError(error);
            handle.perceptionMailbox.stop();
            return;
        }
        if (useDirectMlGpuTensor)
        {
            runtimeConfiguration->session =
                ryoiki::hand_perception::OrtSessionConfiguration::directMl(directMlRuntime);
        }
    }
#endif

    auto palmRunner = ryoiki::hand_perception::OrtPalmDetectionRunner::create(
        getModelPath(runtimeConfiguration->palmModelFileName),
        runtimeConfiguration->session,
        error);
    if (palmRunner == nullptr)
    {
        handle.setError(error);
        handle.perceptionMailbox.stop();
        return;
    }
    auto handRunner = ryoiki::hand_perception::OrtHandLandmarkRunner::create(
        getModelPath(runtimeConfiguration->handModelFileName),
        ryoiki::hand_perception::HandLandmarkModelContract::openCvZoo2023(),
        runtimeConfiguration->session,
        error);
    if (handRunner == nullptr)
    {
        handle.setError(error);
        handle.perceptionMailbox.stop();
        return;
    }

    std::unique_ptr<ryoiki::geometry::IGeometryProcessor> palmGeometry;
    std::unique_ptr<ryoiki::geometry::IGeometryProcessor> handGeometry;
#if defined(RYOIKI_ORT_DIRECTML)
    if (useDirectMlGpuTensor)
    {
        palmGeometry = ryoiki::geometry::D3d12HandGeometryProcessor::create(
            directMlRuntime, error);
        handGeometry = ryoiki::geometry::D3d12HandGeometryProcessor::create(
            directMlRuntime, error);
    }
    else
#endif
    {
        palmGeometry = ryoiki::geometry::D3d11HandGeometryProcessor::create(
            handle.d3dDevice, error);
        handGeometry = ryoiki::geometry::D3d11HandGeometryProcessor::create(
            handle.d3dDevice, error);
    }
    if (palmGeometry == nullptr || handGeometry == nullptr)
    {
        handle.setError(error.empty() ? "GPU preprocessing initialization failed." : error);
        handle.perceptionMailbox.stop();
        return;
    }
    ryoiki::hand_perception::HandPerceptionGraph graph{
        std::move(palmRunner),
        std::move(handRunner),
        std::move(palmGeometry),
        std::move(handGeometry),
        {getCalibrationPalmIntervalFrames()}};
    ryoiki::hand_perception::HandPerceptionResult perceptionResult;
    ryoiki::hand_perception::HandPerceptionGraphMetrics graphMetrics{};
    auto lastPerceptionAt = clock::time_point{};
#if defined(RYOIKI_ORT_DIRECTML)
    bool cameraShareProbeCompleted = false;
    wchar_t cameraShareProbeValue[2]{};
    const bool cameraShareProbeEnabled = GetEnvironmentVariableW(
        L"RYOIKI_PROBE_D3D12_CAMERA_SHARE", cameraShareProbeValue, 2) > 0;
#endif
    const std::string providerLog = "Hand perception runners selected: "
        + graph.providerSummary() + "\n";
    OutputDebugStringA(providerLog.c_str());
    wchar_t palmRotationFilterValue[16]{};
    const DWORD palmRotationFilterLength = GetEnvironmentVariableW(
        L"RYOIKI_PALM_ROTATION_FILTER",
        palmRotationFilterValue,
        static_cast<DWORD>(std::size(palmRotationFilterValue)));
    const bool usePalmRotationEskf =
        palmRotationFilterLength == 0
        || palmRotationFilterLength >= std::size(palmRotationFilterValue)
        || _wcsicmp(palmRotationFilterValue, L"raw") != 0;
    OutputDebugStringA(usePalmRotationEskf
        ? "Palm rotation temporal filter: eskf\n"
        : "Palm rotation temporal filter: raw\n");
    std::ofstream rotationComparisonCsv;
    const DWORD comparisonPathLength = GetEnvironmentVariableW(
        L"RYOIKI_ROTATION_COMPARISON_CSV", nullptr, 0);
    if (comparisonPathLength > 1)
    {
        std::vector<wchar_t> comparisonPath(comparisonPathLength);
        if (GetEnvironmentVariableW(
                L"RYOIKI_ROTATION_COMPARISON_CSV",
                comparisonPath.data(),
                comparisonPathLength) > 0)
        {
            rotationComparisonCsv.open(
                std::filesystem::path{comparisonPath.data()},
                std::ios::out | std::ios::trunc);
            if (rotationComparisonCsv)
            {
                rotationComparisonCsv
                    << "frame_id,timestamp_us,tracking_quality,six_valid,basis_valid,"
                       "six_fit_error,six_angle_deg,basis_angle_deg,difference_angle_deg,"
                       "difference_x_deg,difference_y_deg,difference_z_deg\n";
                OutputDebugStringA("Palm rotation comparison CSV enabled.\n");
            }
            else
            {
                OutputDebugStringA("Palm rotation comparison CSV could not be opened.\n");
            }
        }
    }
    ryoiki::hand_input::measurements::WeightedPalmRotationTracker
        comparisonSixPointTracker;
    ryoiki::hand_input::measurements::PalmBasisRotationTracker
        comparisonPalmBasisTracker;
    bool hasComparisonReference = false;
    std::size_t comparisonRows = 0;

    while (handle.running.load())
    {
        const auto frame = handle.perceptionMailbox.waitForLatest();
        if (frame == nullptr)
        {
            break;
        }
#if defined(RYOIKI_ORT_DIRECTML)
        if (cameraShareProbeEnabled && !cameraShareProbeCompleted
            && directMlRuntime != nullptr)
        {
            cameraShareProbeCompleted = true;
            std::string diagnostic;
            const auto sharedCamera = directMlRuntime->tryOpenD3d11Texture(
                frame->gpuTexture(), diagnostic);
            if (sharedCamera == nullptr)
            {
                std::vector<double> copySamples;
                copySamples.reserve(20);
                std::string copyDiagnostic;
                for (int sample = 0; sample < 20; ++sample)
                {
                    double copyMs = 0.0;
                    const auto copiedCamera = directMlRuntime->copyCameraToSharedTexture(
                        frame->gpuTexture(), static_cast<std::uint64_t>(sample + 1),
                        copyMs, copyDiagnostic);
                    if (copiedCamera == nullptr) break;
                    copySamples.push_back(copyMs);
                }
                if (!copySamples.empty())
                {
                    std::sort(copySamples.begin(), copySamples.end());
                    const auto percentile = [&copySamples](const double ratio)
                    {
                        const auto index = static_cast<std::size_t>(
                            ratio * static_cast<double>(copySamples.size() - 1));
                        return copySamples[index];
                    };
                    std::ostringstream summary;
                    summary << " Full-frame GPU copy n=" << copySamples.size()
                        << ", p50=" << std::fixed << std::setprecision(3)
                        << percentile(0.50) << " ms, p95=" << percentile(0.95) << " ms.";
                    diagnostic += summary.str();
                }
                else
                {
                    diagnostic += " " + copyDiagnostic;
                }
            }
            OutputDebugStringA((diagnostic + "\n").c_str());
            handle.setError(diagnostic);
            handle.perceptionMailbox.stop();
            handle.running.store(false);
            handle.renderStage.requestRedraw();
            break;
        }
#endif

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
        const auto domainSign = ryoiki::hand_input::recognition::recognizeDomainExpansionState(
            perceptionResult.hand);
        const auto domainState = handle.domainSignRecognizer.process(
            domainSign,
            perceptionResult.hand.detected ? perceptionResult.hand.confidence : 0.0F,
            frame->frameId(),
            frame->captureTimestampUs());
        const auto measurementFrame = handle.handMeasurementExtractor.extract(
            perceptionResult.hand,
            frame->frameId(),
            frame->captureTimestampUs(),
            frame->uprightWidth(),
            frame->uprightHeight());
        const auto& handMeasurements = measurementFrame.hand;
        const auto& screenPalm = measurementFrame.screenPalm;
        if (handle.capturePalmRotationReference.exchange(false))
        {
            static_cast<void>(
                handle.palmRotationTracker.captureReference(perceptionResult.hand));
            static_cast<void>(
                handle.incrementalPalmRotationTracker.captureReference(
                    perceptionResult.hand));
            handle.palmRotationEskf.reset();
        }
        const auto absolutePalmRotation =
            handle.palmRotationTracker.estimate(perceptionResult.hand);
        const auto incrementalPalmRotation =
            handle.incrementalPalmRotationTracker.estimate(perceptionResult.hand);
        static_cast<void>(
            handle.incrementalPalmRotationTracker.captureReference(
                perceptionResult.hand));
        const auto palmRotation = usePalmRotationEskf
            ? handle.palmRotationEskf.update(
                absolutePalmRotation,
                incrementalPalmRotation,
                frame->captureTimestampUs())
            : absolutePalmRotation;
        if (rotationComparisonCsv)
        {
            if (!hasComparisonReference
                && handMeasurements.present
                && handMeasurements.quality
                    == ryoiki::hand_input::measurements::HandMeasurementQuality::Valid)
            {
                hasComparisonReference =
                    comparisonSixPointTracker.captureReference(perceptionResult.hand)
                    && comparisonPalmBasisTracker.captureReference(handMeasurements);
            }
            if (hasComparisonReference)
            {
                const auto sixPoint =
                    comparisonSixPointTracker.estimate(perceptionResult.hand);
                const auto palmBasis =
                    comparisonPalmBasisTracker.estimate(handMeasurements);
                std::array<float, 3> differenceVector{};
                float differenceAngle = 0.0F;
                if (sixPoint.valid && palmBasis.valid)
                {
                    const auto difference = relativeRotationDifference(
                        sixPoint.rotation, palmBasis.rotation);
                    differenceVector = rotationVectorDegrees(difference);
                    differenceAngle = rotationAngleDegrees(difference);
                }
                rotationComparisonCsv
                    << frame->frameId() << ','
                    << frame->captureTimestampUs() << ','
                    << perceptionResult.hand.confidence << ','
                    << (sixPoint.valid ? 1 : 0) << ','
                    << (palmBasis.valid ? 1 : 0) << ','
                    << sixPoint.fitError << ','
                    << (sixPoint.valid ? rotationAngleDegrees(sixPoint.rotation) : 0.0F)
                    << ','
                    << (palmBasis.valid ? rotationAngleDegrees(palmBasis.rotation) : 0.0F)
                    << ','
                    << differenceAngle << ','
                    << differenceVector[0] << ','
                    << differenceVector[1] << ','
                    << differenceVector[2] << '\n';
                ++comparisonRows;
                if (comparisonRows % 120 == 0) rotationComparisonCsv.flush();
            }
        }
        const auto openPalm =
            ryoiki::hand_input::recognition::recognizeOpenPalmState(
                handMeasurements);
        const auto openPalmState = handle.openPalmRecognizer.process(
            openPalm,
            handMeasurements.trackingQuality,
            frame->frameId(),
            frame->captureTimestampUs());
        const auto swipeEvent = handle.swipeRecognizer.process(
            measurementFrame,
            openPalm.detected,
            openPalm.confidence);
        if (swipeEvent.has_value())
        {
            handle.handEventRing.publish(*swipeEvent);
        }
        const auto latestSwipeEvent = swipeEvent.value_or(
            ryoiki::hand_input::recognition::HandEvent{});
        ryoiki::hand_input::publication::HandStateSnapshot stateSnapshot{};
        stateSnapshot.frameId = frame->frameId();
        stateSnapshot.timestampUs = frame->captureTimestampUs();
        stateSnapshot.count = 2;
        stateSnapshot.states[0] = domainState;
        stateSnapshot.states[1] = openPalmState;
        handle.handStateSlot.publish(stateSnapshot);
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
            handle.hand.capture_timestamp_us = frame->captureTimestampUs();
            handle.hand.hand_count = perceptionResult.hand.detected ? 1 : 0;
            handle.hand.confidence = perceptionResult.hand.confidence;
            handle.hand.handedness = perceptionResult.hand.detected
                ? perceptionResult.hand.handedness : -1.0F;
            std::memset(handle.hand.bbox, 0, sizeof(handle.hand.bbox));
            std::memset(handle.hand.landmarks, 0, sizeof(handle.hand.landmarks));
            std::memset(handle.hand.world_landmarks, 0, sizeof(handle.hand.world_landmarks));
            std::memset(handle.hand.palm_normal, 0, sizeof(handle.hand.palm_normal));
            std::memset(
                handle.hand.palm_rotation_basis,
                0,
                sizeof(handle.hand.palm_rotation_basis));
            std::memset(handle.hand.palm_center, 0, sizeof(handle.hand.palm_center));
            handle.hand.palm_scale = 0.0F;
            handle.hand.palm_pose_valid = 0;
            std::memset(
                handle.hand.screen_palm_center,
                0,
                sizeof(handle.hand.screen_palm_center));
            handle.hand.screen_palm_scale = 0.0F;
            handle.hand.screen_palm_valid = screenPalm.valid ? 1 : 0;
            if (screenPalm.valid)
            {
                handle.hand.screen_palm_center[0] = screenPalm.centerX;
                handle.hand.screen_palm_center[1] = screenPalm.centerY;
                handle.hand.screen_palm_scale = screenPalm.scale;
            }
            std::memset(
                handle.hand.palm_relative_rotation,
                0,
                sizeof(handle.hand.palm_relative_rotation));
            handle.hand.palm_rotation_fit_error = palmRotation.fitError;
            handle.hand.palm_relative_rotation_valid = palmRotation.valid ? 1 : 0;
            if (palmRotation.valid)
            {
                std::copy(
                    palmRotation.rotation.begin(),
                    palmRotation.rotation.end(),
                    handle.hand.palm_relative_rotation);
            }
            handle.hand.domain_sign_confidence = domainSign.confidence;
            handle.hand.domain_sign_detected = domainSign.detected ? 1 : 0;
            handle.hand.domain_sign_features[0] = domainSign.features.indexExtended;
            handle.hand.domain_sign_features[1] = domainSign.features.middleWrap;
            handle.hand.domain_sign_features[2] = domainSign.features.middleCurled;
            handle.hand.domain_sign_features[3] = domainSign.features.ringCurled;
            handle.hand.domain_sign_features[4] = domainSign.features.pinkyCurled;
            handle.hand.domain_sign_features[5] = domainSign.features.thumbTucked;
            if (perceptionResult.hand.detected)
            {
                if (handMeasurements.quality
                    == ryoiki::hand_input::measurements::HandMeasurementQuality::Valid)
                {
                    const std::array axes{
                        handMeasurements.palmXAxis,
                        handMeasurements.palmYAxis,
                        handMeasurements.palmZAxis};
                    for (std::size_t column = 0; column < axes.size(); ++column)
                    {
                        handle.hand.palm_rotation_basis[column * 3] = axes[column].x;
                        handle.hand.palm_rotation_basis[column * 3 + 1] = axes[column].y;
                        handle.hand.palm_rotation_basis[column * 3 + 2] = axes[column].z;
                    }
                    handle.hand.palm_center[0] = handMeasurements.palmPosition.x;
                    handle.hand.palm_center[1] = handMeasurements.palmPosition.y;
                    handle.hand.palm_center[2] = handMeasurements.palmPosition.z;
                    handle.hand.palm_scale = handMeasurements.palmScale;
                    handle.hand.palm_pose_valid = 1;
                }
                const auto palmNormal = ryoiki::rendering::calculatePalmNormal(
                    perceptionResult.hand);
                handle.hand.palm_normal[0] = palmNormal.x;
                handle.hand.palm_normal[1] = palmNormal.y;
                handle.hand.palm_normal[2] = palmNormal.z;
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
                    const auto& worldLandmark = perceptionResult.hand.worldLandmarks[index];
                    handle.hand.world_landmarks[index * 3] = worldLandmark.x;
                    handle.hand.world_landmarks[index * 3 + 1] = worldLandmark.y;
                    handle.hand.world_landmarks[index * 3 + 2] = worldLandmark.z;
                }
            }
        }

        std::shared_ptr<ryoiki::features::cad::CadInteractionEndpoint>
            cadInteraction;
        {
            std::lock_guard lock{handle.cadInteractionMutex};
            cadInteraction = handle.cadInteraction;
        }
        if (cadInteraction != nullptr)
        {
            ryoiki::features::cad::CadHandInput input{};
            input.frameId = frame->frameId();
            input.captureTimestampUs = frame->captureTimestampUs();
            input.trackingQuality = perceptionResult.hand.confidence;
            input.screenCenterX = screenPalm.centerX;
            input.screenCenterY = screenPalm.centerY;
            input.screenScale = screenPalm.scale;
            input.relativeRotation = palmRotation.rotation;
            input.rotationFitError = palmRotation.fitError;
            input.handPresent = perceptionResult.hand.detected;
            input.screenPalmValid = screenPalm.valid;
            input.relativeRotationValid = palmRotation.valid;
            cadInteraction->process(input);
        }

        if (handle.asynchronousGpuRendering)
        {
            handle.renderStage.publishPerception(
                perceptionResult,
                frame->frameId(),
                domainState,
                openPalmState,
                latestSwipeEvent);
        }
        else
        {
            handle.renderStage.publish({
                frame,
                perceptionResult,
                frame->frameId(),
                domainState,
                openPalmState,
                latestSwipeEvent});
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
        handle->renderFramePool.resetStatistics();
        handle->perceptionMailbox.reset();
        handle->domainSignRecognizer.reset();
        handle->openPalmRecognizer.reset();
        handle->handStateSlot.reset();
        handle->swipeRecognizer.reset();
        handle->handEventRing.reset();

        RECT childRect{};
        GetClientRect(handle->childHwnd, &childRect);
        std::string renderError;
        wchar_t on12ProbeValue[2]{};
        wchar_t gpuTensorValue[2]{};
        const bool useD3d11On12 = GetEnvironmentVariableW(
            L"RYOIKI_PROBE_D3D11ON12_CAPTURE", on12ProbeValue, 2) > 0
            || GetEnvironmentVariableW(
                L"RYOIKI_DIRECTML_GPU_TENSOR", gpuTensorValue, 2) > 0;
        handle->asynchronousGpuRendering =
            GetEnvironmentVariableW(
                L"RYOIKI_DIRECTML_GPU_TENSOR", gpuTensorValue, 2) > 0;
        handle->d3dDevice = useD3d11On12
            ? ryoiki::runtime::D3d11Device::createOn12(renderError)
            : ryoiki::runtime::D3d11Device::create(renderError);
        if (handle->d3dDevice == nullptr)
        {
            handle->running.store(false);
            handle->setError(renderError);
            return kRyoikiStatusFailure;
        }
        if (!handle->renderStage.start(
                handle->d3dDevice,
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
        std::shared_ptr<ryoiki::features::cad::CadInteractionEndpoint> endpoint;
        {
            std::lock_guard lock{handle->cadInteractionMutex};
            endpoint = handle->cadInteraction;
            handle->cadInteractionMode =
                ryoiki::features::cad::HandInteractionMode::None;
        }
        if (endpoint != nullptr)
        {
            static_cast<void>(endpoint->configure(
                ryoiki::features::cad::HandInteractionMode::None,
                ryoiki::presentation::HandPresentationMode::MirrorDirect,
                1.0F));
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

RYOIKI_EXPORT std::int32_t ryoiki_get_latest_states(
    RyoikiHandle* handle,
    RyoikiHandStateSnapshot* out_snapshot)
{
    if (handle == nullptr || out_snapshot == nullptr)
    {
        return kRyoikiStatusFailure;
    }

    try
    {
        const auto latest = handle->handStateSlot.latest();
        RyoikiHandStateSnapshot result{};
        result.abi_version = kRyoikiAbiVersion;
        result.struct_size = static_cast<std::uint32_t>(sizeof(RyoikiHandStateSnapshot));
        result.frame_id = latest.frameId;
        result.timestamp_us = latest.timestampUs;
        result.count = static_cast<std::uint32_t>(
            (std::min)(latest.count, ryoiki::hand_input::publication::kMaxHandStates));
        for (std::size_t index = 0; index < result.count; ++index)
        {
            const auto& source = latest.states[index];
            auto& destination = result.states[index];
            destination.id = source.id;
            destination.phase = static_cast<std::uint32_t>(source.phase);
            destination.transition = static_cast<std::uint32_t>(source.transition);
            destination.flags = static_cast<std::uint32_t>(source.flags);
            destination.confidence = source.confidence;
            destination.input_quality = source.inputQuality;
            destination.began_frame_id = source.beganFrameId;
            destination.current_frame_id = source.currentFrameId;
            destination.timestamp_us = source.timestampUs;
        }
        *out_snapshot = result;
        return kRyoikiStatusSuccess;
    }
    catch (...)
    {
        return kRyoikiStatusFailure;
    }
}

RYOIKI_EXPORT std::int32_t ryoiki_read_hand_events(
    RyoikiHandle* handle,
    const std::uint64_t afterSequence,
    RyoikiHandEventBatch* outBatch)
{
    if (handle == nullptr || outBatch == nullptr)
    {
        return kRyoikiStatusFailure;
    }
    try
    {
        const auto source = handle->handEventRing.readAfter(afterSequence);
        RyoikiHandEventBatch result{};
        result.abi_version = kRyoikiAbiVersion;
        result.struct_size =
            static_cast<std::uint32_t>(sizeof(RyoikiHandEventBatch));
        result.next_sequence = source.nextSequence;
        result.dropped_count = source.droppedCount;
        result.count = static_cast<std::uint32_t>(source.count);
        for (std::size_t index = 0; index < source.count; ++index)
        {
            const auto& event = source.events[index];
            auto& destination = result.events[index];
            destination.sequence = event.sequence;
            destination.id = event.id;
            destination.confidence = event.confidence;
            destination.input_quality = event.inputQuality;
            destination.displacement_x = event.displacementX;
            destination.displacement_y = event.displacementY;
            destination.began_frame_id = event.beganFrameId;
            destination.ended_frame_id = event.endedFrameId;
            destination.began_timestamp_us = event.beganTimestampUs;
            destination.ended_timestamp_us = event.endedTimestampUs;
            destination.duration_us = event.durationUs;
        }
        *outBatch = result;
        return kRyoikiStatusSuccess;
    }
    catch (...)
    {
        return kRyoikiStatusFailure;
    }
}

RYOIKI_EXPORT std::int32_t ryoiki_capture_palm_rotation_reference(
    RyoikiHandle* handle)
{
    if (handle == nullptr || !handle->running.load())
    {
        return kRyoikiStatusFailure;
    }
    {
        std::lock_guard lock{handle->stateMutex};
        handle->hand.palm_relative_rotation_valid = 0;
        handle->hand.palm_rotation_fit_error = 0.0F;
        std::memset(
            handle->hand.palm_relative_rotation,
            0,
            sizeof(handle->hand.palm_relative_rotation));
    }
    handle->capturePalmRotationReference.store(true);
    return kRyoikiStatusSuccess;
}

RYOIKI_EXPORT std::int32_t ryoiki_set_hand_presentation_mode(
    RyoikiHandle* handle,
    const std::int32_t presentationMode)
{
    if (handle == nullptr
        || presentationMode < RYOIKI_HAND_PRESENTATION_MIRROR_DIRECT
        || presentationMode > RYOIKI_HAND_PRESENTATION_PHYSICAL)
    {
        return kRyoikiStatusFailure;
    }
    const auto mode =
        static_cast<ryoiki::presentation::HandPresentationMode>(presentationMode);
    handle->handPresentationMode.store(mode);
    handle->hand3dView.presentationMode = mode;
    handle->renderStage.updateHand3dView(handle->hand3dView);
    return kRyoikiStatusSuccess;
}

RYOIKI_EXPORT std::int32_t ryoiki_configure_cad_hand_interaction(
    RyoikiHandle* visionHandle,
    RyoikiCadHandle* cadHandle,
    const std::int32_t interactionMode,
    const std::int32_t presentationMode,
    const float rotationSensitivity)
{
    if (visionHandle == nullptr || cadHandle == nullptr
        || !std::isfinite(rotationSensitivity)
        || interactionMode < 0 || interactionMode > 3
        || presentationMode < 0 || presentationMode > 1)
    {
        return kRyoikiStatusFailure;
    }
    if (ryoiki_set_hand_presentation_mode(
            visionHandle, presentationMode) == kRyoikiStatusFailure)
    {
        return kRyoikiStatusFailure;
    }
    const auto endpoint = ryoikiCadInteractionEndpoint(cadHandle);
    if (endpoint == nullptr) return kRyoikiStatusFailure;
    const auto mode =
        static_cast<ryoiki::features::cad::HandInteractionMode>(interactionMode);
    if (!endpoint->configure(
            mode,
            static_cast<ryoiki::presentation::HandPresentationMode>(
                presentationMode),
            rotationSensitivity))
    {
        return kRyoikiStatusFailure;
    }
    {
        std::lock_guard lock{visionHandle->cadInteractionMutex};
        if (mode == ryoiki::features::cad::HandInteractionMode::Rotate
            && visionHandle->cadInteractionMode != mode)
        {
            visionHandle->capturePalmRotationReference.store(true);
        }
        visionHandle->cadInteraction = endpoint;
        visionHandle->cadInteractionMode = mode;
    }
    return kRyoikiStatusSuccess;
}

RYOIKI_EXPORT std::int32_t ryoiki_get_cad_hand_interaction(
    RyoikiHandle* visionHandle,
    RyoikiCadHandInteractionResult* outResult)
{
    if (visionHandle == nullptr || outResult == nullptr)
        return kRyoikiStatusFailure;
    std::shared_ptr<ryoiki::features::cad::CadInteractionEndpoint> endpoint;
    {
        std::lock_guard lock{visionHandle->cadInteractionMutex};
        endpoint = visionHandle->cadInteraction;
    }
    return endpoint != nullptr && endpoint->copyLatest(*outResult)
        ? kRyoikiStatusSuccess
        : kRyoikiStatusFailure;
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

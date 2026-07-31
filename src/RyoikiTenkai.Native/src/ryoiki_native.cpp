#include "ryoiki_native.h"
#include "Buffers/frame_pool.h"
#include "Geometry/d3d11_hand_geometry_processor.h"
#include "Features/Cad/cad_interaction_endpoint.h"
#include "HandPerception/MediaPipeGraph/hand_perception_graph.h"
#include "HandPerception/ModelRunners/ort_hand_landmark_runner.h"
#include "HandPerception/ModelRunners/ort_palm_detection_runner.h"
#include "HandInput/Measurements/hand_measurement_extractor.h"
#include "HandInput/Measurements/multi_hand_measurement_stage.h"
#include "HandInput/Measurements/palm_basis_rotation_tracker.h"
#include "HandInput/Measurements/palm_rotation_eskf.h"
#include "HandInput/Measurements/palm_rotation_one_euro_filter.h"
#include "HandInput/Measurements/rotation_observation_gate.h"
#include "HandInput/Measurements/weighted_palm_rotation_tracker.h"
#include "HandInput/Publication/latest_hand_state_slot.h"
#include "HandInput/Publication/ordered_hand_event_ring.h"
#include "HandInput/Recognition/domain_expansion_state_recognizer.h"
#include "HandInput/Recognition/gesture_recording_session.h"
#include "HandInput/Recognition/gesture_match_stabilizer.h"
#include "HandInput/Recognition/multi_hand_gesture_core.h"
#include "HandInput/Recognition/gesture_template_registry.h"
#include "HandInput/Recognition/gesture_template_repository.h"
#include "HandInput/Recognition/open_palm_state_recognizer.h"
#include "HandInput/Recognition/swipe_event_recognizer.h"
#include "Pipeline/perception_mailbox.h"
#include "Rendering/hand_3d_plot.h"
#include "Rendering/gesture_dtw_debug_source.h"
#include "Rendering/gesture_dtw_debug_render_packet_adapter.h"
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
#include <string_view>
#include <thread>
#include <vector>

static_assert(sizeof(RyoikiMetrics) == 240);
static_assert(sizeof(RyoikiPalmResult) == 96);
static_assert(sizeof(RyoikiHandResult) == 720);
static_assert(sizeof(RyoikiHandsResult) == 1120);
static_assert(sizeof(RyoikiHandTopologySnapshot) == 344);
static_assert(sizeof(RyoikiCadHandInteractionResult) == 56);
static_assert(sizeof(RyoikiHandEventBatch) == 1312);
static_assert(sizeof(RyoikiGestureBindingMetadata) == 296);
static_assert(sizeof(RyoikiGestureBindingList) == 2508);

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
        hands.abi_version = kRyoikiAbiVersion;
        hands.struct_size = static_cast<std::uint32_t>(sizeof(RyoikiHandsResult));
        topology.abi_version = kRyoikiAbiVersion;
        topology.struct_size =
            static_cast<std::uint32_t>(sizeof(RyoikiHandTopologySnapshot));
        gestureRecognitionSnapshot.abi_version = kRyoikiAbiVersion;
        gestureRecognitionSnapshot.struct_size =
            static_cast<std::uint32_t>(sizeof(RyoikiGestureRecognitionSnapshot));
        gestureDtwDebugSnapshot.abi_version = kRyoikiAbiVersion;
        gestureDtwDebugSnapshot.struct_size =
            static_cast<std::uint32_t>(sizeof(RyoikiGestureDtwDebugSnapshot));
        gestureRecordingStatus.abi_version = kRyoikiAbiVersion;
        gestureRecordingStatus.struct_size =
            static_cast<std::uint32_t>(sizeof(RyoikiGestureRecordingStatus));
    }

    HWND parentHwnd{};
    HWND childHwnd{};
    std::atomic<bool> running{false};
    std::thread captureWorker;
    std::thread perceptionWorker;
    mutable std::mutex stateMutex;
    mutable std::mutex captureMutex;
    mutable std::mutex cadInteractionMutex;
    mutable std::mutex gestureRecognitionMutex;
    mutable std::mutex gestureRecordingMutex;
    ryoiki::features::cad::HandInteractionMode cadInteractionMode{
        ryoiki::features::cad::HandInteractionMode::None};
    RyoikiMetrics metrics{};
    RyoikiPalmResult palm{};
    RyoikiHandResult hand{};
    RyoikiHandsResult hands{};
    RyoikiHandTopologySnapshot topology{};
    ryoiki::buffers::FramePool framePool{4};
    ryoiki::buffers::FramePool renderFramePool{4};
    ryoiki::pipeline::PerceptionMailbox perceptionMailbox;
    ryoiki::hand_input::measurements::MultiHandMeasurementStage handMeasurementStage;
    ryoiki::hand_input::measurements::PalmBasisRotationTracker
        palmBasisRotationTracker;
    ryoiki::hand_input::measurements::PalmBasisRotationTracker
        incrementalPalmBasisRotationTracker;
    ryoiki::hand_input::measurements::PalmRotationEskf palmRotationEskf;
    ryoiki::hand_input::measurements::PalmRotationOneEuroFilter
        palmRotationOneEuroFilter;
    ryoiki::hand_input::measurements::RotationObservationGate
        palmRotationObservationGate;
    ryoiki::hand_perception::HandTrackId palmRotationTrackId{0};
    ryoiki::hand_perception::HandTrackId palmRotationRebindCandidateId{0};
    std::uint32_t palmRotationMissingFrames{0};
    std::uint32_t palmRotationRebindCandidateFrames{0};
    // Negative means no rotation session has supplied one yet, matching the
    // ABI's handedness convention. Rebind scoring only reads this after a
    // capture has written it.
    float palmRotationLastHandedness{-1.0F};
    float palmRotationLastScreenX{0.0F};
    float palmRotationLastScreenY{0.0F};
    bool palmRotationLastScreenValid{false};
    ryoiki::hand_input::recognition::DomainExpansionStateRecognizer domainSignRecognizer;
    ryoiki::hand_input::recognition::OpenPalmStateRecognizer openPalmRecognizer;
    ryoiki::hand_input::publication::LatestHandStateSlot handStateSlot;
    ryoiki::hand_input::recognition::SwipeEventRecognizer swipeRecognizer;
    ryoiki::hand_input::publication::OrderedHandEventRing handEventRing;
    // On-demand gesture recognition (Stage 2): perception-worker-owned, like
    // handMeasurementStage above. gestureRecognitionSnapshot is the only
    // field another thread may touch, and only under gestureRecognitionMutex.
    ryoiki::hand_input::recognition::GestureTemplateRegistry gestureTemplateRegistry;
    std::unique_ptr<ryoiki::hand_input::recognition::GestureTemplateRepository> gestureTemplateRepository;
    std::vector<ryoiki::hand_input::recognition::GestureDefinitionMetadata> gestureDefinitionMetadata;
    std::mutex gestureRepositoryMutex;
    std::string pendingGestureDefinitionName;
    std::uint32_t pendingGestureDefinitionId{0};
    bool gesturePersistenceAvailable{false};
    std::uint32_t pendingGestureRepositoryCommand{0}; // 1 metadata, 2 delete, 3 reload
    std::uint32_t pendingGestureRepositoryId{0};
    bool pendingGestureRepositoryEnabled{true};
    ryoiki::hand_input::recognition::GestureMatchStabilizer gestureMatchStabilizer;
    ryoiki::hand_input::recognition::GestureMatchStabilizer twoHandGestureMatchStabilizer;
    ryoiki::hand_input::recognition::TwoHandFrameSetBuffer twoHandGestureHistory;
    RyoikiGestureRecognitionSnapshot gestureRecognitionSnapshot{};
    RyoikiGestureDtwDebugSnapshot gestureDtwDebugSnapshot{};
    std::shared_ptr<ryoiki::rendering::GestureDtwDebugSource> gestureDtwDebugSource{
        std::make_shared<ryoiki::rendering::GestureDtwDebugSource>()};
    std::uint64_t lastGestureRecognitionTimestampUs{0};
    std::atomic<bool> gestureTemplateRegistrationRequested{false};
    std::atomic<std::uint32_t> gestureTemplateRegistrationTrackId{0};
    std::atomic<std::uint32_t> gestureTemplateRegistrationTemplateId{0};
    // Explicit begin/finish/cancel recording session (ABI version 23+).
    // gestureRecordingSession is perception-worker-owned except for its own
    // internal mutex-protected command mailbox (see
    // GestureRecordingSession::submitCommand). gestureRecordingStatus is the
    // only cross-thread-readable copy and is guarded by
    // gestureRecordingMutex, mirroring gestureRecognitionSnapshot above.
    ryoiki::hand_input::recognition::GestureRecordingSession gestureRecordingSession;
    ryoiki::hand_input::recognition::GestureRecordingState lastGestureRecordingState{
        ryoiki::hand_input::recognition::GestureRecordingState::Idle};
    RyoikiGestureRecordingStatus gestureRecordingStatus{};
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

std::shared_ptr<ryoiki::rendering::GestureDtwDebugSource>
ryoikiGestureDtwDebugSource(RyoikiHandle* handle) noexcept
{
    return handle == nullptr ? nullptr : handle->gestureDtwDebugSource;
}

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
    enum class PalmRotationFilter
    {
        Raw,
        Eskf,
        OneEuro
    };
    PalmRotationFilter palmRotationFilter = PalmRotationFilter::OneEuro;
    if (palmRotationFilterLength > 0
        && palmRotationFilterLength < std::size(palmRotationFilterValue))
    {
        if (_wcsicmp(palmRotationFilterValue, L"raw") == 0)
        {
            palmRotationFilter = PalmRotationFilter::Raw;
        }
        else if (_wcsicmp(palmRotationFilterValue, L"eskf") == 0)
        {
            palmRotationFilter = PalmRotationFilter::Eskf;
        }
    }
    OutputDebugStringA(
        palmRotationFilter == PalmRotationFilter::Raw
            ? "Palm rotation temporal filter: raw\n"
            : palmRotationFilter == PalmRotationFilter::Eskf
                ? "Palm rotation temporal filter: eskf\n"
                : "Palm rotation temporal filter: one-euro\n");
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
                    << "frame_id,timestamp_us,active_track_id,"
                       "rotation_missing_frames,active_handedness,"
                       "last_handedness,rebind_candidate_handedness,"
                       "tracking_quality,six_valid,basis_valid,"
                       "six_fit_error,six_angle_deg,basis_angle_deg,difference_angle_deg,"
                       "difference_x_deg,difference_y_deg,difference_z_deg,"
                       "gate_state,gate_rejection,gate_raw_delta_deg,"
                       "gate_candidate_frames,gate_outlier_frames,"
                       "gate_invalid_frames,gate_reacquired,"
                       "gate_rebase_offset_deg,gate_rebase_step_deg,"
                       "filtered_valid\n";
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
        const auto multiHandMeasurements = handle.handMeasurementStage.extract(
            perceptionResult,
            frame->frameId(),
            frame->captureTimestampUs(),
            frame->uprightWidth(),
            frame->uprightHeight());
        {
            ryoiki::hand_input::recognition::TwoHandFrameSetObservation frameSet{};
            frameSet.timestampUs = frame->captureTimestampUs();
            std::array<std::size_t, ryoiki::hand_perception::kMaxPerceivedHands> order{};
            for (std::size_t index = 0; index < perceptionResult.handCount; ++index) order[index] = index;
            std::sort(order.begin(), order.begin() + perceptionResult.handCount,
                [&perceptionResult](const auto left, const auto right)
                { return perceptionResult.hands[left].confidence > perceptionResult.hands[right].confidence; });
            frameSet.handCount = (std::min)(perceptionResult.handCount, std::size_t{2});
            for (std::size_t index = 0; index < frameSet.handCount; ++index)
            {
                const auto sourceIndex = order[index];
                frameSet.hands[index] =
                    ryoiki::hand_input::measurements::makeUnifiedFeatureObservation(
                        perceptionResult.hands[sourceIndex], frame->captureTimestampUs());
                frameSet.confidences[index] = perceptionResult.hands[sourceIndex].confidence;
            }
            handle.twoHandGestureHistory.push(std::move(frameSet));
        }
        const auto primaryMeasurementFrame = multiHandMeasurements.handCount > 0
            ? ryoiki::hand_input::measurements::HandMeasurementFrame{
                multiHandMeasurements.hands[0].hand,
                multiHandMeasurements.hands[0].screenPalm}
            : ryoiki::hand_input::measurements::HandMeasurementFrame{};
        const auto& primaryHandMeasurements = primaryMeasurementFrame.hand;
        if (perceptionResult.handCount > 0 && perceptionResult.hands[0].detected)
        {
            std::array<ryoiki::rendering::GestureDtwDebugPoint, 21> liveLandmarks{};
            for (std::size_t landmarkIndex = 0; landmarkIndex < liveLandmarks.size(); ++landmarkIndex)
            {
                const auto& point = perceptionResult.hands[0].landmarks[landmarkIndex];
                liveLandmarks[landmarkIndex] = {point.x, point.y, point.z};
            }
            handle.gestureDtwDebugSource->publishLiveHand(
                frame->frameId(), liveLandmarks);
        }

        // Explicit gesture-recording session (ABI version 23+): drains any
        // pending Begin/Finish/Cancel command from ryoiki_begin_gesture_
        // recording/ryoiki_finish_gesture_recording/ryoiki_cancel_gesture_
        // recording and, once its internal lock is established, appends
        // this frame's observation to the session's own dedicated,
        // non-rolling buffer. This runs before Stage 2 below so
        // recordingSessionActive can pause that unrelated on-demand
        // template-match pass while a take is in progress or finalizing.
        std::string recordingDefinitionName;
        {
            std::lock_guard repositoryLock{handle.gestureRepositoryMutex};
            if (handle.pendingGestureRepositoryCommand != 0
                && handle.gesturePersistenceAvailable)
            {
                std::string repositoryError;
                if (handle.pendingGestureRepositoryCommand == 1)
                {
                    // New definitions intentionally have no row until the
                    // third take commits atomically; existing rows update now.
                    const bool updated = handle.gestureTemplateRepository->updateMetadata(
                        handle.pendingGestureRepositoryId,
                        handle.pendingGestureDefinitionName,
                        handle.pendingGestureRepositoryEnabled, repositoryError);
                    if (updated)
                        handle.gestureTemplateRepository->load(handle.gestureTemplateRegistry,
                            handle.gestureDefinitionMetadata, repositoryError);
                }
                else if (handle.pendingGestureRepositoryCommand == 2)
                    handle.gestureTemplateRepository->remove(
                        handle.pendingGestureRepositoryId, handle.gestureTemplateRegistry, repositoryError);
                else if (handle.pendingGestureRepositoryCommand == 3)
                    handle.gestureTemplateRepository->load(handle.gestureTemplateRegistry,
                        handle.gestureDefinitionMetadata, repositoryError);
                if (!repositoryError.empty()) handle.setError("Gesture repository: " + repositoryError);
                handle.pendingGestureRepositoryCommand = 0;
            }
            recordingDefinitionName = handle.pendingGestureDefinitionName;
        }
        const auto recordingUpdate = handle.gestureRecordingSession.processFrame(
            perceptionResult,
            frame->frameId(),
            frame->captureTimestampUs(),
            handle.gestureTemplateRegistry,
            handle.gesturePersistenceAvailable ? handle.gestureTemplateRepository.get() : nullptr,
            recordingDefinitionName, true);
        const bool recordingJustCompleted = recordingUpdate.state
                == ryoiki::hand_input::recognition::GestureRecordingState::Completed
            && handle.lastGestureRecordingState
                != ryoiki::hand_input::recognition::GestureRecordingState::Completed;
        handle.lastGestureRecordingState = recordingUpdate.state;
        if (recordingJustCompleted)
        {
            // Do not let the just-recorded take immediately validate against
            // itself. Test mode must accumulate a fresh post-registration
            // candidate window.
            handle.handMeasurementStage.reset();
            handle.gestureMatchStabilizer.resetForRegistryChange();
            handle.twoHandGestureMatchStabilizer.resetForRegistryChange();
            handle.lastGestureRecognitionTimestampUs = frame->captureTimestampUs();
        }
        const bool recordingSessionActive = handle.gestureRecordingSession.isActive();
        {
            RyoikiGestureRecordingStatus recordingSnapshot{};
            recordingSnapshot.abi_version = kRyoikiAbiVersion;
            recordingSnapshot.struct_size =
                static_cast<std::uint32_t>(sizeof(RyoikiGestureRecordingStatus));
            recordingSnapshot.state = static_cast<std::uint32_t>(recordingUpdate.state);
            recordingSnapshot.track_id = recordingUpdate.trackId;
            recordingSnapshot.began_frame_id = recordingUpdate.beganFrameId;
            recordingSnapshot.began_timestamp_us = recordingUpdate.beganTimestampUs;
            recordingSnapshot.last_frame_id = recordingUpdate.lastFrameId;
            recordingSnapshot.last_timestamp_us = recordingUpdate.lastTimestampUs;
            recordingSnapshot.sample_count = recordingUpdate.sampleCount;
            recordingSnapshot.usable_sample_count = recordingUpdate.usableSampleCount;
            recordingSnapshot.last_result_template_id = recordingUpdate.lastResultTemplateId;
            recordingSnapshot.current_take = recordingUpdate.currentTake;
            recordingSnapshot.accepted_take_count = recordingUpdate.acceptedTakeCount;
            recordingSnapshot.required_take_count = recordingUpdate.requiredTakeCount;
            recordingSnapshot.attempt_count = recordingUpdate.attemptCount;
            std::memset(
                recordingSnapshot.rejection_reason, 0, sizeof(recordingSnapshot.rejection_reason));
            std::memcpy(
                recordingSnapshot.rejection_reason, recordingUpdate.rejectionReason.data(),
                (std::min)(
                    recordingUpdate.rejectionReason.size(),
                    sizeof(recordingSnapshot.rejection_reason) - 1));

            std::lock_guard recordingLock{handle.gestureRecordingMutex};
            handle.gestureRecordingStatus = recordingSnapshot;
        }

        // Gesture recognition (Stage 2): on-demand, throttled recognition
        // pass over the primary track's rolling candidate-window history,
        // plus consumption of any pending template-registration request.
        // Both read/write handle.handMeasurementStage and
        // handle.gestureTemplateRegistry, which (like the rest of
        // handMeasurementStage's slots) are perception-worker-owned and never
        // synchronized for cross-thread access, so both run here - the
        // architecture's existing recognition scheduling point - rather than
        // from a WPF-called ABI function. Only handle.gestureRecognitionMutex
        // and the snapshot it guards are safe to touch from another thread.
        {
            namespace gesture_recognition = ryoiki::hand_input::recognition;
            namespace gesture_measurements = ryoiki::hand_input::measurements;
            using CandidateArray = std::array<
                gesture_measurements::GestureCandidateWindow,
                gesture_measurements::kGestureCandidateDurationsMs.size()>;

            const auto primaryTrackId = multiHandMeasurements.handCount > 0
                ? multiHandMeasurements.hands[0].trackId
                : ryoiki::hand_perception::HandTrackId{0};

            if (handle.gestureTemplateRegistrationRequested.exchange(false, std::memory_order_acq_rel))
            {
                auto requestedTrackId = static_cast<ryoiki::hand_perception::HandTrackId>(
                    handle.gestureTemplateRegistrationTrackId.load(std::memory_order_acquire));
                if (requestedTrackId == 0)
                {
                    requestedTrackId = primaryTrackId;
                }
                const auto requestedTemplateId =
                    handle.gestureTemplateRegistrationTemplateId.load(std::memory_order_acquire);

                std::uint32_t status = 3; // no_history
                std::string reason = "The requested track has no active candidate-window history.";
                const auto* history =
                    handle.handMeasurementStage.gestureCandidateHistory(requestedTrackId);
                if (history != nullptr)
                {
                    CandidateArray windows{};
                    const auto written = history->buildCandidateWindows(windows);
                    if (written == 0)
                    {
                        reason = "No eligible candidate window is available for this track yet.";
                    }
                    else
                    {
                        // kGestureCandidateDurationsMs is ascending and
                        // buildCandidateWindows writes in that order, so the
                        // last written entry is the longest (best-
                        // conditioned) eligible candidate.
                        const auto creation =
                            gesture_recognition::createTemplateFromCandidateWindow(windows[written - 1]);
                        if (creation.created)
                        {
                            if (handle.gestureTemplateRegistry.registerTemplate(
                                    requestedTemplateId, creation.sequence))
                            {
                                handle.gestureMatchStabilizer.resetForRegistryChange();
                                handle.twoHandGestureMatchStabilizer.resetForRegistryChange();
                                status = 1;
                                reason.clear();
                            }
                            else
                            {
                                status = 2;
                                reason = "The template registry rejected the recording.";
                            }
                        }
                        else
                        {
                            status = 2;
                            reason = creation.rejectionReason;
                        }
                    }
                }

                std::lock_guard gestureLock{handle.gestureRecognitionMutex};
                handle.gestureRecognitionSnapshot.last_registration_status = status;
                handle.gestureRecognitionSnapshot.last_registration_template_id = requestedTemplateId;
                std::memset(
                    handle.gestureRecognitionSnapshot.last_registration_reason, 0,
                    sizeof(handle.gestureRecognitionSnapshot.last_registration_reason));
                std::memcpy(
                    handle.gestureRecognitionSnapshot.last_registration_reason, reason.data(),
                    (std::min)(reason.size(),
                        sizeof(handle.gestureRecognitionSnapshot.last_registration_reason) - 1));
            }

            constexpr std::uint64_t kGestureRecognitionIntervalUs = 200'000; // ~5 Hz on-demand cadence.
            const auto captureTimestampUs = frame->captureTimestampUs();
            // Pause this unrelated on-demand template-match diagnostic while
            // an explicit recording session is awaiting a hand or actively
            // recording, so it does not spend perception-worker time or log
            // noise scoring the take currently being captured.
            const bool dueForRecognition = !recordingSessionActive
                && primaryTrackId != 0
                && (handle.lastGestureRecognitionTimestampUs == 0
                    || captureTimestampUs < handle.lastGestureRecognitionTimestampUs
                    || captureTimestampUs - handle.lastGestureRecognitionTimestampUs
                        >= kGestureRecognitionIntervalUs);
            if (recordingSessionActive || primaryTrackId == 0)
            {
                // PR Reset semantics: losing the live candidate clears only
                // consecutive confirmation. The last trigger still owns its
                // cooldown and duplicate-segment suppression state.
                handle.gestureMatchStabilizer.reset();
                handle.twoHandGestureMatchStabilizer.reset();
            }
            if (dueForRecognition)
            {
                handle.lastGestureRecognitionTimestampUs = captureTimestampUs;

                RyoikiGestureRecognitionSnapshot snapshot{};
                snapshot.abi_version = kRyoikiAbiVersion;
                snapshot.struct_size = static_cast<std::uint32_t>(sizeof(RyoikiGestureRecognitionSnapshot));
                snapshot.frame_id = frame->frameId();
                snapshot.timestamp_us = captureTimestampUs;
                snapshot.track_id = static_cast<std::uint32_t>(primaryTrackId);
                snapshot.template_count = static_cast<std::uint32_t>(handle.gestureTemplateRegistry.size());
                RyoikiGestureDtwDebugSnapshot dtwDebug{};
                dtwDebug.abi_version = kRyoikiAbiVersion;
                dtwDebug.struct_size =
                    static_cast<std::uint32_t>(sizeof(RyoikiGestureDtwDebugSnapshot));
                dtwDebug.frame_id = frame->frameId();
                dtwDebug.timestamp_us = captureTimestampUs;
                dtwDebug.threshold_score = gesture_recognition::confidenceToScore(
                    gesture_recognition::kConfidenceThreshold);

                bool twoHandMatched = false;
                std::uint32_t twoHandTemplateId = 0;
                float twoHandConfidence = 0.0F;
                std::uint64_t twoHandStartUs = 0;
                std::uint64_t twoHandEndUs = 0;
                float twoHandBestScore = (std::numeric_limits<float>::max)();
                if (perceptionResult.handCount >= 2)
                {
                    for (const auto durationMs : gesture_measurements::kGestureCandidateDurationsMs)
                    {
                        const auto source = handle.twoHandGestureHistory.window(
                            captureTimestampUs, static_cast<std::uint64_t>(durationMs) * 1000);
                        const auto candidate = gesture_recognition::createTwoHandCandidate(source);
                        if (!candidate.value.valid) continue;
                        for (const auto& record : handle.gestureTemplateRegistry.records())
                        {
                            const auto twoHandCount = static_cast<std::size_t>(std::count_if(
                                record.trials.begin(), record.trials.begin() + record.trialCount,
                                [](const auto& trial)
                                { return trial.kind == gesture_recognition::GestureTrialKind::TwoHandDynamic; }));
                            if (!record.active
                                || twoHandCount < gesture_recognition::kRequiredGestureTemplateCount) continue;
                            for (std::size_t trialIndex = 0; trialIndex < record.trialCount; ++trialIndex)
                            {
                                const auto* reference = std::get_if<gesture_recognition::TwoHandTemplate>(
                                    &record.trials[trialIndex].payload);
                                if (reference == nullptr) continue;
                                const auto comparison = gesture_recognition::compareTwoHand(
                                    candidate.value, *reference);
                                if (!comparison.eligible || comparison.score >= twoHandBestScore) continue;
                                twoHandMatched = true;
                                twoHandBestScore = comparison.score;
                                twoHandTemplateId = record.templateId;
                                twoHandConfidence = comparison.confidence;
                                twoHandStartUs = source.empty() ? captureTimestampUs : source.front().timestampUs;
                                twoHandEndUs = source.empty() ? captureTimestampUs : source.back().timestampUs;
                            }
                        }
                    }
                }
                if (twoHandMatched)
                {
                    const gesture_recognition::GestureMatchCandidate candidate{
                        twoHandTemplateId, twoHandConfidence, captureTimestampUs,
                        twoHandStartUs, twoHandEndUs};
                    const auto stabilization =
                        handle.twoHandGestureMatchStabilizer.process(&candidate);
                    if (stabilization.confirmed)
                    {
                        gesture_recognition::HandEvent event{};
                        event.id = twoHandTemplateId;
                        event.confidence = twoHandConfidence;
                        event.inputQuality = twoHandConfidence;
                        event.endedFrameId = frame->frameId();
                        event.beganTimestampUs = twoHandStartUs;
                        event.endedTimestampUs = twoHandEndUs;
                        event.durationUs = twoHandEndUs >= twoHandStartUs
                            ? twoHandEndUs - twoHandStartUs : 0;
                        handle.handEventRing.publish(event);
                    }
                }
                else
                {
                    static_cast<void>(handle.twoHandGestureMatchStabilizer.process(nullptr));
                }

                const auto* history =
                    handle.handMeasurementStage.gestureCandidateHistory(primaryTrackId);
                if (history != nullptr)
                {
                    CandidateArray windows{};
                    const auto written = history->buildCandidateWindows(windows);
                    snapshot.candidate_count = static_cast<std::uint32_t>(written);
                    snapshot.has_result = 1;

                    const auto match = gesture_recognition::findBestMatch(
                        windows, written, handle.gestureTemplateRegistry);
                    if (match.matched && perceptionResult.handCount < 2)
                    {
                        const gesture_recognition::GestureMatchCandidate candidate{
                            match.templateId,
                            gesture_recognition::scoreToConfidence(match.comparison.score),
                            captureTimestampUs,
                            match.candidate.startTimestampUs,
                            match.candidate.endTimestampUs};
                        const auto stabilization =
                            handle.gestureMatchStabilizer.process(&candidate);
                        if (stabilization.confirmed)
                        {
                            gesture_recognition::HandEvent event{};
                            event.id = match.templateId;
                            event.confidence = candidate.confidence;
                            event.inputQuality = candidate.confidence;
                            event.endedFrameId = frame->frameId();
                            event.beganTimestampUs = candidate.segmentStartUs;
                            event.endedTimestampUs = candidate.segmentEndUs;
                            event.durationUs = candidate.segmentEndUs >= candidate.segmentStartUs
                                ? candidate.segmentEndUs - candidate.segmentStartUs
                                : 0;
                            handle.handEventRing.publish(event);
                        }
                    }
                    else
                    {
                        static_cast<void>(handle.gestureMatchStabilizer.process(nullptr));
                    }
                    const gesture_recognition::GestureTemplateAttempt* diagnosticAttempt = nullptr;
                    if (!match.attempts.empty())
                    {
                        diagnosticAttempt = &*std::min_element(
                            match.attempts.begin(), match.attempts.end(),
                            [](const auto& left, const auto& right)
                            {
                                return left.outcome.comparison.score < right.outcome.comparison.score;
                            });
                    }

                    const ryoiki::hand_input::measurements::GestureCandidateWindow* bestCandidate = nullptr;
                    const gesture_recognition::ComparisonResult* bestComparison = nullptr;
                    std::uint32_t bestTemplateId = 0;
                    std::uint32_t bestDurationMs = 0;
                    std::size_t bestTemplateTrialIndex = 0;
                    std::string_view bestReason;
                    if (match.matched)
                    {
                        bestCandidate = &match.candidate;
                        bestComparison = &match.comparison;
                        bestTemplateId = match.templateId;
                        bestDurationMs = match.candidateDurationMs;
                        bestTemplateTrialIndex = match.templateTrialIndex;
                        bestReason = match.comparison.reason;
                    }
                    else if (diagnosticAttempt != nullptr
                        && diagnosticAttempt->candidateIndex < written)
                    {
                        bestCandidate = &windows[diagnosticAttempt->candidateIndex];
                        bestComparison = &diagnosticAttempt->outcome.comparison;
                        bestTemplateId = diagnosticAttempt->templateId;
                        bestDurationMs = diagnosticAttempt->candidateDurationMs;
                        bestTemplateTrialIndex = diagnosticAttempt->templateTrialIndex;
                        bestReason = !diagnosticAttempt->outcome.topologyRejectionReason.empty()
                            ? std::string_view{diagnosticAttempt->outcome.topologyRejectionReason}
                            : std::string_view{diagnosticAttempt->outcome.comparison.reason};
                    }

                    if (bestCandidate != nullptr && bestComparison != nullptr)
                    {
                        snapshot.best_template_id = bestTemplateId;
                        snapshot.best_candidate_duration_ms = bestDurationMs;
                        snapshot.best_eligible = bestComparison->eligible ? 1U : 0U;
                        snapshot.best_usable_frame_count =
                            static_cast<std::uint32_t>(bestCandidate->usableFrameCount);
                        snapshot.best_source_frame_count =
                            static_cast<std::uint32_t>(bestCandidate->sourceFrameCount);
                        snapshot.best_active_segment_valid = bestCandidate->activeSegment.valid ? 1U : 0U;
                        snapshot.best_active_segment_path_velocity = bestCandidate->activeSegment.pathVelocity;
                        snapshot.best_score = bestComparison->score;
                        snapshot.best_confidence = gesture_recognition::scoreToConfidence(bestComparison->score);
                        snapshot.best_warp_ratio = bestComparison->warpRatio;
                        snapshot.best_has_warp_ratio = bestComparison->hasWarpRatio ? 1U : 0U;
                        snapshot.best_reverse_score = bestComparison->reverseScore;
                        snapshot.best_has_reverse_score = bestComparison->hasReverseScore ? 1U : 0U;
                        snapshot.best_candidate_start_timestamp_us = bestCandidate->startTimestampUs;
                        snapshot.best_candidate_end_timestamp_us = bestCandidate->endTimestampUs;
                        std::memcpy(snapshot.best_rejection_reason, bestReason.data(),
                            (std::min)(bestReason.size(), sizeof(snapshot.best_rejection_reason) - 1));

                        dtwDebug.template_id = bestTemplateId;
                        dtwDebug.candidate_duration_ms = bestDurationMs;
                        dtwDebug.eligible = bestComparison->eligible ? 1U : 0U;
                        dtwDebug.score = bestComparison->score;
                        dtwDebug.confidence = snapshot.best_confidence;
                        dtwDebug.warp_ratio = bestComparison->warpRatio;
                        dtwDebug.reverse_score = bestComparison->reverseScore;
                        dtwDebug.has_warp_ratio = bestComparison->hasWarpRatio ? 1U : 0U;
                        dtwDebug.has_reverse_score = bestComparison->hasReverseScore ? 1U : 0U;
                        const auto& breakdown = bestComparison->breakdown;
                        const float breakdownValues[12]{breakdown.jointScore, breakdown.boneScore,
                            breakdown.curlScore, breakdown.fingerStateScore, breakdown.spacingScore,
                            breakdown.motionScore, breakdown.palmTurnScore, breakdown.depthScore,
                            breakdown.handednessScore, breakdown.sizeScore,
                            breakdown.translationScore, breakdown.totalScore};
                        std::memcpy(dtwDebug.score_breakdown, breakdownValues, sizeof(breakdownValues));
                        dtwDebug.path_count = static_cast<std::uint32_t>((std::min)(
                            bestComparison->path.size(),
                            static_cast<std::size_t>(kRyoikiMaxGestureDtwPathPoints)));
                        for (std::size_t pathIndex = 0; pathIndex < dtwDebug.path_count; ++pathIndex)
                        {
                            dtwDebug.candidate_indices[pathIndex] = static_cast<std::uint8_t>(
                                bestComparison->path[pathIndex].candidateIndex);
                            dtwDebug.template_indices[pathIndex] = static_cast<std::uint8_t>(
                                bestComparison->path[pathIndex].templateIndex);
                        }
                        std::memcpy(dtwDebug.rejection_reason, bestReason.data(),
                            (std::min)(bestReason.size(), sizeof(dtwDebug.rejection_reason) - 1));

                        const auto* templateRecord =
                            handle.gestureTemplateRegistry.find(bestTemplateId);
                        if (templateRecord != nullptr
                            && bestTemplateTrialIndex < templateRecord->trialCount)
                        {
                            const auto* oneHandTemplate =
                                std::get_if<gesture_recognition::UnifiedSequenceTemplate>(
                                    &templateRecord->trials[bestTemplateTrialIndex].payload);
                            if (oneHandTemplate != nullptr)
                            {
                                auto renderPacket = ryoiki::rendering::buildGestureDtwDebugRenderPacket(
                                    frame->frameId(), captureTimestampUs,
                                    static_cast<std::uint32_t>(primaryTrackId), bestTemplateId,
                                    *bestCandidate, *oneHandTemplate,
                                    *bestComparison);
                                handle.gestureDtwDebugSource->publish(std::move(renderPacket));
                            }
                        }
                    }
                }
                else
                {
                    static_cast<void>(handle.gestureMatchStabilizer.process(nullptr));
                }

                std::lock_guard gestureLock{handle.gestureRecognitionMutex};
                snapshot.last_registration_status =
                    handle.gestureRecognitionSnapshot.last_registration_status;
                snapshot.last_registration_template_id =
                    handle.gestureRecognitionSnapshot.last_registration_template_id;
                std::memcpy(
                    snapshot.last_registration_reason,
                    handle.gestureRecognitionSnapshot.last_registration_reason,
                    sizeof(snapshot.last_registration_reason));
                handle.gestureRecognitionSnapshot = snapshot;
                handle.gestureDtwDebugSnapshot = dtwDebug;
            }
        }

        const auto findTrackedMeasurement =
            [&multiHandMeasurements](
                const ryoiki::hand_perception::HandTrackId trackId)
                -> const ryoiki::hand_input::measurements::TrackedHandMeasurement*
        {
            if (trackId == 0) return nullptr;
            for (std::size_t index = 0;
                index < multiHandMeasurements.handCount;
                ++index)
            {
                if (multiHandMeasurements.hands[index].trackId == trackId)
                {
                    return &multiHandMeasurements.hands[index];
                }
            }
            return nullptr;
        };
        const auto isValidRotationMeasurement =
            [](const ryoiki::hand_input::measurements::TrackedHandMeasurement&
                measurement)
        {
            return measurement.hand.present
                && measurement.hand.quality
                    == ryoiki::hand_input::measurements::HandMeasurementQuality::Valid;
        };

        const ryoiki::hand_input::measurements::TrackedHandMeasurement*
            rotationMeasurement =
                findTrackedMeasurement(handle.palmRotationTrackId);
        // Diagnostics only. Perception drops a track slot after its own grace,
        // so a hand that returns later is a new track and its handedness is
        // never compared with the lost one. Recording both sides shows whether
        // handedness is stable enough to identify a hand across that gap.
        float rebindCandidateHandedness = -1.0F;
        if (handle.capturePalmRotationReference.load(std::memory_order_acquire))
        {
            // A capture request is a level-triggered contract: keep it pending
            // until a valid typed measurement has supplied both references.
            // Prefer the already captured track when it remains visible;
            // otherwise bind this rotation session to the first valid track.
            if (rotationMeasurement == nullptr
                || !isValidRotationMeasurement(*rotationMeasurement))
            {
                rotationMeasurement = nullptr;
                for (std::size_t index = 0;
                    index < multiHandMeasurements.handCount;
                    ++index)
                {
                    if (isValidRotationMeasurement(
                            multiHandMeasurements.hands[index]))
                    {
                        rotationMeasurement =
                            &multiHandMeasurements.hands[index];
                        break;
                    }
                }
            }
            if (rotationMeasurement != nullptr)
            {
                const bool absoluteCaptured =
                    handle.palmBasisRotationTracker.captureReference(
                        rotationMeasurement->hand);
                const bool incrementalCaptured =
                    handle.incrementalPalmBasisRotationTracker.captureReference(
                        rotationMeasurement->hand);
                if (absoluteCaptured && incrementalCaptured)
                {
                    handle.palmRotationTrackId = rotationMeasurement->trackId;
                    handle.palmRotationMissingFrames = 0;
                    handle.palmRotationRebindCandidateId = 0;
                    handle.palmRotationRebindCandidateFrames = 0;
                    handle.palmRotationLastHandedness =
                        rotationMeasurement->hand.handedness;
                    handle.palmRotationLastScreenValid =
                        rotationMeasurement->screenPalm.valid;
                    if (rotationMeasurement->screenPalm.valid)
                    {
                        handle.palmRotationLastScreenX =
                            rotationMeasurement->screenPalm.centerX;
                        handle.palmRotationLastScreenY =
                            rotationMeasurement->screenPalm.centerY;
                    }
                    handle.palmRotationEskf.reset();
                    handle.palmRotationOneEuroFilter.reset();
                    handle.palmRotationObservationGate.reset();
                    handle.capturePalmRotationReference.store(
                        false, std::memory_order_release);
                }
            }
        }
        else if (handle.palmRotationTrackId != 0)
        {
            const bool selectedTrackValid =
                rotationMeasurement != nullptr
                && isValidRotationMeasurement(*rotationMeasurement);
            if (selectedTrackValid)
            {
                handle.palmRotationMissingFrames = 0;
                handle.palmRotationRebindCandidateId = 0;
                handle.palmRotationRebindCandidateFrames = 0;
                handle.palmRotationLastHandedness =
                    rotationMeasurement->hand.handedness;
                handle.palmRotationLastScreenValid =
                    rotationMeasurement->screenPalm.valid;
                if (rotationMeasurement->screenPalm.valid)
                {
                    handle.palmRotationLastScreenX =
                        rotationMeasurement->screenPalm.centerX;
                    handle.palmRotationLastScreenY =
                        rotationMeasurement->screenPalm.centerY;
                }
            }
            else
            {
                ++handle.palmRotationMissingFrames;
                const ryoiki::hand_input::measurements::TrackedHandMeasurement*
                    candidate = nullptr;
                float candidateScore = 1.0e9F;
                for (std::size_t index = 0;
                    index < multiHandMeasurements.handCount;
                    ++index)
                {
                    const auto& current = multiHandMeasurements.hands[index];
                    if (current.trackId == handle.palmRotationTrackId
                        || !isValidRotationMeasurement(current))
                    {
                        continue;
                    }
                    float score = std::abs(
                        current.hand.handedness
                        - handle.palmRotationLastHandedness);
                    if (handle.palmRotationLastScreenValid
                        && current.screenPalm.valid)
                    {
                        score += std::hypot(
                            current.screenPalm.centerX
                                - handle.palmRotationLastScreenX,
                            current.screenPalm.centerY
                                - handle.palmRotationLastScreenY);
                    }
                    if (score < candidateScore)
                    {
                        candidate = &current;
                        candidateScore = score;
                    }
                }

                if (candidate != nullptr)
                {
                    rebindCandidateHandedness = candidate->hand.handedness;
                }
                if (candidate == nullptr)
                {
                    handle.palmRotationRebindCandidateId = 0;
                    handle.palmRotationRebindCandidateFrames = 0;
                }
                else if (candidate->trackId
                    == handle.palmRotationRebindCandidateId)
                {
                    ++handle.palmRotationRebindCandidateFrames;
                }
                else
                {
                    handle.palmRotationRebindCandidateId = candidate->trackId;
                    handle.palmRotationRebindCandidateFrames = 1;
                }

                constexpr std::uint32_t kRequiredRebindCandidateFrames = 2;
                if (candidate != nullptr
                    && handle.palmRotationRebindCandidateFrames
                        >= kRequiredRebindCandidateFrames)
                {
                    const bool absoluteCaptured =
                        handle.palmBasisRotationTracker.captureReference(
                            candidate->hand);
                    const bool incrementalCaptured =
                        handle.incrementalPalmBasisRotationTracker.captureReference(
                            candidate->hand);
                    if (absoluteCaptured && incrementalCaptured)
                    {
                        handle.palmRotationTrackId = candidate->trackId;
                        handle.palmRotationMissingFrames = 0;
                        handle.palmRotationRebindCandidateId = 0;
                        handle.palmRotationRebindCandidateFrames = 0;
                        handle.palmRotationLastHandedness =
                            candidate->hand.handedness;
                        handle.palmRotationLastScreenValid =
                            candidate->screenPalm.valid;
                        if (candidate->screenPalm.valid)
                        {
                            handle.palmRotationLastScreenX =
                                candidate->screenPalm.centerX;
                            handle.palmRotationLastScreenY =
                                candidate->screenPalm.centerY;
                        }
                        // The new track has a new local reference. Preserve the
                        // last published orientation while the gate establishes
                        // a stable trajectory in that new reference frame.
                        handle.palmRotationObservationGate.beginReacquisition();
                        rotationMeasurement = candidate;
                    }
                }
            }
        }
        rotationMeasurement =
            findTrackedMeasurement(handle.palmRotationTrackId);
        const auto rotationMeasurementFrame = rotationMeasurement != nullptr
            ? ryoiki::hand_input::measurements::HandMeasurementFrame{
                rotationMeasurement->hand,
                rotationMeasurement->screenPalm}
            : ryoiki::hand_input::measurements::HandMeasurementFrame{};
        const auto& handMeasurements = rotationMeasurementFrame.hand;
        const auto& screenPalm = rotationMeasurementFrame.screenPalm;
        auto absolutePalmRotation =
            handle.palmBasisRotationTracker.estimate(handMeasurements);
        auto incrementalPalmRotation =
            handle.incrementalPalmBasisRotationTracker.estimate(handMeasurements);
        static_cast<void>(
            handle.incrementalPalmBasisRotationTracker.captureReference(
                handMeasurements));
        // The orthonormal basis has no Kabsch residual. Keep a small finite
        // observation noise for ESKF compatibility; One Euro ignores this value.
        absolutePalmRotation.fitError = 0.02F;
        incrementalPalmRotation.fitError = 0.02F;
        const auto gatedPalmRotation =
            handle.palmRotationObservationGate.update(absolutePalmRotation);
        if (gatedPalmRotation.reacquired)
        {
            handle.palmRotationEskf.reset();
            handle.palmRotationOneEuroFilter.reset();
        }
        ryoiki::hand_input::measurements::PalmRotationEstimate palmRotation{};
        switch (palmRotationFilter)
        {
        case PalmRotationFilter::Raw:
            palmRotation = gatedPalmRotation.observation;
            break;
        case PalmRotationFilter::Eskf:
            palmRotation = handle.palmRotationEskf.update(
                gatedPalmRotation.observation,
                incrementalPalmRotation,
                frame->captureTimestampUs());
            break;
        case PalmRotationFilter::OneEuro:
            palmRotation = handle.palmRotationOneEuroFilter.update(
                gatedPalmRotation.observation,
                frame->captureTimestampUs());
            break;
        }
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
            // Gate and drift diagnostics must not depend on the legacy
            // six-point comparison reference. A reference that never captured
            // previously suppressed every row and produced an empty log.
            ryoiki::hand_input::measurements::PalmRotationEstimate sixPoint{};
            ryoiki::hand_input::measurements::PalmRotationEstimate palmBasis{};
            if (hasComparisonReference)
            {
                sixPoint =
                    comparisonSixPointTracker.estimate(perceptionResult.hand);
                palmBasis =
                    comparisonPalmBasisTracker.estimate(handMeasurements);
            }
            {
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
                    // Zero means no rotation session is bound, which is not a
                    // tracking failure. Without this the two are
                    // indistinguishable in the log.
                    << handle.palmRotationTrackId << ','
                    << handle.palmRotationMissingFrames << ','
                    // Negative means unavailable, matching the ABI's handedness
                    // convention.
                    << (handMeasurements.present
                        ? handMeasurements.handedness
                        : -1.0F) << ','
                    << handle.palmRotationLastHandedness << ','
                    << rebindCandidateHandedness << ','
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
                    << differenceVector[2] << ','
                    << static_cast<std::uint32_t>(gatedPalmRotation.state) << ','
                    << static_cast<std::uint32_t>(gatedPalmRotation.rejection) << ','
                    << gatedPalmRotation.rawDeltaDegrees << ','
                    << gatedPalmRotation.stableCandidateFrames << ','
                    << gatedPalmRotation.outlierFrames << ','
                    << gatedPalmRotation.invalidFrames << ','
                    << (gatedPalmRotation.reacquired ? 1 : 0) << ','
                    << gatedPalmRotation.rebaseOffsetDegrees << ','
                    << gatedPalmRotation.rebaseStepDegrees << ','
                    << (palmRotation.valid ? 1 : 0) << '\n';
                ++comparisonRows;
                if (comparisonRows % 120 == 0) rotationComparisonCsv.flush();
            }
        }
        const auto openPalm =
            ryoiki::hand_input::recognition::recognizeOpenPalmState(
                primaryHandMeasurements);
        const auto openPalmState = handle.openPalmRecognizer.process(
            openPalm,
            primaryHandMeasurements.trackingQuality,
            frame->frameId(),
            frame->captureTimestampUs());
        const auto swipeEvent = handle.swipeRecognizer.process(
            primaryMeasurementFrame,
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

            handle.hands.frame_id = frame->frameId();
            handle.hands.capture_timestamp_us = frame->captureTimestampUs();
            handle.hands.hand_count = static_cast<std::uint32_t>(
                (std::min)(
                    perceptionResult.handCount,
                    ryoiki::hand_perception::kMaxPerceivedHands));
            handle.hands.flags =
                perceptionResult.crossingActive ? 1U : 0U;
            handle.hands.crossing_owner_track_id =
                perceptionResult.crossingOwnerTrackId;
            handle.hands.reserved = 0;
            std::memset(handle.hands.hands, 0, sizeof(handle.hands.hands));
            for (std::size_t handIndex = 0;
                handIndex < handle.hands.hand_count;
                ++handIndex)
            {
                const auto& source = perceptionResult.hands[handIndex];
                auto& destination = handle.hands.hands[handIndex];
                destination.track_id = perceptionResult.trackIds[handIndex];
                destination.flags = perceptionResult.handFlags[handIndex];
                destination.confidence = source.confidence;
                destination.handedness = source.handedness;
                destination.filtered_handedness =
                    perceptionResult.filteredHandedness[handIndex];
                destination.bbox[0] = std::clamp(
                    source.box.left * inverseWidth, 0.0F, 1.0F);
                destination.bbox[1] = std::clamp(
                    source.box.top * inverseHeight, 0.0F, 1.0F);
                destination.bbox[2] = std::clamp(
                    source.box.right * inverseWidth, 0.0F, 1.0F);
                destination.bbox[3] = std::clamp(
                    source.box.bottom * inverseHeight, 0.0F, 1.0F);
                for (std::size_t landmarkIndex = 0;
                    landmarkIndex < source.landmarks.size();
                    ++landmarkIndex)
                {
                    const auto& landmark = source.landmarks[landmarkIndex];
                    destination.landmarks[landmarkIndex * 3] = std::clamp(
                        landmark.x * inverseWidth, 0.0F, 1.0F);
                    destination.landmarks[landmarkIndex * 3 + 1] = std::clamp(
                        landmark.y * inverseHeight, 0.0F, 1.0F);
                    destination.landmarks[landmarkIndex * 3 + 2] =
                        landmark.z * inverseWidth;
                    const auto& worldLandmark =
                        source.worldLandmarks[landmarkIndex];
                    destination.world_landmarks[landmarkIndex * 3] =
                        worldLandmark.x;
                    destination.world_landmarks[landmarkIndex * 3 + 1] =
                        worldLandmark.y;
                    destination.world_landmarks[landmarkIndex * 3 + 2] =
                        worldLandmark.z;
                }
            }

            handle.topology.frame_id = frame->frameId();
            handle.topology.capture_timestamp_us = frame->captureTimestampUs();
            handle.topology.summary_count = static_cast<std::uint32_t>(
                (std::min)(
                    multiHandMeasurements.handCount,
                    static_cast<std::size_t>(kRyoikiMaxHands)));
            handle.topology.reserved = 0;
            std::memset(
                handle.topology.summaries, 0, sizeof(handle.topology.summaries));
            for (std::size_t summaryIndex = 0;
                summaryIndex < handle.topology.summary_count;
                ++summaryIndex)
            {
                const auto& tracked = multiHandMeasurements.hands[summaryIndex];
                const auto& summary = tracked.topologySummary;
                auto& destination = handle.topology.summaries[summaryIndex];
                destination.track_id =
                    static_cast<std::uint32_t>(tracked.trackId);
                destination.valid = summary.valid ? 1U : 0U;
                destination.sample_count =
                    static_cast<std::uint32_t>(summary.sampleCount);
                destination.finger_state_transition_count =
                    summary.fingerStateTransitionCount;
                destination.start_finger_state_mask =
                    summary.startFingerStateMask;
                destination.end_finger_state_mask = summary.endFingerStateMask;
                destination.signed_palm_area_sign_changes =
                    summary.signedPalmAreaSignChanges;
                destination.reserved = 0;
                destination.first_frame_id = summary.firstFrameId;
                destination.last_frame_id = summary.lastFrameId;
                destination.duration_us = summary.durationUs;
                destination.palm_travel = summary.palmTravel;
                destination.palm_orientation_range_radians =
                    summary.palmOrientationRangeRadians;
                destination.handedness_range = summary.handednessRange;
                destination.handedness_mean = summary.handednessMean;
                destination.finger_straightness_range_max =
                    summary.fingerStraightnessRangeMax;
                destination.signed_palm_area_range = summary.signedPalmAreaRange;
                destination.palm_compression_min = summary.palmCompressionMin;
                destination.palm_compression_max = summary.palmCompressionMax;
                destination.palm_compression_drop = summary.palmCompressionDrop;
                destination.palm_depth_range_max = summary.palmDepthRangeMax;
                destination.palm_turn_score = summary.palmTurnScore;
                destination.hand_scale_ratio_range = summary.handScaleRatioRange;
                destination.hand_scale_ratio_delta = summary.handScaleRatioDelta;
                destination.bounding_box_area_ratio_range =
                    summary.boundingBoxAreaRatioRange;
                destination.bounding_box_area_ratio_delta =
                    summary.boundingBoxAreaRatioDelta;
                destination.translation_delta_x = summary.translationDeltaX;
                destination.translation_delta_y = summary.translationDeltaY;
                destination.translation_distance = summary.translationDistance;
                destination.topology_change_score = summary.topologyChangeScore;
                destination.reserved_float = 0.0F;
            }
            const auto& relation = multiHandMeasurements.relation;
            handle.topology.relation.valid = relation.valid ? 1U : 0U;
            handle.topology.relation.first_track_id =
                static_cast<std::uint32_t>(relation.firstTrackId);
            handle.topology.relation.second_track_id =
                static_cast<std::uint32_t>(relation.secondTrackId);
            handle.topology.relation.ordering =
                static_cast<std::uint32_t>(relation.ordering);
            handle.topology.relation.delta_x = relation.deltaX;
            handle.topology.relation.delta_y = relation.deltaY;
            handle.topology.relation.distance = relation.distance;
            handle.topology.relation.angle_radians = relation.angleRadians;
            handle.topology.relation.scale_ratio = relation.scaleRatio;
            handle.topology.relation.quality = relation.quality;

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
                if (primaryHandMeasurements.quality
                    == ryoiki::hand_input::measurements::HandMeasurementQuality::Valid)
                {
                    const std::array axes{
                        primaryHandMeasurements.palmXAxis,
                        primaryHandMeasurements.palmYAxis,
                        primaryHandMeasurements.palmZAxis};
                    for (std::size_t column = 0; column < axes.size(); ++column)
                    {
                        handle.hand.palm_rotation_basis[column * 3] = axes[column].x;
                        handle.hand.palm_rotation_basis[column * 3 + 1] = axes[column].y;
                        handle.hand.palm_rotation_basis[column * 3 + 2] = axes[column].z;
                    }
                    handle.hand.palm_center[0] = primaryHandMeasurements.palmPosition.x;
                    handle.hand.palm_center[1] = primaryHandMeasurements.palmPosition.y;
                    handle.hand.palm_center[2] = primaryHandMeasurements.palmPosition.z;
                    handle.hand.palm_scale = primaryHandMeasurements.palmScale;
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
            input.trackingQuality = handMeasurements.trackingQuality;
            input.screenCenterX = screenPalm.centerX;
            input.screenCenterY = screenPalm.centerY;
            input.screenScale = screenPalm.scale;
            input.relativeRotation = palmRotation.rotation;
            // Basis rotation validity is governed by the typed measurement and
            // tracking-quality gates. Six-point residual no longer rejects a
            // visually stable basis observation.
            input.rotationFitError = palmRotation.fitError;
            input.handPresent = handMeasurements.present;
            input.screenPalmValid = screenPalm.valid;
            input.relativeRotationValid = palmRotation.valid;
            if (cadInteraction->process(input))
            {
                // The interaction re-zeroed after a tracking gap. Capturing a
                // fresh reference here also resets the observation gate, so the
                // rebase offset accumulated during the gap is discarded rather
                // than carried into the resumed session.
                handle.capturePalmRotationReference.store(
                    true, std::memory_order_release);
            }
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
        try
        {
            wchar_t overridePath[32768]{};
            const DWORD overrideLength = GetEnvironmentVariableW(
                L"RYOIKI_GESTURE_DB_PATH", overridePath, static_cast<DWORD>(std::size(overridePath)));
            std::filesystem::path databasePath;
            if (overrideLength > 0 && overrideLength < std::size(overridePath))
            {
                databasePath = overridePath;
            }
            else
            {
                wchar_t localAppData[32768]{};
                const DWORD length = GetEnvironmentVariableW(
                    L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData)));
                databasePath = length > 0
                    ? std::filesystem::path{localAppData} / L"RyoikiTenkai" / L"gestures.db"
                    : std::filesystem::current_path() / L"gestures.db";
            }
            if (databasePath.has_parent_path())
                std::filesystem::create_directories(databasePath.parent_path());
            const auto databasePathUtf8 = databasePath.u8string();
            handle->gestureTemplateRepository = std::make_unique<
                ryoiki::hand_input::recognition::GestureTemplateRepository>(
                    std::string{databasePathUtf8.begin(), databasePathUtf8.end()});
            std::string persistenceError;
            handle->gesturePersistenceAvailable = handle->gestureTemplateRepository->open(persistenceError)
                && handle->gestureTemplateRepository->load(handle->gestureTemplateRegistry,
                    handle->gestureDefinitionMetadata, persistenceError);
            if (!handle->gesturePersistenceAvailable)
                handle->setError("Gesture persistence unavailable: " + persistenceError);
        }
        catch (const std::exception& error)
        {
            handle->setError(std::string{"Gesture persistence unavailable: "} + error.what());
        }
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
            handle->hands = {};
            handle->hands.abi_version = kRyoikiAbiVersion;
            handle->hands.struct_size = static_cast<std::uint32_t>(sizeof(RyoikiHandsResult));
            handle->topology = {};
            handle->topology.abi_version = kRyoikiAbiVersion;
            handle->topology.struct_size =
                static_cast<std::uint32_t>(sizeof(RyoikiHandTopologySnapshot));
            handle->lastError.clear();
            handle->lastDisplayAt = {};
            handle->lastPresentedFrameId = 0;
        }
        handle->framePool.resetStatistics();
        handle->renderFramePool.resetStatistics();
        handle->perceptionMailbox.reset();
        handle->handMeasurementStage.reset();
        handle->twoHandGestureHistory.reset();
        handle->gestureMatchStabilizer.resetForRegistryChange();
        handle->twoHandGestureMatchStabilizer.resetForRegistryChange();
        handle->palmBasisRotationTracker.reset();
        handle->incrementalPalmBasisRotationTracker.reset();
        handle->palmRotationEskf.reset();
        handle->palmRotationOneEuroFilter.reset();
        handle->palmRotationObservationGate.reset();
        handle->palmRotationTrackId = 0;
        handle->palmRotationRebindCandidateId = 0;
        handle->palmRotationMissingFrames = 0;
        handle->palmRotationRebindCandidateFrames = 0;
        handle->palmRotationLastHandedness = -1.0F;
        handle->palmRotationLastScreenX = 0.0F;
        handle->palmRotationLastScreenY = 0.0F;
        handle->palmRotationLastScreenValid = false;
        handle->capturePalmRotationReference.store(false);
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

RYOIKI_EXPORT std::int32_t ryoiki_get_latest_hands(
    RyoikiHandle* handle,
    RyoikiHandsResult* out_result)
{
    if (handle == nullptr || out_result == nullptr)
    {
        return kRyoikiStatusFailure;
    }

    try
    {
        std::lock_guard lock{handle->stateMutex};
        *out_result = handle->hands;
        return kRyoikiStatusSuccess;
    }
    catch (...)
    {
        return kRyoikiStatusFailure;
    }
}

RYOIKI_EXPORT std::int32_t ryoiki_get_latest_hand_topology(
    RyoikiHandle* handle,
    RyoikiHandTopologySnapshot* out_snapshot)
{
    if (handle == nullptr || out_snapshot == nullptr)
    {
        return kRyoikiStatusFailure;
    }

    try
    {
        std::lock_guard lock{handle->stateMutex};
        *out_snapshot = handle->topology;
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

RYOIKI_EXPORT std::int32_t ryoiki_get_latest_gesture_recognition(
    RyoikiHandle* handle,
    RyoikiGestureRecognitionSnapshot* outSnapshot)
{
    if (handle == nullptr || outSnapshot == nullptr)
    {
        return kRyoikiStatusFailure;
    }
    try
    {
        std::lock_guard lock{handle->gestureRecognitionMutex};
        *outSnapshot = handle->gestureRecognitionSnapshot;
        return kRyoikiStatusSuccess;
    }
    catch (...)
    {
        return kRyoikiStatusFailure;
    }
}

RYOIKI_EXPORT std::int32_t ryoiki_get_latest_gesture_dtw_debug(
    RyoikiHandle* handle,
    RyoikiGestureDtwDebugSnapshot* outSnapshot)
{
    if (handle == nullptr || outSnapshot == nullptr)
    {
        return kRyoikiStatusFailure;
    }
    try
    {
        std::lock_guard lock{handle->gestureRecognitionMutex};
        *outSnapshot = handle->gestureDtwDebugSnapshot;
        return kRyoikiStatusSuccess;
    }
    catch (...)
    {
        return kRyoikiStatusFailure;
    }
}

RYOIKI_EXPORT std::int32_t ryoiki_request_gesture_template_registration(
    RyoikiHandle* handle,
    const std::uint32_t trackId,
    const std::uint32_t templateId)
{
    if (handle == nullptr || !handle->running.load())
    {
        return kRyoikiStatusFailure;
    }
    handle->gestureTemplateRegistrationTrackId.store(trackId, std::memory_order_release);
    handle->gestureTemplateRegistrationTemplateId.store(templateId, std::memory_order_release);
    handle->gestureTemplateRegistrationRequested.store(true, std::memory_order_release);
    return kRyoikiStatusSuccess;
}

RYOIKI_EXPORT std::int32_t ryoiki_begin_gesture_recording(
    RyoikiHandle* handle,
    const std::uint32_t templateId)
{
    if (handle == nullptr || !handle->running.load())
    {
        return kRyoikiStatusFailure;
    }
    handle->gestureRecordingSession.submitCommand(
        ryoiki::hand_input::recognition::GestureRecordingCommandKind::Begin, templateId);
    return kRyoikiStatusSuccess;
}

RYOIKI_EXPORT std::int32_t ryoiki_finish_gesture_recording(RyoikiHandle* handle)
{
    if (handle == nullptr || !handle->running.load())
    {
        return kRyoikiStatusFailure;
    }
    handle->gestureRecordingSession.submitCommand(
        ryoiki::hand_input::recognition::GestureRecordingCommandKind::Finish, 0);
    return kRyoikiStatusSuccess;
}

RYOIKI_EXPORT std::int32_t ryoiki_cancel_gesture_recording(RyoikiHandle* handle)
{
    if (handle == nullptr || !handle->running.load())
    {
        return kRyoikiStatusFailure;
    }
    handle->gestureRecordingSession.submitCommand(
        ryoiki::hand_input::recognition::GestureRecordingCommandKind::Cancel, 0);
    return kRyoikiStatusSuccess;
}

RYOIKI_EXPORT std::int32_t ryoiki_get_gesture_recording_status(
    RyoikiHandle* handle,
    RyoikiGestureRecordingStatus* outStatus)
{
    if (handle == nullptr || outStatus == nullptr)
    {
        return kRyoikiStatusFailure;
    }
    try
    {
        std::lock_guard lock{handle->gestureRecordingMutex};
        *outStatus = handle->gestureRecordingStatus;
        return kRyoikiStatusSuccess;
    }
    catch (...)
    {
        return kRyoikiStatusFailure;
    }
}

RYOIKI_EXPORT std::int32_t ryoiki_list_gesture_definitions(
    RyoikiHandle* handle, RyoikiGestureDefinitionList* outList)
{
    if (!handle || !outList) return kRyoikiStatusFailure;
    *outList = {};
    outList->abi_version = kRyoikiAbiVersion;
    outList->struct_size = sizeof(*outList);
    std::lock_guard lock{handle->gestureRepositoryMutex};
    std::vector<ryoiki::hand_input::recognition::GestureDefinitionMetadata> values;
    std::string error;
    if (!handle->gesturePersistenceAvailable
        || !handle->gestureTemplateRepository->list(values, error))
    {
        if (error.empty()) error = "Gesture persistence is unavailable.";
        std::memcpy(outList->error, error.data(), (std::min)(error.size(), sizeof(outList->error)-1));
        return kRyoikiStatusFailure;
    }
    outList->count = static_cast<std::uint32_t>((std::min)(values.size(),
        static_cast<std::size_t>(kRyoikiMaxGestureDefinitions)));
    for (std::size_t i=0;i<outList->count;++i)
    {
        outList->items[i].id=values[i].id; outList->items[i].enabled=values[i].enabled?1U:0U;
        outList->items[i].take_count=values[i].takeCount;
        std::memcpy(outList->items[i].name,values[i].name.data(),(std::min)(values[i].name.size(),sizeof(outList->items[i].name)-1));
    }
    return kRyoikiStatusSuccess;
}

RYOIKI_EXPORT std::int32_t ryoiki_set_gesture_definition_metadata(
    RyoikiHandle* handle, const std::uint32_t id, const char* name, const std::uint32_t enabled)
{
    if (!handle || !name || id==0) return kRyoikiStatusFailure;
    std::lock_guard lock{handle->gestureRepositoryMutex};
    handle->pendingGestureDefinitionId=id;
    handle->pendingGestureDefinitionName=std::string{name,(std::min)(std::strlen(name),std::size_t{63})};
    handle->pendingGestureRepositoryId=id; handle->pendingGestureRepositoryEnabled=enabled!=0;
    handle->pendingGestureRepositoryCommand=1;
    return kRyoikiStatusSuccess;
}

RYOIKI_EXPORT std::int32_t ryoiki_delete_gesture_definition(RyoikiHandle* handle,const std::uint32_t id)
{
    if(!handle||id==0)return kRyoikiStatusFailure;std::lock_guard lock{handle->gestureRepositoryMutex};
    handle->pendingGestureRepositoryId=id;handle->pendingGestureRepositoryCommand=2;return kRyoikiStatusSuccess;
}

RYOIKI_EXPORT std::int32_t ryoiki_reload_gesture_definitions(RyoikiHandle* handle)
{
    if(!handle)return kRyoikiStatusFailure;std::lock_guard lock{handle->gestureRepositoryMutex};
    handle->pendingGestureRepositoryCommand=3;return kRyoikiStatusSuccess;
}

RYOIKI_EXPORT std::int32_t ryoiki_list_gesture_recordings(
    RyoikiHandle* handle, const std::uint32_t definitionId, RyoikiGestureRecordingList* outList)
{
    if (!handle || !outList || definitionId == 0) return kRyoikiStatusFailure;
    *outList = {}; outList->abi_version = kRyoikiAbiVersion; outList->struct_size = sizeof(*outList);
    std::lock_guard lock{handle->gestureRepositoryMutex};
    std::vector<ryoiki::hand_input::recognition::GestureRecordingProvenance> values;
    std::string error;
    if (!handle->gesturePersistenceAvailable
        || !handle->gestureTemplateRepository->listRecordings(definitionId, values, error))
    {
        if (error.empty()) error = "Gesture recording provenance is unavailable.";
        std::memcpy(outList->error, error.data(), (std::min)(error.size(), sizeof(outList->error)-1));
        return kRyoikiStatusFailure;
    }
    outList->count = static_cast<std::uint32_t>((std::min)(values.size(), std::size_t{3}));
    for (std::size_t i=0; i<outList->count; ++i)
    {
        const auto& value=values[i]; auto& item=outList->items[i];
        item.take_index=value.takeIndex; item.accepted=value.quality.accepted?1U:0U;
        item.captured_at_us=value.capturedAtUs; item.duration_ms=value.durationMs;
        item.average_confidence=value.averageConfidence;
        item.source_frame_count=value.quality.sourceFrameCount;
        item.valid_landmark_frame_count=value.quality.validLandmarkFrameCount;
        item.high_confidence_frame_count=value.quality.highConfidenceFrameCount;
        item.frame_count=static_cast<std::uint32_t>(value.frames.size());
        item.effective_fps=value.quality.effectiveFps; item.usable_effective_fps=value.quality.usableEffectiveFps;
        std::memcpy(item.source_id,value.sourceId.data(),(std::min)(value.sourceId.size(),sizeof(item.source_id)-1));
        std::memcpy(item.quality_reason,value.quality.reason.data(),(std::min)(value.quality.reason.size(),sizeof(item.quality_reason)-1));
    }
    return kRyoikiStatusSuccess;
}

RYOIKI_EXPORT std::int32_t ryoiki_export_gesture_recording(
    RyoikiHandle* handle, const std::uint32_t definitionId,
    const std::uint32_t takeIndex, const char* path)
{
    if (!handle || !path || definitionId == 0 || takeIndex == 0) return kRyoikiStatusFailure;
    std::lock_guard lock{handle->gestureRepositoryMutex}; std::string error;
    const bool success=handle->gesturePersistenceAvailable
        && handle->gestureTemplateRepository->exportRecording(definitionId,takeIndex,path,error);
    if(!success&&!error.empty())handle->setError("Gesture recording export: "+error);
    return success?kRyoikiStatusSuccess:kRyoikiStatusFailure;
}

bool ryoikiLoadGestureRecording(RyoikiHandle* handle, const std::uint32_t definitionId,
    const std::uint32_t takeIndex,
    ryoiki::hand_input::recognition::GestureRecordingProvenance& out,
    std::string& error) noexcept
{
    if(!handle||definitionId==0||takeIndex==0){error="Invalid recording selection.";return false;}
    std::lock_guard lock{handle->gestureRepositoryMutex};
    std::vector<ryoiki::hand_input::recognition::GestureRecordingProvenance> values;
    if(!handle->gesturePersistenceAvailable
        ||!handle->gestureTemplateRepository->listRecordings(definitionId,values,error))return false;
    const auto found=std::find_if(values.begin(),values.end(),[takeIndex](const auto& value){return value.takeIndex==takeIndex;});
    if(found==values.end()){error="Recording take was not found.";return false;} out=*found;return true;
}

RYOIKI_EXPORT std::int32_t ryoiki_list_gesture_bindings(
    RyoikiHandle* handle, RyoikiGestureBindingList* outList)
{
    if (!handle || !outList) return kRyoikiStatusFailure;
    *outList = {}; outList->abi_version = kRyoikiAbiVersion;
    outList->struct_size = sizeof(*outList);
    std::lock_guard lock{handle->gestureRepositoryMutex};
    std::vector<ryoiki::hand_input::recognition::GestureBindingMetadata> values;
    std::string error;
    if (!handle->gesturePersistenceAvailable
        || !handle->gestureTemplateRepository->listBindings(values, error))
    {
        if (error.empty()) error = "Gesture persistence is unavailable.";
        std::memcpy(outList->error, error.data(), (std::min)(error.size(), sizeof(outList->error)-1));
        return kRyoikiStatusFailure;
    }
    outList->count = static_cast<std::uint32_t>((std::min)(values.size(),
        static_cast<std::size_t>(kRyoikiMaxGestureDefinitions)));
    for (std::size_t index = 0; index < outList->count; ++index)
    {
        auto& destination = outList->items[index]; const auto& source = values[index];
        destination.definition_id = source.definitionId;
        destination.enabled = source.enabled ? 1U : 0U;
        std::memcpy(destination.action_type, source.actionType.data(),
            (std::min)(source.actionType.size(), sizeof(destination.action_type)-1));
        std::memcpy(destination.action_parameter, source.actionParameter.data(),
            (std::min)(source.actionParameter.size(), sizeof(destination.action_parameter)-1));
    }
    return kRyoikiStatusSuccess;
}

RYOIKI_EXPORT std::int32_t ryoiki_upsert_gesture_binding(
    RyoikiHandle* handle, const std::uint32_t definitionId, const char* actionType,
    const char* actionParameter, const std::uint32_t enabled)
{
    if (!handle || definitionId == 0 || !actionType || !actionParameter) return kRyoikiStatusFailure;
    const auto typeLength = std::strlen(actionType);
    const auto parameterLength = std::strlen(actionParameter);
    if (typeLength > 31 || parameterLength > 255) return kRyoikiStatusFailure;
    const std::string type{actionType, typeLength};
    if (type != "app.launch" && type != "keyboard.typeText" && type != "keyboard.hotkey"
        && type != "handoff.grab" && type != "handoff.release")
        return kRyoikiStatusFailure;
    const std::string parameter{actionParameter, parameterLength};
    std::lock_guard lock{handle->gestureRepositoryMutex}; std::string error;
    return handle->gesturePersistenceAvailable
        && handle->gestureTemplateRepository->upsertBinding(
            {definitionId, enabled != 0, type, parameter}, error)
        ? kRyoikiStatusSuccess : kRyoikiStatusFailure;
}

RYOIKI_EXPORT std::int32_t ryoiki_delete_gesture_binding(
    RyoikiHandle* handle, const std::uint32_t definitionId)
{
    if (!handle || definitionId == 0) return kRyoikiStatusFailure;
    std::lock_guard lock{handle->gestureRepositoryMutex}; std::string error;
    return handle->gesturePersistenceAvailable
        && handle->gestureTemplateRepository->removeBinding(definitionId, error)
        ? kRyoikiStatusSuccess : kRyoikiStatusFailure;
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

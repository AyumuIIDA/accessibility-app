#include "ryoiki_native.h"

#include "Features/Cad/cad_interaction_endpoint.h"
#include "Rendering/cad_render_stage.h"
#include "Rendering/cad_view.h"
#include "Runtime/d3d11_device.h"

#include <Windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace
{
constexpr wchar_t kCadWindowClassName[] = L"RyoikiTenkaiCadViewport";

void copyString(const std::string& source, char* buffer, const std::int32_t length)
{
    if (buffer == nullptr || length <= 0) return;
    const auto count = (std::min)(source.size(), static_cast<std::size_t>(length - 1));
    std::memcpy(buffer, source.data(), count);
    buffer[count] = '\0';
}
}

struct RyoikiCadHandle
{
    HWND childHwnd{};
    std::shared_ptr<ryoiki::runtime::D3d11Device> device;
    ryoiki::rendering::CadRenderStage renderStage;
    ryoiki::rendering::CadView view{};
    POINT lastDragPoint{};
    bool dragging{false};
    std::mutex mutex;
    std::string error;
    std::shared_ptr<ryoiki::features::cad::CadInteractionEndpoint> interaction;

    void setError(std::string value)
    {
        std::lock_guard lock{mutex};
        error = std::move(value);
    }
};

namespace ryoiki::features::cad
{
CadInteractionEndpoint::CadInteractionEndpoint(
    RyoikiCadHandle& cadHandle)
    : cadHandle_(&cadHandle)
{
    latest_.abi_version = kRyoikiAbiVersion;
    latest_.struct_size = sizeof(RyoikiCadHandInteractionResult);
    latest_.zoom = 1.0F;
}

bool CadInteractionEndpoint::configure(
    const HandInteractionMode mode,
    const presentation::HandPresentationMode presentationMode,
    const float rotationSensitivity) noexcept
{
    if (!std::isfinite(rotationSensitivity)) return false;
    std::lock_guard lock{mutex_};
    if (cadHandle_ == nullptr) return false;
    if (mode_ != mode)
    {
        binding_.reset();
    }
    mode_ = mode;
    presentationMode_ = presentationMode;
    rotationSensitivity_ = rotationSensitivity;
    if (mode_ == HandInteractionMode::None)
    {
        rendering::CadView view{};
        {
            std::lock_guard cadLock{cadHandle_->mutex};
            view = cadHandle_->view;
        }
        latest_ = {};
        latest_.abi_version = kRyoikiAbiVersion;
        latest_.struct_size = sizeof(RyoikiCadHandInteractionResult);
        latest_.state = static_cast<std::int32_t>(
            HandInteractionState::Inactive);
        latest_.yaw_radians = view.yawRadians;
        latest_.pitch_radians = view.pitchRadians;
        latest_.zoom = view.zoom;
        latest_.pan_x = view.panX;
        latest_.pan_y = view.panY;
    }
    return true;
}

bool CadInteractionEndpoint::process(const CadHandInput& input) noexcept
{
    std::lock_guard endpointLock{mutex_};
    if (cadHandle_ == nullptr) return false;

    rendering::CadView currentView{};
    {
        std::lock_guard cadLock{cadHandle_->mutex};
        currentView = cadHandle_->view;
    }
    const auto output = binding_.update(
        input,
        currentView,
        mode_,
        presentationMode_,
        rotationSensitivity_,
        std::chrono::steady_clock::now());
    if (output.viewChanged)
    {
        {
            std::lock_guard cadLock{cadHandle_->mutex};
            cadHandle_->view = output.view;
        }
        cadHandle_->renderStage.setView(output.view);
    }

    latest_ = {};
    latest_.abi_version = kRyoikiAbiVersion;
    latest_.struct_size = sizeof(RyoikiCadHandInteractionResult);
    latest_.frame_id = input.frameId;
    latest_.state = static_cast<std::int32_t>(output.state);
    latest_.view_changed = output.viewChanged ? 1 : 0;
    latest_.yaw_radians = output.view.yawRadians;
    latest_.pitch_radians = output.view.pitchRadians;
    latest_.zoom = output.view.zoom;
    latest_.pan_x = output.view.panX;
    latest_.pan_y = output.view.panY;
    latest_.yaw_delta_degrees = output.yawDeltaDegrees;
    latest_.pitch_delta_degrees = output.pitchDeltaDegrees;
    return output.referenceCaptureRequested;
}

bool CadInteractionEndpoint::copyLatest(
    RyoikiCadHandInteractionResult& result) const noexcept
{
    std::lock_guard lock{mutex_};
    if (cadHandle_ == nullptr) return false;
    result = latest_;
    return true;
}

void CadInteractionEndpoint::detach() noexcept
{
    std::lock_guard lock{mutex_};
    cadHandle_ = nullptr;
    binding_.reset();
}
}

std::shared_ptr<ryoiki::features::cad::CadInteractionEndpoint>
ryoikiCadInteractionEndpoint(RyoikiCadHandle* handle) noexcept
{
    if (handle == nullptr) return {};
    return handle->interaction;
}

namespace
{
LRESULT CALLBACK CadWindowProc(const HWND hwnd, const UINT message, const WPARAM wparam, const LPARAM lparam)
{
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* handle = reinterpret_cast<RyoikiCadHandle*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message)
    {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{}; BeginPaint(hwnd, &paint);
        if (handle != nullptr) handle->renderStage.redraw();
        EndPaint(hwnd, &paint); return 0;
    }
    case WM_SIZE:
        if (handle != nullptr) handle->renderStage.resize(LOWORD(lparam), HIWORD(lparam));
        return 0;
    case WM_LBUTTONDOWN:
        if (handle != nullptr)
        {
            handle->dragging = true;
            handle->lastDragPoint = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            SetCapture(hwnd); return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (handle != nullptr && handle->dragging)
        {
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ryoiki::rendering::CadView view{};
            {
                std::lock_guard lock{handle->mutex};
                handle->view.yawRadians += static_cast<float>(point.x - handle->lastDragPoint.x) * 0.008F;
                handle->view.pitchRadians = std::clamp(
                    handle->view.pitchRadians + static_cast<float>(point.y - handle->lastDragPoint.y) * 0.008F,
                    -1.48353F, 1.48353F);
                view = handle->view;
            }
            handle->lastDragPoint = point;
            handle->renderStage.setView(view); return 0;
        }
        break;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (handle != nullptr)
        {
            handle->dragging = false;
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (handle != nullptr)
        {
            const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
            ryoiki::rendering::CadView view{};
            {
                std::lock_guard lock{handle->mutex};
                handle->view.zoom = std::clamp(handle->view.zoom * std::pow(1.14F, steps), 0.35F, 4.0F);
                view = handle->view;
            }
            handle->renderStage.setView(view); return 0;
        }
        break;
    case WM_LBUTTONDBLCLK:
        if (handle != nullptr)
        {
            {
                std::lock_guard lock{handle->mutex};
                handle->view = {};
            }
            handle->renderStage.setView({}); return 0;
        }
        break;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool registerCadWindowClass()
{
    static std::once_flag once;
    static bool registered = false;
    std::call_once(once, []
    {
        WNDCLASSW value{};
        value.lpfnWndProc = CadWindowProc;
        value.hInstance = GetModuleHandleW(nullptr);
        value.lpszClassName = kCadWindowClassName;
        value.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        value.style = CS_DBLCLKS;
        registered = RegisterClassW(&value) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    });
    return registered;
}
}

RYOIKI_EXPORT RyoikiCadHandle* ryoiki_cad_create(void* parent_hwnd)
{
    try
    {
        if (parent_hwnd == nullptr || !registerCadWindowClass()) return nullptr;
        auto handle = std::make_unique<RyoikiCadHandle>();
        handle->interaction =
            std::make_shared<ryoiki::features::cad::CadInteractionEndpoint>(*handle);
        std::string error;
        handle->device = ryoiki::runtime::D3d11Device::create(error);
        if (handle->device == nullptr) return nullptr;
        RECT bounds{}; GetClientRect(static_cast<HWND>(parent_hwnd), &bounds);
        const auto width = static_cast<std::uint32_t>((std::max)(1L, bounds.right - bounds.left));
        const auto height = static_cast<std::uint32_t>((std::max)(1L, bounds.bottom - bounds.top));
        handle->childHwnd = CreateWindowExW(0, kCadWindowClassName, L"",
            WS_CHILD | WS_VISIBLE, 0, 0, width, height, static_cast<HWND>(parent_hwnd),
            nullptr, GetModuleHandleW(nullptr), handle.get());
        if (handle->childHwnd == nullptr) return nullptr;
        if (!handle->renderStage.start(handle->device, handle->childHwnd, width, height, error))
        {
            handle->setError(error);
        }
        return handle.release();
    }
    catch (...) { return nullptr; }
}

RYOIKI_EXPORT std::int32_t ryoiki_cad_resize(
    RyoikiCadHandle* handle, const std::int32_t width, const std::int32_t height)
{
    if (handle == nullptr || handle->childHwnd == nullptr || width <= 0 || height <= 0) return 0;
    return MoveWindow(handle->childHwnd, 0, 0, width, height, TRUE) ? 1 : 0;
}

RYOIKI_EXPORT std::int32_t ryoiki_cad_set_view(
    RyoikiCadHandle* handle, const float yaw, const float pitch, const float zoom)
{
    if (handle == nullptr || !std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(zoom)) return 0;
    ryoiki::rendering::CadView view{};
    {
        std::lock_guard lock{handle->mutex};
        handle->view.yawRadians = yaw;
        handle->view.pitchRadians = pitch;
        handle->view.zoom = zoom;
        view = handle->view;
    }
    handle->renderStage.setView(view);
    return 1;
}

RYOIKI_EXPORT std::int32_t ryoiki_cad_set_view_ex(
    RyoikiCadHandle* handle,
    const float yaw,
    const float pitch,
    const float zoom,
    const float panX,
    const float panY)
{
    if (handle == nullptr || !std::isfinite(yaw) || !std::isfinite(pitch)
        || !std::isfinite(zoom) || !std::isfinite(panX) || !std::isfinite(panY))
    {
        return 0;
    }
    const ryoiki::rendering::CadView view{yaw, pitch, zoom, panX, panY};
    {
        std::lock_guard lock{handle->mutex};
        handle->view = view;
    }
    handle->renderStage.setView(view);
    return 1;
}

RYOIKI_EXPORT std::int32_t ryoiki_cad_get_view_ex(
    RyoikiCadHandle* handle,
    float* outYaw,
    float* outPitch,
    float* outZoom,
    float* outPanX,
    float* outPanY)
{
    if (handle == nullptr || outYaw == nullptr || outPitch == nullptr
        || outZoom == nullptr || outPanX == nullptr || outPanY == nullptr)
    {
        return 0;
    }
    std::lock_guard lock{handle->mutex};
    *outYaw = handle->view.yawRadians;
    *outPitch = handle->view.pitchRadians;
    *outZoom = handle->view.zoom;
    *outPanX = handle->view.panX;
    *outPanY = handle->view.panY;
    return 1;
}

RYOIKI_EXPORT std::int32_t ryoiki_cad_get_view(
    RyoikiCadHandle* handle, float* outYaw, float* outPitch, float* outZoom)
{
    if (handle == nullptr || outYaw == nullptr || outPitch == nullptr || outZoom == nullptr) return 0;
    std::lock_guard lock{handle->mutex};
    *outYaw = handle->view.yawRadians;
    *outPitch = handle->view.pitchRadians;
    *outZoom = handle->view.zoom;
    return 1;
}

RYOIKI_EXPORT void ryoiki_cad_reset_view(RyoikiCadHandle* handle)
{
    if (handle == nullptr) return;
    {
        std::lock_guard lock{handle->mutex};
        handle->view = {};
    }
    handle->renderStage.setView({});
}

RYOIKI_EXPORT void ryoiki_cad_destroy(RyoikiCadHandle* handle)
{
    if (handle == nullptr) return;
    if (handle->interaction != nullptr) handle->interaction->detach();
    handle->renderStage.stop();
    if (handle->childHwnd != nullptr) DestroyWindow(handle->childHwnd);
    delete handle;
}

RYOIKI_EXPORT std::int32_t ryoiki_cad_get_last_error(
    RyoikiCadHandle* handle, char* buffer, const std::int32_t length)
{
    if (handle == nullptr || buffer == nullptr || length <= 0) return 0;
    std::lock_guard lock{handle->mutex};
    copyString(handle->error, buffer, length);
    return handle->error.empty() ? 0 : 1;
}

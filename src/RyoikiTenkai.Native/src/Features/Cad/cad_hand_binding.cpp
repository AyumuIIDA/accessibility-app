#include "Features/Cad/cad_hand_binding.h"

#include <algorithm>
#include <cmath>

namespace ryoiki::features::cad
{
namespace
{
constexpr auto kMissingGrace = std::chrono::milliseconds{220};
constexpr float kMinimumSensitivity = 0.25F;
constexpr float kMaximumSensitivity = 2.0F;
constexpr float kRotationFitLimit = 0.35F;
constexpr float kDeadZoneRadians = 0.020F;

bool finiteRotation(const CadHandInput& input) noexcept
{
    return std::all_of(
        input.relativeRotation.begin(),
        input.relativeRotation.end(),
        [](const float value) { return std::isfinite(value); })
        && std::isfinite(input.rotationFitError);
}

std::array<float, 9> transformRotation(
    const std::array<float, 9>& rotation,
    const presentation::HandPresentationTransform& presentationTransform) noexcept
{
    const auto presented = presentationTransform.transformRotation(rotation);
    // Camera world coordinates are Y-down while the CAD orbit is Y-up. This
    // base conversion is independent from horizontal presentation policy.
    constexpr std::array<float, 3> kCameraToCadSigns{1.0F, -1.0F, 1.0F};
    std::array<float, 9> transformed{};
    for (std::size_t column = 0; column < 3; ++column)
    {
        for (std::size_t row = 0; row < 3; ++row)
        {
            transformed[column * 3 + row] =
                presented[column * 3 + row]
                * kCameraToCadSigns[row] * kCameraToCadSigns[column];
        }
    }
    return transformed;
}

void rotationVector(
    const std::array<float, 9>& matrix,
    float& rotationX,
    float& rotationY) noexcept
{
    const float cosine = std::clamp(
        (matrix[0] + matrix[4] + matrix[8] - 1.0F) * 0.5F,
        -1.0F,
        1.0F);
    const float angle = std::acos(cosine);
    if (angle < 1.0e-4F)
    {
        rotationX = (matrix[5] - matrix[7]) * 0.5F;
        rotationY = (matrix[6] - matrix[2]) * 0.5F;
        return;
    }
    const float denominator = 2.0F * std::sin(angle);
    if (std::abs(denominator) <= 1.0e-6F)
    {
        rotationX = 0.0F;
        rotationY = 0.0F;
        return;
    }
    const float scale = angle / denominator;
    rotationX = (matrix[5] - matrix[7]) * scale;
    rotationY = (matrix[6] - matrix[2]) * scale;
}

HandInteractionState activeState(const HandInteractionMode mode) noexcept
{
    switch (mode)
    {
    case HandInteractionMode::Rotate: return HandInteractionState::Rotating;
    case HandInteractionMode::Pan: return HandInteractionState::Panning;
    case HandInteractionMode::Zoom: return HandInteractionState::Zooming;
    default: return HandInteractionState::Ready;
    }
}
}

CadHandBindingOutput CadHandBinding::update(
    const CadHandInput& input,
    const rendering::CadView& currentView,
    const HandInteractionMode mode,
    const presentation::HandPresentationMode presentationMode,
    const float rotationSensitivity,
    const std::chrono::steady_clock::time_point now) noexcept
{
    CadHandBindingOutput output{};
    output.view = currentView;
    const auto presentationTransform =
        presentation::HandPresentationTransform::fromMode(presentationMode);
    if (mode == HandInteractionMode::None)
    {
        mustRelease_ = false;
        end();
        output.state = input.handPresent
            ? HandInteractionState::Ready
            : HandInteractionState::Inactive;
        return output;
    }
    if (mustRelease_)
    {
        output.state = HandInteractionState::AwaitingRelease;
        return output;
    }

    const bool rotationValid = input.relativeRotationValid
        && finiteRotation(input)
        && input.rotationFitError <= kRotationFitLimit;
    const bool measurementValid = input.handPresent
        && input.trackingQuality >= 0.70F
        && input.screenPalmValid
        && (mode != HandInteractionMode::Rotate || rotationValid);
    const bool newFrame = input.frameId != lastFrameId_;
    if (newFrame)
    {
        lastFrameId_ = input.frameId;
        lastNewFrameAt_ = now;
    }
    const bool stale = lastNewFrameAt_ != std::chrono::steady_clock::time_point{}
        && now - lastNewFrameAt_ >= kMissingGrace;
    if (!measurementValid || stale)
    {
        if (missingSince_ == std::chrono::steady_clock::time_point{})
        {
            missingSince_ = now;
        }
        if (now - missingSince_ < kMissingGrace)
        {
            output.state = HandInteractionState::Suspended;
            return output;
        }
        end();
        mustRelease_ = true;
        output.state = HandInteractionState::AwaitingRelease;
        return output;
    }
    missingSince_ = {};
    if (!newFrame)
    {
        output.state = active_
            ? activeState(activeMode_)
            : HandInteractionState::Ready;
        return output;
    }

    if (!active_ || mode != activeMode_)
    {
        referenceView_ = currentView;
        referenceCenterX_ = input.screenCenterX;
        referenceCenterY_ = input.screenCenterY;
        referenceScreenScale_ = input.screenScale;
        capturedRotationSensitivity_ = std::clamp(
            rotationSensitivity, kMinimumSensitivity, kMaximumSensitivity);
        activeMode_ = mode;
        active_ = true;
        output.state = activeState(mode);
        return output;
    }

    if (mode == HandInteractionMode::Pan)
    {
        const float unitsPerScreen = 3.2F / std::max(currentView.zoom, 0.35F);
        output.view.panX = std::clamp(
            referenceView_.panX
                + presentationTransform.transformScreenDeltaX(
                    input.screenCenterX - referenceCenterX_) * unitsPerScreen,
            -4.0F,
            4.0F);
        output.view.panY = std::clamp(
            referenceView_.panY
                + (input.screenCenterY - referenceCenterY_) * unitsPerScreen,
            -4.0F,
            4.0F);
        output.viewChanged = true;
        output.state = HandInteractionState::Panning;
        return output;
    }
    if (mode == HandInteractionMode::Zoom)
    {
        const float ratio = input.screenScale
            / std::max(referenceScreenScale_, 1.0e-5F);
        output.view.zoom = std::clamp(
            referenceView_.zoom * ratio, 0.35F, 4.0F);
        output.viewChanged = true;
        output.state = HandInteractionState::Zooming;
        return output;
    }

    const auto transformed = transformRotation(
        input.relativeRotation, presentationTransform);
    float rotationX = 0.0F;
    float rotationY = 0.0F;
    rotationVector(transformed, rotationX, rotationY);
    float yawDelta = rotationY;
    float pitchDelta = -rotationX;
    if (std::abs(yawDelta) < kDeadZoneRadians) yawDelta = 0.0F;
    if (std::abs(pitchDelta) < kDeadZoneRadians) pitchDelta = 0.0F;
    yawDelta *= capturedRotationSensitivity_;
    pitchDelta *= capturedRotationSensitivity_;

    output.yawDeltaDegrees = yawDelta * 180.0F / 3.14159265358979323846F;
    output.pitchDeltaDegrees = pitchDelta * 180.0F / 3.14159265358979323846F;
    output.view.yawRadians = referenceView_.yawRadians + yawDelta;
    output.view.pitchRadians = std::clamp(
        referenceView_.pitchRadians + pitchDelta,
        -1.48353F,
        1.48353F);
    output.viewChanged = true;
    output.state = HandInteractionState::Rotating;
    return output;
}

void CadHandBinding::reset() noexcept
{
    *this = {};
}

void CadHandBinding::end() noexcept
{
    active_ = false;
    activeMode_ = HandInteractionMode::None;
    missingSince_ = {};
}
}

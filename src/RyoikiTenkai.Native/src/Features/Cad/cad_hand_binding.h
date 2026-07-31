#pragma once

#include "Presentation/hand_presentation_transform.h"
#include "Rendering/cad_view.h"

#include <array>
#include <chrono>
#include <cstdint>

namespace ryoiki::features::cad
{
enum class HandInteractionMode : std::int32_t
{
    None = 0,
    Rotate = 1,
    Pan = 2,
    Zoom = 3
};

enum class HandInteractionState : std::int32_t
{
    Inactive = 0,
    Ready = 1,
    Rotating = 2,
    Panning = 3,
    Zooming = 4,
    Suspended = 5,
    // Retained for ABI and managed-display compatibility. The binding no longer
    // emits it: holding the clutch is a continuous statement of intent, so a
    // tracking gap re-zeroes every reference instead of demanding a new press.
    AwaitingRelease = 6
};

struct CadHandInput
{
    std::uint64_t frameId{0};
    std::uint64_t captureTimestampUs{0};
    float trackingQuality{0.0F};
    float screenCenterX{0.0F};
    float screenCenterY{0.0F};
    float screenScale{0.0F};
    std::array<float, 9> relativeRotation{};
    float rotationFitError{0.0F};
    bool handPresent{false};
    bool screenPalmValid{false};
    bool relativeRotationValid{false};
};

struct CadHandBindingOutput
{
    rendering::CadView view{};
    HandInteractionState state{HandInteractionState::Inactive};
    float yawDeltaDegrees{0.0F};
    float pitchDeltaDegrees{0.0F};
    bool viewChanged{false};
    // Set on the frame the interaction (re)activates. The binding owns view and
    // screen-space references, but the palm rotation reference belongs to the
    // native estimator, so the runtime must capture a fresh one.
    bool referenceCaptureRequested{false};
};

class CadHandBinding final
{
public:
    [[nodiscard]] CadHandBindingOutput update(
        const CadHandInput& input,
        const rendering::CadView& currentView,
        HandInteractionMode mode,
        presentation::HandPresentationMode presentationMode,
        float rotationSensitivity,
        std::chrono::steady_clock::time_point now) noexcept;

    void reset() noexcept;

private:
    void end() noexcept;

    rendering::CadView referenceView_{};
    float referenceCenterX_{0.0F};
    float referenceCenterY_{0.0F};
    float referenceScreenScale_{1.0F};
    float capturedRotationSensitivity_{1.0F};
    std::uint64_t lastFrameId_{0};
    std::chrono::steady_clock::time_point missingSince_{};
    std::chrono::steady_clock::time_point lastNewFrameAt_{};
    HandInteractionMode activeMode_{HandInteractionMode::None};
    bool active_{false};
};
}

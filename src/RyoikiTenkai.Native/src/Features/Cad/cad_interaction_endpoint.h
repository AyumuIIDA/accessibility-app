#pragma once

#include "Features/Cad/cad_hand_binding.h"
#include "ryoiki_native.h"

#include <memory>
#include <mutex>

struct RyoikiCadHandle;

namespace ryoiki::features::cad
{
class CadInteractionEndpoint final
{
public:
    explicit CadInteractionEndpoint(RyoikiCadHandle& cadHandle);

    CadInteractionEndpoint(const CadInteractionEndpoint&) = delete;
    CadInteractionEndpoint& operator=(const CadInteractionEndpoint&) = delete;

    [[nodiscard]] bool configure(
        HandInteractionMode mode,
        presentation::HandPresentationMode presentationMode,
        float rotationSensitivity) noexcept;
    // Returns true when the interaction (re)activated and the caller must
    // capture a fresh palm rotation reference before the next frame.
    [[nodiscard]] bool process(const CadHandInput& input) noexcept;
    [[nodiscard]] bool copyLatest(
        RyoikiCadHandInteractionResult& result) const noexcept;
    void detach() noexcept;

private:
    mutable std::mutex mutex_;
    RyoikiCadHandle* cadHandle_{nullptr};
    CadHandBinding binding_;
    HandInteractionMode mode_{HandInteractionMode::None};
    presentation::HandPresentationMode presentationMode_{
        presentation::HandPresentationMode::MirrorDirect};
    float rotationSensitivity_{1.0F};
    RyoikiCadHandInteractionResult latest_{};
};
}

[[nodiscard]] std::shared_ptr<ryoiki::features::cad::CadInteractionEndpoint>
ryoikiCadInteractionEndpoint(RyoikiCadHandle* handle) noexcept;

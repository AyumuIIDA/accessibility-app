#pragma once

#include "HandInput/Measurements/weighted_palm_rotation_tracker.h"

#include <array>
#include <cstdint>

namespace ryoiki::hand_input::measurements
{
enum class RotationObservationGateState : std::uint32_t
{
    Uninitialized = 0,
    Tracking = 1,
    Holding = 2,
    Reacquiring = 3
};

enum class RotationObservationRejection : std::uint32_t
{
    None = 0,
    Invalid = 1,
    AngularJump = 2,
    AngularSpeed = 3
};

struct RotationObservationGateResult
{
    PalmRotationEstimate observation{};
    RotationObservationGateState state{
        RotationObservationGateState::Uninitialized};
    RotationObservationRejection rejection{
        RotationObservationRejection::None};
    float rawDeltaDegrees{0.0F};
    std::uint32_t stableCandidateFrames{0};
    // Length of the current transient-outlier run. Non-zero while the gate is
    // holding its output against a physically implausible observation without
    // abandoning the tracked trajectory.
    std::uint32_t outlierFrames{0};
    // Length of the current dropout. Diagnostic only; a dropout always
    // abandons the trajectory.
    std::uint32_t invalidFrames{0};
    // Net divergence of the published trajectory from the raw absolute
    // measurement. Each recovery discards the motion across its unobservable
    // gap, so this value accumulates and is the diagnostic for reference drift.
    float rebaseOffsetDegrees{0.0F};
    // Divergence injected by this reacquisition only. Non-zero on the frame
    // that republishes after a recovery.
    float rebaseStepDegrees{0.0F};
    bool reacquired{false};
};

// Rejects discontinuous SO(3) observations.
//
// A physically implausible consecutive delta is first treated as a transient
// outlier: the output is held, but the tracked trajectory and its reference are
// kept, so an observation that returns to the previous branch resumes with no
// injected offset. Only a run of outliers longer than the budget, or an invalid
// measurement, abandons the trajectory and rebases a stable recovered one onto
// the last accepted output. Motion during that unobservable gap is intentionally
// discarded; motion after the first recovery candidate remains.
class RotationObservationGate final
{
public:
    [[nodiscard]] RotationObservationGateResult update(
        const PalmRotationEstimate& observation) noexcept;
    // Forces the next finite trajectory through continuity-preserving
    // reacquisition without discarding the last published orientation.
    void beginReacquisition() noexcept;
    void reset() noexcept;

private:
    [[nodiscard]] RotationObservationGateResult evaluate(
        const PalmRotationEstimate& observation) noexcept;

    std::array<float, 9> rebase_{
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F};
    std::array<float, 9> lastRaw_{
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F};
    std::array<float, 9> lastEffective_{
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F};
    std::array<float, 9> previousCandidate_{
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F};
    std::array<float, 9> publishedRebase_{
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F};
    RotationObservationGateState state_{
        RotationObservationGateState::Uninitialized};
    std::uint32_t stableCandidateFrames_{0};
    std::uint32_t outlierFrames_{0};
    std::uint32_t invalidFrames_{0};
};
}

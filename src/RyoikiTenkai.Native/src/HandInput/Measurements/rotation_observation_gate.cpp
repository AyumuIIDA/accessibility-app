#include "HandInput/Measurements/rotation_observation_gate.h"

#include <algorithm>
#include <cmath>

namespace ryoiki::hand_input::measurements
{
namespace
{
// A consecutive delta this large implies about 2,700 degrees per second at the
// current perception rate, which is beyond human forearm rotation and is
// instead produced by a landmark or palm-basis sign flip. The rule is expressed
// per inference step, not per camera timestamp, because capture timestamps
// occasionally collapse to microseconds and cannot bound an angular speed.
constexpr float kMaximumConsecutiveDeltaDegrees = 90.0F;
constexpr std::uint32_t kRequiredStableCandidateFrames = 3;
// How long the gate holds its output against implausible observations before
// accepting that the trajectory genuinely moved. Kept below the CAD binding's
// 220 ms tracking-loss grace so a held output never outlives the interaction.
constexpr std::uint32_t kOutlierBudgetFrames = 5;
constexpr float kRadiansToDegrees = 180.0F / 3.14159265358979323846F;

using Matrix3 = std::array<float, 9>;

Matrix3 identity() noexcept
{
    return {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F};
}

Matrix3 multiply(const Matrix3& left, const Matrix3& right) noexcept
{
    Matrix3 result{};
    for (std::size_t column = 0; column < 3; ++column)
    {
        for (std::size_t row = 0; row < 3; ++row)
        {
            for (std::size_t inner = 0; inner < 3; ++inner)
            {
                result[column * 3 + row] +=
                    left[inner * 3 + row] * right[column * 3 + inner];
            }
        }
    }
    return result;
}

Matrix3 transpose(const Matrix3& value) noexcept
{
    Matrix3 result{};
    for (std::size_t column = 0; column < 3; ++column)
    {
        for (std::size_t row = 0; row < 3; ++row)
        {
            result[column * 3 + row] = value[row * 3 + column];
        }
    }
    return result;
}

bool finiteRotation(const PalmRotationEstimate& observation) noexcept
{
    return observation.valid
        && std::all_of(
            observation.rotation.begin(),
            observation.rotation.end(),
            [](const float value) { return std::isfinite(value); });
}

float angularDifferenceDegrees(
    const Matrix3& left,
    const Matrix3& right) noexcept
{
    const auto difference = multiply(transpose(left), right);
    const float cosine = std::clamp(
        (difference[0] + difference[4] + difference[8] - 1.0F) * 0.5F,
        -1.0F,
        1.0F);
    return std::acos(cosine) * kRadiansToDegrees;
}

RotationObservationRejection validateDelta(
    const float deltaDegrees) noexcept
{
    if (deltaDegrees > kMaximumConsecutiveDeltaDegrees)
    {
        return RotationObservationRejection::AngularJump;
    }
    return RotationObservationRejection::None;
}

}

RotationObservationGateResult RotationObservationGate::update(
    const PalmRotationEstimate& observation) noexcept
{
    auto result = evaluate(observation);
    // Reported on every update so a diagnostic log can show accumulated drift
    // even while the gate is holding or reacquiring.
    result.rebaseOffsetDegrees = angularDifferenceDegrees(identity(), rebase_);
    return result;
}

RotationObservationGateResult RotationObservationGate::evaluate(
    const PalmRotationEstimate& observation) noexcept
{
    RotationObservationGateResult result{};
    result.state = state_;
    if (!finiteRotation(observation))
    {
        // Counted for diagnostics only. Tracking loss carries no continuity
        // information, so it always abandons the trajectory rather than
        // consuming the outlier budget.
        ++invalidFrames_;
        result.invalidFrames = invalidFrames_;
        result.rejection = RotationObservationRejection::Invalid;
        state_ = RotationObservationGateState::Holding;
        stableCandidateFrames_ = 0;
        outlierFrames_ = 0;
        result.state = state_;
        return result;
    }
    invalidFrames_ = 0;

    if (state_ == RotationObservationGateState::Uninitialized)
    {
        lastRaw_ = observation.rotation;
        lastEffective_ = observation.rotation;
        rebase_ = identity();
        publishedRebase_ = rebase_;
        outlierFrames_ = 0;
        state_ = RotationObservationGateState::Tracking;
        result.observation = observation;
        result.state = state_;
        return result;
    }

    if (state_ == RotationObservationGateState::Tracking)
    {
        result.rawDeltaDegrees =
            angularDifferenceDegrees(lastRaw_, observation.rotation);
        result.rejection = validateDelta(result.rawDeltaDegrees);
        if (result.rejection != RotationObservationRejection::None)
        {
            ++outlierFrames_;
            result.outlierFrames = outlierFrames_;
            if (outlierFrames_ <= kOutlierBudgetFrames)
            {
                // Transient outlier. Hold the output but keep the trajectory
                // anchor, so an observation that returns to the previous branch
                // resumes without rebasing and without injecting an offset.
                result.state = state_;
                return result;
            }
            // The implausible branch persisted, so the trajectory really moved.
            outlierFrames_ = 0;
            state_ = RotationObservationGateState::Holding;
            stableCandidateFrames_ = 0;
            result.state = state_;
            return result;
        }
        outlierFrames_ = 0;
        lastRaw_ = observation.rotation;
        lastEffective_ = multiply(rebase_, observation.rotation);
        result.observation = observation;
        result.observation.rotation = lastEffective_;
        result.state = state_;
        return result;
    }

    if (state_ == RotationObservationGateState::Holding)
    {
        previousCandidate_ = observation.rotation;
        rebase_ = multiply(lastEffective_, transpose(observation.rotation));
        stableCandidateFrames_ = 1;
        state_ = RotationObservationGateState::Reacquiring;
        result.state = state_;
        result.stableCandidateFrames = stableCandidateFrames_;
        return result;
    }

    result.rawDeltaDegrees =
        angularDifferenceDegrees(previousCandidate_, observation.rotation);
    result.rejection = validateDelta(result.rawDeltaDegrees);
    if (result.rejection != RotationObservationRejection::None)
    {
        // Treat the current finite sample as a new candidate origin. This can
        // recover from repeated outliers without briefly publishing a bad pose.
        previousCandidate_ = observation.rotation;
        rebase_ = multiply(lastEffective_, transpose(observation.rotation));
        stableCandidateFrames_ = 1;
        result.state = state_;
        result.stableCandidateFrames = stableCandidateFrames_;
        return result;
    }

    previousCandidate_ = observation.rotation;
    ++stableCandidateFrames_;
    result.stableCandidateFrames = stableCandidateFrames_;
    if (stableCandidateFrames_ < kRequiredStableCandidateFrames)
    {
        result.state = state_;
        return result;
    }

    lastRaw_ = observation.rotation;
    lastEffective_ = multiply(rebase_, observation.rotation);
    state_ = RotationObservationGateState::Tracking;
    result.observation = observation;
    result.observation.rotation = lastEffective_;
    result.state = state_;
    result.reacquired = true;
    // The motion across the unobservable gap is discarded here. Publish how
    // much this single recovery moved the output away from the raw trajectory.
    result.rebaseStepDegrees =
        angularDifferenceDegrees(publishedRebase_, rebase_);
    publishedRebase_ = rebase_;
    return result;
}

void RotationObservationGate::reset() noexcept
{
    rebase_ = identity();
    lastRaw_ = identity();
    lastEffective_ = identity();
    previousCandidate_ = identity();
    publishedRebase_ = identity();
    state_ = RotationObservationGateState::Uninitialized;
    stableCandidateFrames_ = 0;
    outlierFrames_ = 0;
    invalidFrames_ = 0;
}

void RotationObservationGate::beginReacquisition() noexcept
{
    if (state_ == RotationObservationGateState::Uninitialized)
    {
        return;
    }
    state_ = RotationObservationGateState::Holding;
    stableCandidateFrames_ = 0;
    outlierFrames_ = 0;
    invalidFrames_ = 0;
}
}

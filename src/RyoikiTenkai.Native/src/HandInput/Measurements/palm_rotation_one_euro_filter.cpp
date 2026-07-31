#include "HandInput/Measurements/palm_rotation_one_euro_filter.h"

#include <algorithm>
#include <cmath>

namespace ryoiki::hand_input::measurements
{
namespace
{
constexpr float kMinimumDt = 1.0F / 240.0F;
constexpr float kMaximumDt = 0.1F;
// One Euro trades noise for lag by opening its cutoff with angular speed. The
// previous 6 Hz / 0.05 pair never opened: at the measured 8 rad/s the cutoff
// rose only to 6.4 Hz, so the filter behaved as a fixed low-pass that was both
// too loose at rest and too tight in motion. These values are derived from the
// recorded speed distribution (about 0.7 rad/s of noise while the palm is
// quasi-stationary, about 8 rad/s during fast rotation) and improve both ends:
//
//   at rest   cutoff 2.7 Hz, noise gain 0.47 (was 0.62)
//   in motion cutoff  10 Hz, lag 7.3 degrees (was 11.4)
constexpr float kMinimumCutoffHz = 2.0F;
constexpr float kSpeedCoefficient = 1.0F;
// Smooths the speed estimate itself. The speed term uses a magnitude, so noise
// biases it upward and would otherwise reopen the cutoff it is meant to close.
constexpr float kDerivativeCutoffHz = 1.0F;
constexpr float kTwoPi = 6.28318530717958647692F;

using Matrix3 = std::array<float, 9>;
using Vector3 = std::array<float, 3>;

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

Vector3 logSo3(const Matrix3& rotation) noexcept
{
    const float cosine = std::clamp(
        (rotation[0] + rotation[4] + rotation[8] - 1.0F) * 0.5F, -1.0F, 1.0F);
    const float angle = std::acos(cosine);
    const Vector3 skew{
        rotation[5] - rotation[7],
        rotation[6] - rotation[2],
        rotation[1] - rotation[3]};
    if (angle < 1.0e-4F)
    {
        return {0.5F * skew[0], 0.5F * skew[1], 0.5F * skew[2]};
    }
    const float sine = std::sin(angle);
    if (std::abs(sine) <= 1.0e-5F) return {};
    const float scale = angle / (2.0F * sine);
    return {scale * skew[0], scale * skew[1], scale * skew[2]};
}

Matrix3 expSo3(const Vector3& value) noexcept
{
    const float squaredAngle =
        value[0] * value[0] + value[1] * value[1] + value[2] * value[2];
    const float angle = std::sqrt(squaredAngle);
    float a = 1.0F;
    float b = 0.5F;
    if (angle > 1.0e-5F)
    {
        a = std::sin(angle) / angle;
        b = (1.0F - std::cos(angle)) / squaredAngle;
    }
    const float x = value[0];
    const float y = value[1];
    const float z = value[2];
    return {
        1.0F - b * (y * y + z * z), a * z + b * x * y, -a * y + b * x * z,
        -a * z + b * x * y, 1.0F - b * (x * x + z * z), a * x + b * y * z,
        a * y + b * x * z, -a * x + b * y * z, 1.0F - b * (x * x + y * y)};
}

float length(const Vector3& value) noexcept
{
    return std::sqrt(
        value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
}

float smoothingFactor(const float cutoffHz, const float dt) noexcept
{
    const float timeConstant = 1.0F / (kTwoPi * cutoffHz);
    return 1.0F / (1.0F + timeConstant / dt);
}
}

PalmRotationEstimate PalmRotationOneEuroFilter::update(
    const PalmRotationEstimate& observation,
    const std::uint64_t timestampUs) noexcept
{
    if (!observation.valid || timestampUs == 0) return {};
    if (!initialized_)
    {
        orientation_ = observation.rotation;
        previousObservation_ = observation.rotation;
        previousTimestampUs_ = timestampUs;
        filteredAngularSpeed_ = 0.0F;
        initialized_ = true;
        return observation;
    }
    if (timestampUs <= previousTimestampUs_)
    {
        return {orientation_, observation.fitError, true};
    }
    const float dt = std::clamp(
        static_cast<float>(timestampUs - previousTimestampUs_) * 1.0e-6F,
        kMinimumDt,
        kMaximumDt);
    previousTimestampUs_ = timestampUs;
    const auto observationDelta = logSo3(
        multiply(transpose(previousObservation_), observation.rotation));
    previousObservation_ = observation.rotation;
    const float angularSpeed = length(observationDelta) / dt;
    const float derivativeAlpha = smoothingFactor(kDerivativeCutoffHz, dt);
    filteredAngularSpeed_ +=
        derivativeAlpha * (angularSpeed - filteredAngularSpeed_);
    const float cutoff =
        kMinimumCutoffHz + kSpeedCoefficient * filteredAngularSpeed_;
    const float orientationAlpha = smoothingFactor(cutoff, dt);
    const auto error = logSo3(
        multiply(transpose(orientation_), observation.rotation));
    orientation_ = multiply(
        orientation_,
        expSo3({
            orientationAlpha * error[0],
            orientationAlpha * error[1],
            orientationAlpha * error[2]}));
    return {orientation_, observation.fitError, true};
}

void PalmRotationOneEuroFilter::reset() noexcept
{
    orientation_ = {
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F};
    previousObservation_ = orientation_;
    filteredAngularSpeed_ = 0.0F;
    previousTimestampUs_ = 0;
    initialized_ = false;
}
}

#include "HandInput/Measurements/palm_rotation_eskf.h"

#include <algorithm>
#include <cmath>

namespace ryoiki::hand_input::measurements
{
namespace
{
constexpr float kMinimumDt = 1.0F / 240.0F;
constexpr float kMaximumDt = 0.1F;
constexpr float kOrientationProcessNoise = 0.025F;
constexpr float kAngularAccelerationNoise = 4.0F;
constexpr float kMinimumAbsoluteSigma = 0.012F;
constexpr float kAbsoluteFitScale = 0.45F;
constexpr float kMinimumVelocitySigma = 0.35F;
constexpr float kVelocityFitScale = 6.0F;
constexpr float kOrientationGateSquared = 16.27F; // chi-square, 3 DoF, 99.9%
constexpr float kVelocityGateSquared = 16.27F;

using Matrix3 = std::array<float, 9>;
using Matrix6 = std::array<float, 36>;
using Vector3 = std::array<float, 3>;

Matrix3 identity3() noexcept
{
    return {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F};
}

Matrix3 multiply3(const Matrix3& left, const Matrix3& right) noexcept
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

Matrix3 transpose3(const Matrix3& value) noexcept
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
    if (std::abs(sine) < 1.0e-5F)
    {
        const float x = std::sqrt(std::max(0.0F, (rotation[0] + 1.0F) * 0.5F));
        const float y = std::copysign(
            std::sqrt(std::max(0.0F, (rotation[4] + 1.0F) * 0.5F)), skew[1]);
        const float z = std::copysign(
            std::sqrt(std::max(0.0F, (rotation[8] + 1.0F) * 0.5F)), skew[2]);
        return {angle * x, angle * y, angle * z};
    }
    const float scale = angle / (2.0F * sine);
    return {scale * skew[0], scale * skew[1], scale * skew[2]};
}

Matrix3 expSo3(const Vector3& value) noexcept
{
    const float angleSquared =
        value[0] * value[0] + value[1] * value[1] + value[2] * value[2];
    const float angle = std::sqrt(angleSquared);
    float a = 1.0F;
    float b = 0.5F;
    if (angle > 1.0e-5F)
    {
        a = std::sin(angle) / angle;
        b = (1.0F - std::cos(angle)) / angleSquared;
    }
    const float x = value[0];
    const float y = value[1];
    const float z = value[2];
    return {
        1.0F - b * (y * y + z * z), a * z + b * x * y, -a * y + b * x * z,
        -a * z + b * x * y, 1.0F - b * (x * x + z * z), a * x + b * y * z,
        a * y + b * x * z, -a * x + b * y * z, 1.0F - b * (x * x + y * y)};
}

bool inverse3(const Matrix3& value, Matrix3& inverse) noexcept
{
    const float determinant =
        value[0] * (value[4] * value[8] - value[7] * value[5])
        - value[3] * (value[1] * value[8] - value[7] * value[2])
        + value[6] * (value[1] * value[5] - value[4] * value[2]);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-12F) return false;
    const float scale = 1.0F / determinant;
    inverse = {
        (value[4] * value[8] - value[7] * value[5]) * scale,
        (value[7] * value[2] - value[1] * value[8]) * scale,
        (value[1] * value[5] - value[4] * value[2]) * scale,
        (value[6] * value[5] - value[3] * value[8]) * scale,
        (value[0] * value[8] - value[6] * value[2]) * scale,
        (value[3] * value[2] - value[0] * value[5]) * scale,
        (value[3] * value[7] - value[6] * value[4]) * scale,
        (value[6] * value[1] - value[0] * value[7]) * scale,
        (value[0] * value[4] - value[3] * value[1]) * scale};
    return true;
}

Vector3 multiply3Vector(const Matrix3& matrix, const Vector3& value) noexcept
{
    return {
        matrix[0] * value[0] + matrix[3] * value[1] + matrix[6] * value[2],
        matrix[1] * value[0] + matrix[4] * value[1] + matrix[7] * value[2],
        matrix[2] * value[0] + matrix[5] * value[1] + matrix[8] * value[2]};
}

float dot(const Vector3& left, const Vector3& right) noexcept
{
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

void initializeCovariance(Matrix6& covariance) noexcept
{
    covariance = {};
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        covariance[axis * 6 + axis] = 0.02F * 0.02F;
        covariance[(axis + 3) * 6 + axis + 3] = 1.0F;
    }
}

void predictCovariance(Matrix6& covariance, const float dt) noexcept
{
    Matrix6 transition{};
    for (std::size_t index = 0; index < 6; ++index)
    {
        transition[index * 6 + index] = 1.0F;
    }
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        transition[(axis + 3) * 6 + axis] = dt;
    }
    Matrix6 intermediate{};
    Matrix6 predicted{};
    for (std::size_t column = 0; column < 6; ++column)
    {
        for (std::size_t row = 0; row < 6; ++row)
        {
            for (std::size_t inner = 0; inner < 6; ++inner)
            {
                intermediate[column * 6 + row] +=
                    transition[inner * 6 + row] * covariance[column * 6 + inner];
            }
        }
    }
    for (std::size_t column = 0; column < 6; ++column)
    {
        for (std::size_t row = 0; row < 6; ++row)
        {
            for (std::size_t inner = 0; inner < 6; ++inner)
            {
                predicted[column * 6 + row] +=
                    intermediate[inner * 6 + row] * transition[inner * 6 + column];
            }
        }
    }
    const float orientationVariance =
        kOrientationProcessNoise * kOrientationProcessNoise * dt;
    const float velocityVariance =
        kAngularAccelerationNoise * kAngularAccelerationNoise * dt;
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        predicted[axis * 6 + axis] += orientationVariance;
        predicted[(axis + 3) * 6 + axis + 3] += velocityVariance;
    }
    covariance = predicted;
}

bool updateBlock(
    Matrix6& covariance,
    const std::size_t stateOffset,
    const Vector3& innovation,
    const float variance,
    const float gateSquared,
    std::array<float, 6>& correction) noexcept
{
    Matrix3 innovationCovariance{};
    for (std::size_t column = 0; column < 3; ++column)
    {
        for (std::size_t row = 0; row < 3; ++row)
        {
            innovationCovariance[column * 3 + row] =
                covariance[(column + stateOffset) * 6 + row + stateOffset];
        }
        innovationCovariance[column * 3 + column] += variance;
    }
    Matrix3 inverseInnovation{};
    if (!inverse3(innovationCovariance, inverseInnovation)) return false;
    const auto normalizedInnovation =
        multiply3Vector(inverseInnovation, innovation);
    if (dot(innovation, normalizedInnovation) > gateSquared) return false;

    float gain[6][3]{};
    for (std::size_t row = 0; row < 6; ++row)
    {
        for (std::size_t column = 0; column < 3; ++column)
        {
            for (std::size_t inner = 0; inner < 3; ++inner)
            {
                gain[row][column] +=
                    covariance[(inner + stateOffset) * 6 + row]
                    * inverseInnovation[column * 3 + inner];
            }
            correction[row] += gain[row][column] * innovation[column];
        }
    }

    Matrix6 updated = covariance;
    for (std::size_t column = 0; column < 6; ++column)
    {
        for (std::size_t row = 0; row < 6; ++row)
        {
            float reduction = 0.0F;
            for (std::size_t inner = 0; inner < 3; ++inner)
            {
                reduction += gain[row][inner]
                    * covariance[column * 6 + inner + stateOffset];
            }
            updated[column * 6 + row] -= reduction;
        }
    }
    for (std::size_t column = 0; column < 6; ++column)
    {
        for (std::size_t row = 0; row < 6; ++row)
        {
            covariance[column * 6 + row] =
                0.5F * (updated[column * 6 + row] + updated[row * 6 + column]);
        }
    }
    return true;
}
}

PalmRotationEstimate PalmRotationEskf::update(
    const PalmRotationEstimate& absoluteObservation,
    const PalmRotationEstimate& incrementalObservation,
    const std::uint64_t timestampUs) noexcept
{
    PalmRotationEstimate result{};
    if (!absoluteObservation.valid || timestampUs == 0)
    {
        return result;
    }
    if (!initialized_)
    {
        orientation_ = absoluteObservation.rotation;
        angularVelocity_ = {};
        initializeCovariance(covariance_);
        previousTimestampUs_ = timestampUs;
        initialized_ = true;
        result = absoluteObservation;
        return result;
    }
    if (timestampUs <= previousTimestampUs_)
    {
        result.rotation = orientation_;
        result.fitError = absoluteObservation.fitError;
        result.valid = true;
        return result;
    }

    const float dt = std::clamp(
        static_cast<float>(timestampUs - previousTimestampUs_) * 1.0e-6F,
        kMinimumDt,
        kMaximumDt);
    previousTimestampUs_ = timestampUs;
    Vector3 observedVelocity{};
    const bool hasVelocityObservation = incrementalObservation.valid;
    if (hasVelocityObservation)
    {
        // The incremental fit maps previous camera/world coordinates to current
        // coordinates. Convert that spatial delta into the nominal orientation's
        // local tangent frame before comparing it with body angular velocity.
        const auto bodyDelta = multiply3(
            multiply3(transpose3(orientation_), incrementalObservation.rotation),
            orientation_);
        const auto incrementalVector = logSo3(bodyDelta);
        observedVelocity = {
            incrementalVector[0] / dt,
            incrementalVector[1] / dt,
            incrementalVector[2] / dt};
    }
    orientation_ = multiply3(
        orientation_,
        expSo3({
            angularVelocity_[0] * dt,
            angularVelocity_[1] * dt,
            angularVelocity_[2] * dt}));
    predictCovariance(covariance_, dt);

    if (hasVelocityObservation)
    {
        const Vector3 innovation{
            observedVelocity[0] - angularVelocity_[0],
            observedVelocity[1] - angularVelocity_[1],
            observedVelocity[2] - angularVelocity_[2]};
        const float sigma = kMinimumVelocitySigma
            + kVelocityFitScale * incrementalObservation.fitError / dt;
        std::array<float, 6> correction{};
        if (updateBlock(
                covariance_, 3, innovation, sigma * sigma,
                kVelocityGateSquared, correction))
        {
            orientation_ = multiply3(
                orientation_, expSo3({correction[0], correction[1], correction[2]}));
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                angularVelocity_[axis] += correction[axis + 3];
            }
        }
    }

    const auto absoluteError = logSo3(
        multiply3(transpose3(orientation_), absoluteObservation.rotation));
    const float absoluteSigma =
        kMinimumAbsoluteSigma + kAbsoluteFitScale * absoluteObservation.fitError;
    std::array<float, 6> correction{};
    if (updateBlock(
            covariance_, 0, absoluteError, absoluteSigma * absoluteSigma,
            kOrientationGateSquared, correction))
    {
        orientation_ = multiply3(
            orientation_, expSo3({correction[0], correction[1], correction[2]}));
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            angularVelocity_[axis] += correction[axis + 3];
        }
    }

    result.rotation = orientation_;
    result.fitError = absoluteObservation.fitError;
    result.valid = true;
    return result;
}

void PalmRotationEskf::reset() noexcept
{
    orientation_ = identity3();
    angularVelocity_ = {};
    covariance_ = {};
    previousTimestampUs_ = 0;
    initialized_ = false;
}
}

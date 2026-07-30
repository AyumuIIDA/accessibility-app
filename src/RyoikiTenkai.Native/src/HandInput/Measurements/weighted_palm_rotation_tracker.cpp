#include "HandInput/Measurements/weighted_palm_rotation_tracker.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ryoiki::hand_input::measurements
{
namespace
{
using Point = WeightedPalmRotationTracker::Point;
constexpr std::array<std::size_t, 6> kIndices{0, 5, 9, 13, 17, 1};
constexpr std::array<float, 6> kWeights{1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 0.25F};
constexpr float kWeightSum = 5.25F;
constexpr std::size_t kRobustIterations = 3;

bool finite(const Point& point) noexcept
{
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

Point subtract(const Point left, const Point right) noexcept
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

float squaredLength(const Point value) noexcept
{
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

bool normalizePoints(
    const hand_perception::HandLandmarkResult& hand,
    std::array<Point, 6>& points) noexcept
{
    if (!hand.detected)
    {
        return false;
    }
    Point center{};
    for (std::size_t index = 0; index < kIndices.size(); ++index)
    {
        const auto& landmark = hand.worldLandmarks[kIndices[index]];
        points[index] = {landmark.x, landmark.y, landmark.z};
        if (!finite(points[index]))
        {
            return false;
        }
        center.x += points[index].x * kWeights[index];
        center.y += points[index].y * kWeights[index];
        center.z += points[index].z * kWeights[index];
    }
    center.x /= kWeightSum;
    center.y /= kWeightSum;
    center.z /= kWeightSum;

    float weightedSquaredRadius = 0.0F;
    for (std::size_t index = 0; index < points.size(); ++index)
    {
        points[index] = subtract(points[index], center);
        weightedSquaredRadius += kWeights[index] * squaredLength(points[index]);
    }
    const float scale = std::sqrt(weightedSquaredRadius / kWeightSum);
    if (!std::isfinite(scale) || scale <= 1.0e-5F)
    {
        return false;
    }
    const float inverseScale = 1.0F / scale;
    for (auto& point : points)
    {
        point.x *= inverseScale;
        point.y *= inverseScale;
        point.z *= inverseScale;
    }
    return true;
}

std::array<float, 9> quaternionToMatrix(
    const std::array<float, 4>& quaternion) noexcept
{
    const float w = quaternion[0];
    const float x = quaternion[1];
    const float y = quaternion[2];
    const float z = quaternion[3];
    return {
        1.0F - 2.0F * (y * y + z * z),
        2.0F * (x * y + w * z),
        2.0F * (x * z - w * y),
        2.0F * (x * y - w * z),
        1.0F - 2.0F * (x * x + z * z),
        2.0F * (y * z + w * x),
        2.0F * (x * z + w * y),
        2.0F * (y * z - w * x),
        1.0F - 2.0F * (x * x + y * y)};
}

Point transform(const std::array<float, 9>& matrix, const Point point) noexcept
{
    return {
        matrix[0] * point.x + matrix[3] * point.y + matrix[6] * point.z,
        matrix[1] * point.x + matrix[4] * point.y + matrix[7] * point.z,
        matrix[2] * point.x + matrix[5] * point.y + matrix[8] * point.z};
}

bool hasObservableGeometry(const std::array<Point, 6>& points) noexcept
{
    float maximumAreaSquared = 0.0F;
    for (std::size_t first = 1; first < points.size(); ++first)
    {
        const auto a = subtract(points[first], points[0]);
        for (std::size_t second = first + 1; second < points.size(); ++second)
        {
            const auto b = subtract(points[second], points[0]);
            const Point cross{
                a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x};
            maximumAreaSquared = std::max(maximumAreaSquared, squaredLength(cross));
        }
    }
    return std::isfinite(maximumAreaSquared) && maximumAreaSquared >= 1.0e-3F;
}

bool solveRotation(
    const std::array<Point, 6>& reference,
    const std::array<Point, 6>& current,
    const std::array<float, 6>& weights,
    std::array<float, 9>& rotation) noexcept
{
    float h[3][3]{};
    for (std::size_t index = 0; index < current.size(); ++index)
    {
        const float referenceValues[]{reference[index].x, reference[index].y, reference[index].z};
        const float currentValues[]{current[index].x, current[index].y, current[index].z};
        for (std::size_t row = 0; row < 3; ++row)
        {
            for (std::size_t column = 0; column < 3; ++column)
            {
                h[row][column] +=
                    weights[index] * referenceValues[row] * currentValues[column];
            }
        }
    }

    const float trace = h[0][0] + h[1][1] + h[2][2];
    float matrix[4][4]{
        {trace, h[1][2] - h[2][1], h[2][0] - h[0][2], h[0][1] - h[1][0]},
        {h[1][2] - h[2][1], h[0][0] - h[1][1] - h[2][2], h[0][1] + h[1][0], h[0][2] + h[2][0]},
        {h[2][0] - h[0][2], h[0][1] + h[1][0], -h[0][0] + h[1][1] - h[2][2], h[1][2] + h[2][1]},
        {h[0][1] - h[1][0], h[0][2] + h[2][0], h[1][2] + h[2][1], -h[0][0] - h[1][1] + h[2][2]}};
    float eigenvectors[4][4]{
        {1.0F, 0.0F, 0.0F, 0.0F},
        {0.0F, 1.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, 1.0F, 0.0F},
        {0.0F, 0.0F, 0.0F, 1.0F}};

    // A fixed-size Jacobi eigensolver is deterministic for this symmetric
    // problem and does not have power iteration's weak behavior near 180°.
    for (std::size_t sweep = 0; sweep < 12; ++sweep)
    {
        std::size_t p = 0;
        std::size_t q = 1;
        float largest = 0.0F;
        for (std::size_t row = 0; row < 4; ++row)
        {
            for (std::size_t column = row + 1; column < 4; ++column)
            {
                const float magnitude = std::abs(matrix[row][column]);
                if (magnitude > largest)
                {
                    largest = magnitude;
                    p = row;
                    q = column;
                }
            }
        }
        if (largest <= 1.0e-7F) break;
        const float angle = 0.5F * std::atan2(
            2.0F * matrix[p][q], matrix[q][q] - matrix[p][p]);
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        for (std::size_t index = 0; index < 4; ++index)
        {
            const float mip = matrix[index][p];
            const float miq = matrix[index][q];
            matrix[index][p] = cosine * mip - sine * miq;
            matrix[index][q] = sine * mip + cosine * miq;
        }
        for (std::size_t index = 0; index < 4; ++index)
        {
            const float mpi = matrix[p][index];
            const float mqi = matrix[q][index];
            matrix[p][index] = cosine * mpi - sine * mqi;
            matrix[q][index] = sine * mpi + cosine * mqi;
            const float vip = eigenvectors[index][p];
            const float viq = eigenvectors[index][q];
            eigenvectors[index][p] = cosine * vip - sine * viq;
            eigenvectors[index][q] = sine * vip + cosine * viq;
        }
    }

    std::size_t dominant = 0;
    for (std::size_t index = 1; index < 4; ++index)
    {
        if (matrix[index][index] > matrix[dominant][dominant]) dominant = index;
    }
    std::array<float, 4> quaternion{};
    float lengthSquared = 0.0F;
    for (std::size_t index = 0; index < 4; ++index)
    {
        quaternion[index] = eigenvectors[index][dominant];
        lengthSquared += quaternion[index] * quaternion[index];
    }
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12F) return false;
    const float inverseLength = 1.0F / std::sqrt(lengthSquared);
    for (auto& value : quaternion) value *= inverseLength;
    rotation = quaternionToMatrix(quaternion);
    return true;
}
}

bool WeightedPalmRotationTracker::captureReference(
    const hand_perception::HandLandmarkResult& hand) noexcept
{
    hasReference_ = normalizePoints(hand, reference_);
    return hasReference_;
}

PalmRotationEstimate WeightedPalmRotationTracker::estimate(
    const hand_perception::HandLandmarkResult& hand) const noexcept
{
    PalmRotationEstimate result{};
    if (!hasReference_)
    {
        return result;
    }
    std::array<Point, 6> current{};
    if (!normalizePoints(hand, current))
    {
        return result;
    }
    if (!hasObservableGeometry(reference_) || !hasObservableGeometry(current)) return result;

    std::array<float, 6> robustWeights = kWeights;
    for (std::size_t iteration = 0; iteration < kRobustIterations; ++iteration)
    {
        if (!solveRotation(reference_, current, robustWeights, result.rotation)) return {};
        std::array<float, 6> residuals{};
        for (std::size_t index = 0; index < current.size(); ++index)
        {
            const auto aligned = transform(result.rotation, reference_[index]);
            residuals[index] = std::sqrt(squaredLength(subtract(current[index], aligned)));
        }
        auto ordered = residuals;
        std::sort(ordered.begin(), ordered.end());
        const float median = 0.5F * (ordered[2] + ordered[3]);
        const float huberCutoff = std::max(1.5F * median, 1.0e-3F);
        for (std::size_t index = 0; index < robustWeights.size(); ++index)
        {
            const float robustFactor = residuals[index] <= huberCutoff
                ? 1.0F
                : huberCutoff / residuals[index];
            robustWeights[index] = kWeights[index] * robustFactor;
        }
    }

    // Report ordinary normalized RMS after the robust fit. Keeping the metric
    // un-clipped makes a bad landmark visible to policy and diagnostics.
    float weightedSquaredError = 0.0F;
    for (std::size_t index = 0; index < current.size(); ++index)
    {
        const auto aligned = transform(result.rotation, reference_[index]);
        weightedSquaredError +=
            kWeights[index] * squaredLength(subtract(current[index], aligned));
    }
    result.fitError = std::sqrt(weightedSquaredError / kWeightSum);
    result.valid = std::isfinite(result.fitError);
    return result;
}

void WeightedPalmRotationTracker::reset() noexcept
{
    reference_ = {};
    hasReference_ = false;
}
}

#include "HandInput/Measurements/palm_basis_rotation_tracker.h"

#include <algorithm>
#include <cmath>

namespace ryoiki::hand_input::measurements
{
namespace
{
std::array<float, 9> basisMatrix(const HandMeasurements& measurements) noexcept
{
    const std::array axes{
        measurements.palmXAxis,
        measurements.palmYAxis,
        measurements.palmZAxis};
    std::array<float, 9> result{};
    for (std::size_t column = 0; column < axes.size(); ++column)
    {
        result[column * 3] = axes[column].x;
        result[column * 3 + 1] = axes[column].y;
        result[column * 3 + 2] = axes[column].z;
    }
    return result;
}

bool finiteBasis(const std::array<float, 9>& basis) noexcept
{
    return std::all_of(
        basis.begin(), basis.end(), [](const float value) { return std::isfinite(value); });
}

std::array<float, 9> multiplyByTranspose(
    const std::array<float, 9>& left,
    const std::array<float, 9>& right) noexcept
{
    std::array<float, 9> result{};
    for (std::size_t column = 0; column < 3; ++column)
    {
        for (std::size_t row = 0; row < 3; ++row)
        {
            for (std::size_t inner = 0; inner < 3; ++inner)
            {
                result[column * 3 + row] +=
                    left[inner * 3 + row] * right[inner * 3 + column];
            }
        }
    }
    return result;
}
}

bool PalmBasisRotationTracker::captureReference(
    const HandMeasurements& measurements) noexcept
{
    hasReference_ = measurements.present
        && measurements.quality == HandMeasurementQuality::Valid;
    if (!hasReference_)
    {
        referenceBasis_ = {};
        return false;
    }
    referenceBasis_ = basisMatrix(measurements);
    hasReference_ = finiteBasis(referenceBasis_);
    return hasReference_;
}

PalmRotationEstimate PalmBasisRotationTracker::estimate(
    const HandMeasurements& measurements) const noexcept
{
    PalmRotationEstimate result{};
    if (!hasReference_
        || !measurements.present
        || measurements.quality != HandMeasurementQuality::Valid)
    {
        return result;
    }
    const auto currentBasis = basisMatrix(measurements);
    if (!finiteBasis(currentBasis)) return result;
    // Basis matrices contain local axes as columns. B_current * B_reference^T
    // maps a vector expressed in the reference camera/world frame to current.
    result.rotation = multiplyByTranspose(currentBasis, referenceBasis_);
    result.valid = true;
    return result;
}

void PalmBasisRotationTracker::reset() noexcept
{
    referenceBasis_ = {};
    hasReference_ = false;
}
}

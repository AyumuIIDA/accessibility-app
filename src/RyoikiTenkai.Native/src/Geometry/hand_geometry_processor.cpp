#include "Geometry/hand_geometry_processor.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace ryoiki::geometry
{
namespace
{
constexpr float kInverseByteRange = 1.0F / 255.0F;

bool hasShape(
    const buffers::FloatTensorBuffer& tensor,
    const std::array<std::int64_t, 4>& expected)
{
    return tensor.shape() == expected;
}

void packBgraToRgbNhwc(const cv::Mat& bgra, buffers::FloatTensorBuffer& tensor)
{
    float* destination = tensor.data();
    for (int y = 0; y < bgra.rows; ++y)
    {
        const auto* source = bgra.ptr<cv::Vec4b>(y);
        for (int x = 0; x < bgra.cols; ++x)
        {
            *destination++ = source[x][2] * kInverseByteRange;
            *destination++ = source[x][1] * kInverseByteRange;
            *destination++ = source[x][0] * kInverseByteRange;
        }
    }
}

AffineTransform toTransform(const cv::Mat& matrix)
{
    AffineTransform transform;
    for (int row = 0; row < 2; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            transform.values[static_cast<std::size_t>(row * 3 + column)] =
                static_cast<float>(matrix.at<double>(row, column));
        }
    }
    return transform;
}

cv::Matx23f toCvTransform(const AffineTransform& transform)
{
    const auto& value = transform.values;
    return {
        value[0], value[1], value[2],
        value[3], value[4], value[5]};
}
}

struct HandGeometryProcessor::Impl
{
    cv::Mat palmBgra{cv::Mat::zeros(kPalmInputSize, kPalmInputSize, CV_8UC4)};
    cv::Mat handBgra{cv::Mat::zeros(kHandInputSize, kHandInputSize, CV_8UC4)};
};

HandGeometryProcessor::HandGeometryProcessor() : impl_{std::make_unique<Impl>()}
{
}

HandGeometryProcessor::~HandGeometryProcessor() = default;

buffers::MemoryLocation HandGeometryProcessor::inputMemoryLocation() const noexcept
{
    return buffers::MemoryLocation::Cpu;
}

buffers::MemoryLocation HandGeometryProcessor::outputMemoryLocation() const noexcept
{
    return buffers::MemoryLocation::Cpu;
}

bool HandGeometryProcessor::preprocessPalm(
    const buffers::FrameBuffer& frame,
    buffers::FloatTensorBuffer& tensor,
    PalmPreprocessResult& result)
{
    constexpr std::array<std::int64_t, 4> kShape{1, kPalmInputSize, kPalmInputSize, 3};
    if (!hasShape(tensor, kShape) || frame.pixels().empty())
    {
        return false;
    }

    const cv::Mat source(
        static_cast<int>(frame.height()),
        static_cast<int>(frame.width()),
        CV_8UC4,
        const_cast<std::uint8_t*>(frame.pixels().data()),
        frame.stride());

    const auto uprightWidth = frame.uprightWidth();
    const auto uprightHeight = frame.uprightHeight();
    const float ratio = (std::min)(
        kPalmInputSize / static_cast<float>(uprightWidth),
        kPalmInputSize / static_cast<float>(uprightHeight));
    const int resizedWidth = (std::max)(1, static_cast<int>(uprightWidth * ratio));
    const int resizedHeight = (std::max)(1, static_cast<int>(uprightHeight * ratio));
    const int left = (kPalmInputSize - resizedWidth) / 2;
    const int top = (kPalmInputSize - resizedHeight) / 2;

    result.transform.scaleX = resizedWidth / static_cast<float>(uprightWidth);
    result.transform.scaleY = resizedHeight / static_cast<float>(uprightHeight);
    result.transform.padLeft = static_cast<float>(left);
    result.transform.padTop = static_cast<float>(top);

    const AffineTransform tensorToUpright{{
        1.0F / result.transform.scaleX,
        0.0F,
        (0.5F - result.transform.padLeft) / result.transform.scaleX - 0.5F,
        0.0F,
        1.0F / result.transform.scaleY,
        (0.5F - result.transform.padTop) / result.transform.scaleY - 0.5F}};
    AffineTransform uprightToStorageEdges{};
    if (!invert(createStorageToUprightTransform(
            frame.width(), frame.height(), frame.orientation()), uprightToStorageEdges))
    {
        return false;
    }
    const auto tensorToStorageCenters = compose(
        tensorToUpright,
        toPixelCenterTransform(uprightToStorageEdges));
    const auto tensorToStorage = toCvTransform(tensorToStorageCenters);
    cv::warpAffine(
        source,
        impl_->palmBgra,
        tensorToStorage,
        cv::Size{kPalmInputSize, kPalmInputSize},
        cv::INTER_LINEAR | cv::WARP_INVERSE_MAP,
        cv::BORDER_CONSTANT,
        cv::Scalar{});
    packBgraToRgbNhwc(impl_->palmBgra, tensor);

    return true;
}

bool HandGeometryProcessor::preprocessHand(
    const buffers::FrameBuffer& frame,
    const RotatedRegion& region,
    buffers::FloatTensorBuffer& tensor,
    HandPreprocessResult& result)
{
    constexpr std::array<std::int64_t, 4> kShape{1, kHandInputSize, kHandInputSize, 3};
    if (!hasShape(tensor, kShape) || frame.pixels().empty()
        || region.width <= 0.0F || region.height <= 0.0F)
    {
        return false;
    }

    const cv::Mat source(
        static_cast<int>(frame.height()),
        static_cast<int>(frame.width()),
        CV_8UC4,
        const_cast<std::uint8_t*>(frame.pixels().data()),
        frame.stride());

    const float cosine = std::cos(region.rotationRadiansClockwise);
    const float sine = std::sin(region.rotationRadiansClockwise);
    const cv::Point2f horizontal{cosine * region.width * 0.5F, sine * region.width * 0.5F};
    const cv::Point2f vertical{-sine * region.height * 0.5F, cosine * region.height * 0.5F};
    const cv::Point2f center{region.center.x, region.center.y};

    const std::array<cv::Point2f, 3> tensorPoints{
        cv::Point2f{0.0F, 0.0F},
        cv::Point2f{static_cast<float>(kHandInputSize), 0.0F},
        cv::Point2f{0.0F, static_cast<float>(kHandInputSize)}
    };
    const std::array<cv::Point2f, 3> uprightPoints{
        center - horizontal - vertical,
        center + horizontal - vertical,
        center - horizontal + vertical
    };
    const cv::Mat tensorToSource = cv::getAffineTransform(tensorPoints.data(), uprightPoints.data());
    AffineTransform uprightToStorageEdges{};
    if (!invert(createStorageToUprightTransform(
            frame.width(), frame.height(), frame.orientation()), uprightToStorageEdges))
    {
        return false;
    }
    const auto tensorToStorageCenters = compose(
        toTransform(tensorToSource),
        toPixelCenterTransform(uprightToStorageEdges));
    const auto tensorToStorage = toCvTransform(tensorToStorageCenters);
    cv::warpAffine(
        source,
        impl_->handBgra,
        tensorToStorage,
        cv::Size{kHandInputSize, kHandInputSize},
        cv::INTER_LINEAR | cv::WARP_INVERSE_MAP,
        cv::BORDER_CONSTANT,
        cv::Scalar{});
    packBgraToRgbNhwc(impl_->handBgra, tensor);

    cv::Mat sourceToTensor;
    cv::invertAffineTransform(tensorToSource, sourceToTensor);
    result.tensorToSource = toTransform(tensorToSource);
    result.sourceToTensor = toTransform(sourceToTensor);
    return true;
}
}

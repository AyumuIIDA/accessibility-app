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
}

struct HandGeometryProcessor::Impl
{
    cv::Mat resizedPalmBgra;
    cv::Mat palmBgra{cv::Mat::zeros(kPalmInputSize, kPalmInputSize, CV_8UC4)};
    cv::Mat handBgra{cv::Mat::zeros(kHandInputSize, kHandInputSize, CV_8UC4)};
};

HandGeometryProcessor::HandGeometryProcessor() : impl_{std::make_unique<Impl>()}
{
}

HandGeometryProcessor::~HandGeometryProcessor() = default;

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

    const float ratio = (std::min)(
        kPalmInputSize / static_cast<float>(frame.width()),
        kPalmInputSize / static_cast<float>(frame.height()));
    const int resizedWidth = (std::max)(1, static_cast<int>(frame.width() * ratio));
    const int resizedHeight = (std::max)(1, static_cast<int>(frame.height() * ratio));
    const int left = (kPalmInputSize - resizedWidth) / 2;
    const int top = (kPalmInputSize - resizedHeight) / 2;

    cv::resize(
        source,
        impl_->resizedPalmBgra,
        cv::Size{resizedWidth, resizedHeight},
        0.0,
        0.0,
        cv::INTER_LINEAR);
    impl_->palmBgra.setTo(cv::Scalar{});
    impl_->resizedPalmBgra.copyTo(
        impl_->palmBgra(cv::Rect{left, top, resizedWidth, resizedHeight}));
    packBgraToRgbNhwc(impl_->palmBgra, tensor);

    result.transform.scaleX = resizedWidth / static_cast<float>(frame.width());
    result.transform.scaleY = resizedHeight / static_cast<float>(frame.height());
    result.transform.padLeft = static_cast<float>(left);
    result.transform.padTop = static_cast<float>(top);
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
    const std::array<cv::Point2f, 3> sourcePoints{
        center - horizontal - vertical,
        center + horizontal - vertical,
        center - horizontal + vertical
    };

    const cv::Mat tensorToSource = cv::getAffineTransform(tensorPoints.data(), sourcePoints.data());
    cv::warpAffine(
        source,
        impl_->handBgra,
        tensorToSource,
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

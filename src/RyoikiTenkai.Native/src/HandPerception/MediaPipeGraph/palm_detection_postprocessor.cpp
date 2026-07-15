#include "HandPerception/MediaPipeGraph/palm_detection_postprocessor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ryoiki::hand_perception
{
namespace
{
constexpr float kPalmInputSize = 192.0F;

float sigmoid(const float value) noexcept
{
    if (value >= 0.0F)
    {
        const float exponential = std::exp(-value);
        return 1.0F / (1.0F + exponential);
    }

    const float exponential = std::exp(value);
    return exponential / (1.0F + exponential);
}

bool isFinite(const PalmDetection& detection) noexcept
{
    if (!std::isfinite(detection.score)
        || !std::isfinite(detection.box.left)
        || !std::isfinite(detection.box.top)
        || !std::isfinite(detection.box.right)
        || !std::isfinite(detection.box.bottom))
    {
        return false;
    }

    return std::all_of(
        detection.keypoints.begin(),
        detection.keypoints.end(),
        [](const geometry::Point2f point)
        {
            return std::isfinite(point.x) && std::isfinite(point.y);
        });
}
}

void PalmDetectionResult::clear() noexcept
{
    size_ = 0;
}

bool PalmDetectionResult::add(const PalmDetection& detection) noexcept
{
    if (size_ >= detections_.size())
    {
        return false;
    }
    detections_[size_++] = detection;
    return true;
}

std::size_t PalmDetectionResult::size() const noexcept
{
    return size_;
}

const PalmDetection& PalmDetectionResult::operator[](const std::size_t index) const noexcept
{
    return detections_[index];
}

PalmDetectionPostprocessor::PalmDetectionPostprocessor(const PalmPostprocessOptions options)
    : options_{options}, anchors_{createAnchors()}
{
}

void PalmDetectionPostprocessor::process(
    const PalmDetectionRawOutput& rawOutput,
    const geometry::LetterboxTransform& transform,
    PalmDetectionResult& result)
{
    result.clear();
    std::size_t candidateCount = 0;
    const auto regressions = rawOutput.regressions();
    const auto scores = rawOutput.scores();

    for (std::size_t anchorIndex = 0; anchorIndex < anchors_.size(); ++anchorIndex)
    {
        const float score = sigmoid(scores[anchorIndex]);
        if (!std::isfinite(score) || score < options_.scoreThreshold)
        {
            continue;
        }

        const auto regressionOffset = anchorIndex * PalmDetectionRawOutput::kRegressionCount;
        const auto& anchor = anchors_[anchorIndex];
        const float centerX = regressions[regressionOffset] + anchor.x * kPalmInputSize;
        const float centerY = regressions[regressionOffset + 1] + anchor.y * kPalmInputSize;
        const float width = regressions[regressionOffset + 2];
        const float height = regressions[regressionOffset + 3];

        PalmDetection detection;
        const auto topLeft = transform.tensorToSource({
            centerX - width * 0.5F,
            centerY - height * 0.5F});
        const auto bottomRight = transform.tensorToSource({
            centerX + width * 0.5F,
            centerY + height * 0.5F});
        detection.box = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
        detection.score = score;

        for (std::size_t keypointIndex = 0; keypointIndex < detection.keypoints.size(); ++keypointIndex)
        {
            const auto keypointOffset = regressionOffset + 4 + keypointIndex * 2;
            detection.keypoints[keypointIndex] = transform.tensorToSource({
                regressions[keypointOffset] + anchor.x * kPalmInputSize,
                regressions[keypointOffset + 1] + anchor.y * kPalmInputSize});
        }

        if (isFinite(detection))
        {
            candidates_[candidateCount++] = detection;
        }
    }

    std::sort(
        candidates_.begin(),
        candidates_.begin() + static_cast<std::ptrdiff_t>(candidateCount),
        [](const PalmDetection& first, const PalmDetection& second)
        {
            return first.score > second.score;
        });

    for (std::size_t candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex)
    {
        const auto& candidate = candidates_[candidateIndex];
        bool overlaps = false;
        for (std::size_t selectedIndex = 0; selectedIndex < result.size(); ++selectedIndex)
        {
            if (intersectionOverUnion(candidate.box, result[selectedIndex].box) >= options_.nmsThreshold)
            {
                overlaps = true;
                break;
            }
        }

        if (!overlaps && !result.add(candidate))
        {
            break;
        }
    }
}

std::array<PalmDetectionPostprocessor::Anchor, PalmDetectionRawOutput::kAnchorCount>
PalmDetectionPostprocessor::createAnchors()
{
    std::array<Anchor, PalmDetectionRawOutput::kAnchorCount> anchors{};
    std::size_t index = 0;
    const auto addLayer = [&anchors, &index](const int featureMapSize, const int anchorsPerCell)
    {
        for (int y = 0; y < featureMapSize; ++y)
        {
            for (int x = 0; x < featureMapSize; ++x)
            {
                for (int anchor = 0; anchor < anchorsPerCell; ++anchor)
                {
                    anchors[index++] = {
                        (static_cast<float>(x) + 0.5F) / featureMapSize,
                        (static_cast<float>(y) + 0.5F) / featureMapSize};
                }
            }
        }
    };
    addLayer(24, 2);
    addLayer(12, 6);
    return anchors;
}

float PalmDetectionPostprocessor::intersectionOverUnion(
    const BoundingBox& first,
    const BoundingBox& second) noexcept
{
    const float intersectionWidth = (std::max)(0.0F, (std::min)(first.right, second.right)
        - (std::max)(first.left, second.left));
    const float intersectionHeight = (std::max)(0.0F, (std::min)(first.bottom, second.bottom)
        - (std::max)(first.top, second.top));
    const float intersection = intersectionWidth * intersectionHeight;
    const float firstArea = (std::max)(0.0F, first.right - first.left)
        * (std::max)(0.0F, first.bottom - first.top);
    const float secondArea = (std::max)(0.0F, second.right - second.left)
        * (std::max)(0.0F, second.bottom - second.top);
    const float unionArea = firstArea + secondArea - intersection;
    return unionArea > std::numeric_limits<float>::epsilon()
        ? intersection / unionArea
        : 0.0F;
}
}

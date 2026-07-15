#pragma once

#include "Geometry/geometry_types.h"
#include "HandPerception/ModelRunners/palm_detection_runner.h"

#include <array>
#include <cstddef>

namespace ryoiki::hand_perception
{
struct BoundingBox
{
    float left{0.0F};
    float top{0.0F};
    float right{0.0F};
    float bottom{0.0F};
};

struct PalmDetection
{
    BoundingBox box;
    std::array<geometry::Point2f, 7> keypoints{};
    float score{0.0F};
};

class PalmDetectionResult final
{
public:
    static constexpr std::size_t kCapacity = 4;

    void clear() noexcept;
    bool add(const PalmDetection& detection) noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const PalmDetection& operator[](std::size_t index) const noexcept;

private:
    std::array<PalmDetection, kCapacity> detections_{};
    std::size_t size_{0};
};

struct PalmPostprocessOptions
{
    float scoreThreshold{0.35F};
    float nmsThreshold{0.3F};
};

class PalmDetectionPostprocessor final
{
public:
    explicit PalmDetectionPostprocessor(PalmPostprocessOptions options = {});

    void process(
        const PalmDetectionRawOutput& rawOutput,
        const geometry::LetterboxTransform& transform,
        PalmDetectionResult& result);

private:
    struct Anchor
    {
        float x{0.0F};
        float y{0.0F};
    };

    static std::array<Anchor, PalmDetectionRawOutput::kAnchorCount> createAnchors();
    static float intersectionOverUnion(const BoundingBox& first, const BoundingBox& second) noexcept;

    PalmPostprocessOptions options_;
    std::array<Anchor, PalmDetectionRawOutput::kAnchorCount> anchors_;
    std::array<PalmDetection, PalmDetectionRawOutput::kAnchorCount> candidates_{};
};
}

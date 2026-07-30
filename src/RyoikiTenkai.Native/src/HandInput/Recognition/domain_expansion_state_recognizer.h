#pragma once

#include "HandInput/Recognition/hand_state.h"
#include "HandInput/Recognition/timed_state_stabilizer.h"
#include "HandPerception/MediaPipeGraph/hand_landmark_graph.h"

namespace ryoiki::hand_input::recognition
{
struct DomainExpansionStateFeatures
{
    float indexExtended{0.0F};
    float middleWrap{0.0F};
    float middleCurled{0.0F};
    float ringCurled{0.0F};
    float pinkyCurled{0.0F};
    float thumbTucked{0.0F};
};

struct DomainExpansionStateResult
{
    DomainExpansionStateFeatures features{};
    float confidence{0.0F};
    bool detected{false};
    bool inputValid{false};
};

[[nodiscard]] DomainExpansionStateResult recognizeDomainExpansionState(
    const hand_perception::HandLandmarkResult& hand) noexcept;

class DomainExpansionStateRecognizer final
{
public:
    DomainExpansionStateRecognizer() noexcept;

    [[nodiscard]] HandStateResult process(
        const DomainExpansionStateResult& sample,
        float inputQuality,
        std::uint64_t frameId,
        std::uint64_t timestampUs) noexcept;

    void reset() noexcept;

private:
    TimedStateStabilizer stabilizer_;
};
}

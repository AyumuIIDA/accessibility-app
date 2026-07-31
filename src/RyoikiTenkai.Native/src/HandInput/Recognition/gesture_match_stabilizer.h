#pragma once
#include <cstdint>

namespace ryoiki::hand_input::recognition
{
inline constexpr std::uint32_t kGestureConsecutiveMatchThreshold = 2;
inline constexpr std::uint64_t kGestureCooldownUs = 1'500'000;

enum class GestureMatchDecision : std::uint32_t
{
    NoMatch,
    WaitingForConsecutiveMatch,
    DuplicateSegmentSuppressed,
    Cooldown,
    Confirmed
};

struct GestureMatchCandidate
{
    std::uint32_t gestureId{0};
    float confidence{0.0F};
    std::uint64_t timestampUs{0};
    std::uint64_t segmentStartUs{0};
    std::uint64_t segmentEndUs{0};
};

struct GestureMatchStabilizationResult
{
    GestureMatchDecision decision{GestureMatchDecision::NoMatch};
    std::uint32_t consecutiveCount{0};
    std::uint64_t cooldownRemainingUs{0};
    bool confirmed{false};
};

class GestureMatchStabilizer final
{
public:
    [[nodiscard]] GestureMatchStabilizationResult process(
        const GestureMatchCandidate* match) noexcept;
    // Mirrors PR Reset: pending confirmation is cleared; cooldown and the
    // previously triggered segment deliberately remain.
    void reset() noexcept;
    // Registry identity changed, so suppression state from old definitions
    // must not affect the new set.
    void resetForRegistryChange() noexcept;
private:
    std::uint32_t candidateGestureId_{0};
    std::uint32_t candidateCount_{0};
    std::uint64_t lastTriggeredAtUs_{0};
    std::uint64_t lastTriggeredSegmentStartUs_{0};
    std::uint64_t lastTriggeredSegmentEndUs_{0};
    bool hasTriggeredSegment_{false};
};
}

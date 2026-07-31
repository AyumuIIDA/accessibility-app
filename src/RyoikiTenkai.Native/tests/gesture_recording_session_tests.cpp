#include "HandInput/Measurements/gesture_candidate_window_history.h"
#include "HandInput/Recognition/gesture_recording_session.h"

#include <cstdint>
#include <array>
#include <iostream>
#include <stdexcept>

namespace
{
namespace perception = ryoiki::hand_perception;
namespace measurements = ryoiki::hand_input::measurements;
namespace recognition = ryoiki::hand_input::recognition;

void require(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error{message};
    }
}

perception::HandPerceptionResult makeHands(const std::uint32_t count)
{
    perception::HandPerceptionResult result{};
    result.handCount = count;
    for (std::uint32_t index = 0; index < count; ++index)
    {
        result.trackIds[index] = index + 10;
        result.hands[index].detected = true;
        result.hands[index].confidence = 0.95F;
    }
    return result;
}

perception::HandPerceptionResult makeMovingHand(const float dx)
{
    auto result = makeHands(1);
    constexpr std::array<perception::Landmark3f, 5> offsets{{
        {-30.0F, -20.0F, 0.0F}, {40.0F, -70.0F, 0.0F},
        {0.0F, -100.0F, 0.0F}, {-40.0F, -70.0F, 0.0F}, {-70.0F, -40.0F, 0.0F}}};
    constexpr std::array<std::size_t, 5> bases{1, 5, 9, 13, 17};
    constexpr std::array<std::size_t, 5> tips{4, 8, 12, 16, 20};
    auto& landmarks = result.hands[0].landmarks;
    landmarks[0] = {300.0F + dx, 450.0F, 0.0F};
    for (std::size_t finger = 0; finger < 5; ++finger)
    {
        const perception::Landmark3f base{landmarks[0].x + offsets[finger].x,
            landmarks[0].y + offsets[finger].y, 0.0F};
        const perception::Landmark3f tip{landmarks[0].x + offsets[finger].x * 2.0F,
            landmarks[0].y + offsets[finger].y * 2.0F, 0.0F};
        landmarks[bases[finger]] = base;
        landmarks[tips[finger]] = tip;
        for (std::size_t joint = 1; joint <= 2; ++joint)
        {
            const float t = static_cast<float>(joint) / 3.0F;
            landmarks[bases[finger] + joint] = {
                base.x + (tip.x - base.x) * t, base.y + (tip.y - base.y) * t, 0.0F};
        }
    }
    return result;
}

recognition::GestureRecordingStatus captureTake(
    recognition::GestureRecordingSession& session,
    recognition::GestureTemplateRegistry& registry,
    const std::uint32_t definitionId,
    std::uint64_t& frameId,
    std::uint64_t& timestampUs)
{
    session.submitCommand(recognition::GestureRecordingCommandKind::Begin, definitionId);
    recognition::GestureRecordingStatus status{};
    for (int index = 0; index < 130; ++index)
    {
        status = session.processFrame(
            makeMovingHand(static_cast<float>(index) * 3.0F), frameId++, timestampUs, registry);
        timestampUs += 20'000;
    }
    session.submitCommand(recognition::GestureRecordingCommandKind::Finish, 0);
    return session.processFrame(makeMovingHand(390.0F), frameId++, timestampUs, registry);
}

void testWaitsForExactlyOneHandAndLocksWithoutRebinding()
{
    recognition::GestureRecordingSession session;
    recognition::GestureTemplateRegistry registry;
    session.submitCommand(recognition::GestureRecordingCommandKind::Begin, 42);

    auto status = session.processFrame(makeHands(2), 1, 1'000, registry);
    require(status.state == recognition::GestureRecordingState::Recording,
        "A two-hand take did not start without a UI-selected track ID.");
    require(status.trackId == 10 && status.sampleCount == 1,
        "The confidence-selected primary track was not captured alongside the frame set.");

    status = session.processFrame(makeHands(1), 2, 2'000, registry);
    require(status.state == recognition::GestureRecordingState::Recording,
        "Exactly one usable hand did not start recording.");
    require(status.trackId == 10 && status.sampleCount == 2,
        "The locked primary track was not retained when the second hand disappeared.");
    require(status.currentTake == 1 && status.acceptedTakeCount == 0
            && status.requiredTakeCount == 3 && status.attemptCount == 1,
        "A fresh Begin must expose one-based take 1 and zero accepted takes.");

    auto otherHand = makeHands(1);
    otherHand.trackIds[0] = 99;
    for (std::uint32_t gap = 0; gap <= measurements::kHandTopologyGapToleranceFrames; ++gap)
    {
        status = session.processFrame(otherHand, 3 + gap, 3'000 + gap * 1'000, registry);
    }
    require(status.state == recognition::GestureRecordingState::TrackLost,
        "A lost locked track was rebound instead of rejecting the take.");
    require(status.trackId == 10 && status.sampleCount == 2,
        "Samples from a different track entered the recording take.");
}

void testFinishUsesOnlyExplicitSessionAndCancelResetsLifecycle()
{
    recognition::GestureRecordingSession session;
    recognition::GestureTemplateRegistry registry;
    session.submitCommand(recognition::GestureRecordingCommandKind::Begin, 77);
    auto status = session.processFrame(makeHands(0), 1, 1'000, registry);
    require(status.state == recognition::GestureRecordingState::AwaitingHand,
        "Begin without a hand must remain armed.");

    session.submitCommand(recognition::GestureRecordingCommandKind::Finish, 0);
    status = session.processFrame(makeHands(0), 2, 2'000, registry);
    require(status.state == recognition::GestureRecordingState::Rejected,
        "Finish without captured samples must reject the take.");
    require(status.lastResultTemplateId == 77,
        "Finish lost the template ID supplied by Begin.");
    require(registry.size() == 0, "An empty explicit take registered a template.");

    session.submitCommand(recognition::GestureRecordingCommandKind::Begin, 88);
    status = session.processFrame(makeHands(1), 3, 3'000, registry);
    require(status.sampleCount == 1, "A fresh Begin did not reset and start a new take.");
    session.submitCommand(recognition::GestureRecordingCommandKind::Cancel, 0);
    status = session.processFrame(makeHands(1), 4, 4'000, registry);
    require(status.state == recognition::GestureRecordingState::Cancelled,
        "Cancel did not terminate the explicit recording take.");
}

void testThreeAcceptedTakesCommitAtomicallyAndFailuresRetrySameTake()
{
    recognition::GestureRecordingSession session;
    recognition::GestureTemplateRegistry registry;
    std::uint64_t frameId = 1;
    std::uint64_t timestampUs = 1'000'000;

    auto status = captureTake(session, registry, 500, frameId, timestampUs);
    require(status.state == recognition::GestureRecordingState::AwaitingNextTake
            && status.acceptedTakeCount == 1 && status.currentTake == 2
            && status.requiredTakeCount == 3 && status.attemptCount == 1,
        "After accepting take 1, status must point at one-based take 2 without activating a definition.");
    require(registry.size() == 0, "A partial recording session leaked into the active registry.");

    // An intentionally short retry fails take 2 and must preserve accepted take 1.
    session.submitCommand(recognition::GestureRecordingCommandKind::Begin, 500);
    status = session.processFrame(makeMovingHand(0.0F), frameId++, timestampUs, registry);
    session.submitCommand(recognition::GestureRecordingCommandKind::Finish, 0);
    status = session.processFrame(makeMovingHand(0.0F), frameId++, timestampUs + 20'000, registry);
    require(status.state == recognition::GestureRecordingState::Rejected
            && status.acceptedTakeCount == 1 && status.currentTake == 2
            && status.attemptCount == 2,
        "A failed take must retain accepted takes and retry the same take index.");

    timestampUs += 100'000;
    status = captureTake(session, registry, 500, frameId, timestampUs);
    require(status.state == recognition::GestureRecordingState::AwaitingNextTake
            && status.acceptedTakeCount == 2 && status.currentTake == 3
            && status.attemptCount == 3,
        "Retrying take 2 did not advance status to one-based take 3 only after acceptance.");
    timestampUs += 100'000;
    status = captureTake(session, registry, 500, frameId, timestampUs);
    require(status.state == recognition::GestureRecordingState::Completed
            && status.acceptedTakeCount == 3 && status.currentTake == 3
            && status.requiredTakeCount == 3 && status.attemptCount == 4,
        "Three accepted takes must complete the native session.");
    require(registry.size() == 3, "The completed definition must retain three separate templates.");
    const auto* definition = registry.find(500);
    require(definition != nullptr && definition->trialCount == 3,
        "The registry did not group all accepted trials under the definition ID.");
}
}

int main()
{
    try
    {
        testWaitsForExactlyOneHandAndLocksWithoutRebinding();
        testFinishUsesOnlyExplicitSessionAndCancelResetsLifecycle();
        testThreeAcceptedTakesCommitAtomicallyAndFailuresRetrySameTake();
        std::cout << "Gesture recording session tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Gesture recording session tests failed: " << error.what() << '\n';
        return 1;
    }
}

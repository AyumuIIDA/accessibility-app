#include "HandInput/Measurements/gesture_candidate_window_history.h"
#include "HandInput/Measurements/hand_unified_feature_frame.h"
#include "HandInput/Recognition/gesture_template_registry.h"
#include "HandInput/Recognition/unified_sequence_gesture_comparator.h"
#include "HandPerception/MediaPipeGraph/hand_perception_graph.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
using ryoiki::hand_perception::Landmark3f;
namespace measurements = ryoiki::hand_input::measurements;
namespace recognition = ryoiki::hand_input::recognition;

void require(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error{message};
    }
}

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Same synthetic fully-open-hand construction as
// gesture_candidate_window_history_tests.cpp, duplicated locally so this
// test file stays independently buildable/testable.
constexpr std::array<Landmark3f, 5> kFingerBaseOffsets{{
    {-30.0F, -20.0F, 0.0F},
    {40.0F, -70.0F, 0.0F},
    {0.0F, -100.0F, 0.0F},
    {-40.0F, -70.0F, 0.0F},
    {-70.0F, -40.0F, 0.0F}}};
constexpr std::array<std::size_t, 5> kBaseIndices{1, 5, 9, 13, 17};
constexpr std::array<std::size_t, 5> kTipIndices{4, 8, 12, 16, 20};
constexpr Landmark3f kWrist{300.0F, 450.0F, 0.0F};
constexpr float kOpenMultiplier = 2.0F;

std::array<Landmark3f, 21> makeOpenHandLandmarks(const float dx, const float dy)
{
    std::array<Landmark3f, 21> landmarks{};
    const Landmark3f wrist{kWrist.x + dx, kWrist.y + dy, 0.0F};
    landmarks[0] = wrist;
    for (std::size_t finger = 0; finger < 5; ++finger)
    {
        const Landmark3f base{
            wrist.x + kFingerBaseOffsets[finger].x, wrist.y + kFingerBaseOffsets[finger].y, 0.0F};
        const Landmark3f tip{
            wrist.x + kFingerBaseOffsets[finger].x * kOpenMultiplier,
            wrist.y + kFingerBaseOffsets[finger].y * kOpenMultiplier,
            0.0F};
        landmarks[kBaseIndices[finger]] = base;
        landmarks[kTipIndices[finger]] = tip;
        for (std::size_t joint = 1; joint <= 2; ++joint)
        {
            const float f = static_cast<float>(joint) / 3.0F;
            landmarks[kBaseIndices[finger] + joint] = {
                base.x + (tip.x - base.x) * f, base.y + (tip.y - base.y) * f, 0.0F};
        }
    }
    return landmarks;
}

measurements::UnifiedFeatureObservation makeObservation(
    const std::uint64_t timestampUs, const float dx = 0.0F, const float dy = 0.0F)
{
    measurements::UnifiedFeatureObservation observation{};
    observation.timestampUs = timestampUs;
    observation.imageLandmarks = makeOpenHandLandmarks(dx, dy);
    observation.handedness = 0.8F;
    observation.boundingBoxArea = 0.0F;
    return observation;
}

// A dense, ~2580 ms, fully-usable history whose wrist ramps a large,
// monotonic translation: every candidate duration (including the 2100/2500 ms
// ones needed to satisfy the template-creation duration gate) ends up with
// translationDistance well above kMinimumTranslationDistance, so its longer
// candidates are accepted as templates.
measurements::GestureCandidateWindowHistory makeMovingHistory()
{
    measurements::GestureCandidateWindowHistory history;
    constexpr int kFrameCount = 130;
    constexpr std::uint64_t kIntervalUs = 20'000;
    for (int i = 0; i < kFrameCount; ++i)
    {
        history.push(makeObservation(static_cast<std::uint64_t>(i) * kIntervalUs, static_cast<float>(i) * 3.0F, 0.0F), true);
    }
    return history;
}

// A dense, fully-usable, but perfectly stationary history: every candidate
// duration is eligible (duration/usable-frame gates pass) but topology
// carries no meaningful action signal.
measurements::GestureCandidateWindowHistory makeStationaryHistory()
{
    measurements::GestureCandidateWindowHistory history;
    constexpr int kFrameCount = 130;
    constexpr std::uint64_t kIntervalUs = 20'000;
    for (int i = 0; i < kFrameCount; ++i)
    {
        history.push(makeObservation(static_cast<std::uint64_t>(i) * kIntervalUs, 0.0F, 0.0F), true);
    }
    return history;
}

using CandidateArray =
    std::array<measurements::GestureCandidateWindow, measurements::kGestureCandidateDurationsMs.size()>;

// ---------------------------------------------------------------------
// createTemplateFromCandidateWindow
// ---------------------------------------------------------------------

void testCreateTemplateRejectsInvalidWindow()
{
    measurements::GestureCandidateWindow window{};
    window.valid = false;
    const auto result = recognition::createTemplateFromCandidateWindow(window);
    require(!result.created, "An invalid candidate window must never produce a template.");
    require(!result.rejectionReason.empty(), "A rejected template creation must always report a reason.");
}

void testCreateTemplateRejectsTooShortDuration()
{
    auto history = makeMovingHistory();
    CandidateArray windows{};
    const auto written = history.buildCandidateWindows(windows);
    require(written == measurements::kGestureCandidateDurationsMs.size(), "Setup: expected every duration eligible.");
    // Index 0 is the 1400 ms candidate: below the template's 1900 ms floor.
    const auto result = recognition::createTemplateFromCandidateWindow(windows[0]);
    require(!result.created, "A candidate shorter than the template duration floor must be rejected.");
    require(contains(result.rejectionReason, "1.9 seconds"),
        "The rejection reason must identify the duration floor.");
}

void testCreateTemplateRejectsStaticPose()
{
    auto history = makeStationaryHistory();
    CandidateArray windows{};
    const auto written = history.buildCandidateWindows(windows);
    require(written == measurements::kGestureCandidateDurationsMs.size(), "Setup: expected every duration eligible.");
    // Index 3 is the 2500 ms candidate: long/dense enough, but stationary.
    const auto result = recognition::createTemplateFromCandidateWindow(windows[3]);
    require(!result.created, "A stationary candidate must not be accepted as a gesture template.");
    require(contains(result.rejectionReason, "stable pose"),
        "The rejection reason must identify a non-gesture stable pose.");
}

void testCreateTemplateSucceedsForRealMotion()
{
    auto history = makeMovingHistory();
    CandidateArray windows{};
    const auto written = history.buildCandidateWindows(windows);
    require(written == measurements::kGestureCandidateDurationsMs.size(), "Setup: expected every duration eligible.");
    // Index 3 is the 2500 ms candidate: long/dense enough and has real motion.
    const auto result = recognition::createTemplateFromCandidateWindow(windows[3]);
    require(result.created, "A long, dense, moving candidate must be accepted as a gesture template.");
    require(result.rejectionReason.empty(), "A created template must not carry a rejection reason.");
    require(result.sequence.topology.valid, "A created template's topology must be valid.");
    require(result.sequence.frames.size() == measurements::kUnifiedSequenceLength,
        "A created template must carry the fixed 32-point unified sequence.");
    require(result.sequence.visualization.valid,
        "A created template must retain the registration trajectory and skeleton.");
    require(result.sequence.visualization.frames.size() == measurements::kUnifiedSequenceLength,
        "Template visualization must be resampled to the same fixed 32 points as DTW.");
}

// ---------------------------------------------------------------------
// GestureTemplateRegistry
// ---------------------------------------------------------------------

recognition::UnifiedSequenceTemplate makeTemplateFromMovingHistory()
{
    auto history = makeMovingHistory();
    CandidateArray windows{};
    history.buildCandidateWindows(windows);
    const auto result = recognition::createTemplateFromCandidateWindow(windows[3]);
    return result.sequence;
}

void addAcceptedDefinition(
    recognition::GestureTemplateRegistry& registry,
    const std::uint32_t id,
    const recognition::UnifiedSequenceTemplate& sequence)
{
    const std::array<recognition::UnifiedSequenceTemplate,
        recognition::kRequiredGestureTemplateCount> takes{sequence, sequence, sequence};
    require(registry.appendTemplateSet(id, takes), "Setup: expected a three-take definition.");
}

void testRegistryRegisterFindRemoveClear()
{
    recognition::GestureTemplateRegistry registry;
    require(registry.size() == 0, "A fresh registry must start empty.");
    require(registry.find(100) == nullptr, "An empty registry must not find any template.");

    const auto sequence = makeTemplateFromMovingHistory();
    require(registry.registerTemplate(100, sequence), "Registering a new template ID must succeed.");
    require(registry.size() == 1, "The registry must report one registered template.");
    require(registry.find(100) != nullptr, "A registered template must be findable by ID.");

    // Re-registering the same ID replaces in place without growing size.
    require(registry.registerTemplate(100, sequence), "Re-registering an existing ID must succeed (replacement).");
    require(registry.size() == 1, "Replacing an existing template must not change the registry size.");

    require(registry.removeTemplate(100), "Removing a present template must succeed.");
    require(registry.size() == 0, "Removing the only template must empty the registry.");
    require(!registry.removeTemplate(100), "Removing an already-absent template must fail.");

    require(registry.registerTemplate(1, sequence) && registry.registerTemplate(2, sequence),
        "Setup: expected two templates registered.");
    registry.clear();
    require(registry.size() == 0, "clear() must remove every registered template.");
}

void testRegistryCapacityFullRejectsNewIdButAllowsReplacement()
{
    recognition::GestureTemplateRegistry registry;
    const auto sequence = makeTemplateFromMovingHistory();
    for (std::uint32_t id = 0; id < recognition::kMaxGestureTemplates; ++id)
    {
        require(registry.registerTemplate(id, sequence), "Filling the registry to capacity must succeed.");
    }
    require(registry.size() == recognition::kMaxGestureTemplates, "Setup: expected the registry to be full.");

    require(!registry.registerTemplate(
                static_cast<std::uint32_t>(recognition::kMaxGestureTemplates), sequence),
        "Registering a new ID beyond capacity must fail.");
    require(registry.size() == recognition::kMaxGestureTemplates,
        "A rejected registration must not change the registry size.");

    require(registry.registerTemplate(0, sequence),
        "Replacing an existing ID must still succeed when the registry is full.");
    require(registry.size() == recognition::kMaxGestureTemplates,
        "A replacement must not change the registry size.");
}

void testRegistriesAreIndependent()
{
    recognition::GestureTemplateRegistry registryA;
    recognition::GestureTemplateRegistry registryB;
    const auto sequence = makeTemplateFromMovingHistory();
    require(registryA.registerTemplate(5, sequence), "Setup: expected registration to succeed.");
    require(registryA.size() == 1 && registryB.size() == 0,
        "Two registry instances must not share template storage.");
    require(registryB.find(5) == nullptr, "A template registered in one registry must not appear in another.");
}

// ---------------------------------------------------------------------
// findBestMatch
// ---------------------------------------------------------------------

void testFindBestMatchEmptyRegistryNeverMatches()
{
    auto history = makeMovingHistory();
    CandidateArray windows{};
    const auto written = history.buildCandidateWindows(windows);
    recognition::GestureTemplateRegistry registry;
    const auto result = recognition::findBestMatch(windows, written, registry);
    require(!result.matched, "An empty template registry must never produce a match.");
    require(result.attempts.empty(), "An empty template registry must attempt zero comparisons.");
}

void testFindBestMatchEmptyCandidatesNeverMatches()
{
    CandidateArray windows{};
    recognition::GestureTemplateRegistry registry;
    addAcceptedDefinition(registry, 1, makeTemplateFromMovingHistory());
    const auto result = recognition::findBestMatch(windows, 0, registry);
    require(!result.matched, "Zero candidate windows must never produce a match.");
    require(result.attempts.empty(), "Zero candidate windows must attempt zero comparisons.");
}

void testDefinitionWithFewerThanThreeTakesIsInactive()
{
    auto history = makeMovingHistory();
    CandidateArray windows{};
    const auto written = history.buildCandidateWindows(windows);
    recognition::GestureTemplateRegistry registry;
    require(registry.registerTemplate(9, makeTemplateFromMovingHistory()), "Setup: expected one stored take.");
    const auto result = recognition::findBestMatch(windows, written, registry);
    require(!result.matched && result.attempts.empty(),
        "PR parity requires a definition with fewer than three accepted examples to stay inactive.");
}

void testFindBestMatchRejectionOnlyWhenTopologyNeverMatches()
{
    // A registered template built from real motion, matched against a
    // stationary candidate history: every attempt must be topology-rejected,
    // so no match is ever produced, but every attempt is still reported.
    recognition::GestureTemplateRegistry registry;
    addAcceptedDefinition(registry, 1, makeTemplateFromMovingHistory());

    auto stationaryHistory = makeStationaryHistory();
    CandidateArray windows{};
    const auto written = stationaryHistory.buildCandidateWindows(windows);
    require(written > 0, "Setup: expected at least one stationary candidate window.");

    const auto result = recognition::findBestMatch(windows, written, registry);
    require(!result.matched, "A stationary candidate history must never match a moving template.");
    require(result.attempts.size() == written * recognition::kRequiredGestureTemplateCount,
        "Every accepted take/candidate pair must be attempted and reported.");
    for (const auto& attempt : result.attempts)
    {
        require(attempt.templateId == 1, "Every attempt must report the template identity it was compared against.");
        require(!attempt.outcome.topologyRejectionReason.empty() || !attempt.outcome.comparison.eligible,
            "Every attempt against a non-matching candidate must be rejected (topology or DTW).");
    }
}

void testFindBestMatchPicksLowestScoringEligibleTemplate()
{
    // Two templates built from the same moving history's candidate: an exact
    // self-match (score ~0) and a perturbed copy (nonzero score). The exact
    // match must win.
    auto history = makeMovingHistory();
    CandidateArray windows{};
    const auto written = history.buildCandidateWindows(windows);
    require(written == measurements::kGestureCandidateDurationsMs.size(), "Setup: expected every duration eligible.");

    const auto exactResult = recognition::createTemplateFromCandidateWindow(windows[3]);
    require(exactResult.created, "Setup: expected the exact template to be created.");

    auto perturbed = exactResult.sequence;
    for (auto& frame : perturbed.frames)
    {
        frame.values[0] += 0.35F; // Large enough to raise the DTW score but stay eligible/topology-passing.
    }

    recognition::GestureTemplateRegistry registry;
    addAcceptedDefinition(registry, 10, perturbed);
    addAcceptedDefinition(registry, 20, exactResult.sequence);

    const auto result = recognition::findBestMatch(windows, written, registry);
    require(result.matched, "A registry containing the candidate's own exact template must match.");
    require(result.templateId == 20, "The exact (lowest-score) template must win over the perturbed one.");
    require(result.comparison.eligible, "The winning match must be eligible.");
    require(result.comparison.score < 1.0e-3F, "An exact self-match must score near zero.");
}

void testFindBestMatchReportsCandidateDurationAndTemplateIdentity()
{
    auto history = makeMovingHistory();
    CandidateArray windows{};
    const auto written = history.buildCandidateWindows(windows);
    const auto templateResult = recognition::createTemplateFromCandidateWindow(windows[3]);
    require(templateResult.created, "Setup: expected template creation to succeed.");

    recognition::GestureTemplateRegistry registry;
    addAcceptedDefinition(registry, 42, templateResult.sequence);

    const auto result = recognition::findBestMatch(windows, written, registry);
    require(result.matched, "Setup: expected a match.");
    require(result.templateId == 42, "The best match must report the winning template's identity.");
    require(result.candidateDurationMs == measurements::kGestureCandidateDurationsMs[3],
        "The best match must report the winning candidate's own duration.");
    require(result.candidate.usableFrameCount >= measurements::kGestureMinimumCandidateUsableFrames,
        "The best match must surface the winning candidate's usable frame count.");
    require(result.comparison.hasWarpRatio, "An eligible match's comparison must report a warp ratio.");
    require(result.comparison.hasReverseScore,
        "An eligible match's comparison must report the reverse-DTW diagnostic score.");
}
}

int main()
{
    try
    {
        testCreateTemplateRejectsInvalidWindow();
        testCreateTemplateRejectsTooShortDuration();
        testCreateTemplateRejectsStaticPose();
        testCreateTemplateSucceedsForRealMotion();
        testRegistryRegisterFindRemoveClear();
        testRegistryCapacityFullRejectsNewIdButAllowsReplacement();
        testRegistriesAreIndependent();
        testFindBestMatchEmptyRegistryNeverMatches();
        testFindBestMatchEmptyCandidatesNeverMatches();
        testDefinitionWithFewerThanThreeTakesIsInactive();
        testFindBestMatchRejectionOnlyWhenTopologyNeverMatches();
        testFindBestMatchPicksLowestScoringEligibleTemplate();
        testFindBestMatchReportsCandidateDurationAndTemplateIdentity();
        std::cout << "GestureTemplateRegistry tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}

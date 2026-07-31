#include "HandInput/Measurements/gesture_candidate_window_history.h"
#include "HandInput/Measurements/hand_topology_history.h"
#include "HandInput/Measurements/hand_unified_feature_frame.h"
#include "HandInput/Recognition/unified_sequence_gesture_comparator.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
namespace measurements = ryoiki::hand_input::measurements;
namespace recognition = ryoiki::hand_input::recognition;
using measurements::UnifiedFeatureFrame;

void require(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error{message};
    }
}

void requireNear(const double actual, const double expected, const double tolerance, const char* message)
{
    require(std::isfinite(actual), "Expected a finite value.");
    require(std::abs(actual - expected) <= tolerance, message);
}

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------
// Synthetic UnifiedFeatureFrame construction. Values are written directly
// into the flat 64-float vector at the offsets hand_unified_feature_frame.h
// documents, rather than derived from synthetic landmarks, so each test
// controls exactly one feature group at a time.
// ---------------------------------------------------------------------

UnifiedFeatureFrame makeFrame(
    const double timeOffsetMs,
    const float centerX = 0.0F,
    const float centerY = 0.0F,
    const float palmOrientationRadians = 0.0F,
    const float palmVelocity = 0.0F,
    const float signedPalmArea = 0.1F,
    const float palmCompression = 0.5F,
    const float palmDepthRange = 0.05F,
    const float handScaleRatio = 1.0F,
    const float boundingBoxAreaRatio = 1.0F,
    const float handedness = 0.8F)
{
    UnifiedFeatureFrame frame{};
    frame.timeOffsetMs = timeOffsetMs;
    frame.centerX = centerX;
    frame.centerY = centerY;
    frame.palmOrientationRadians = palmOrientationRadians;
    frame.palmVelocity = palmVelocity;
    for (std::size_t i = 0; i < measurements::kFingerStraightnessLength; ++i)
    {
        frame.values[measurements::kFingerStraightnessOffset + i] = 0.05F; // baseline: curled/closed.
    }
    frame.values[56] = std::sin(palmOrientationRadians);
    frame.values[57] = std::cos(palmOrientationRadians);
    frame.values[measurements::kHandednessOffset] = handedness;
    frame.values[measurements::kSignedPalmAreaOffset] = signedPalmArea;
    frame.values[measurements::kPalmCompressionOffset] = palmCompression;
    frame.values[measurements::kPalmDepthRangeOffset] = palmDepthRange;
    frame.values[measurements::kHandScaleRatioOffset] = handScaleRatio;
    frame.values[measurements::kBoundingBoxAreaRatioOffset] = boundingBoxAreaRatio;
    return frame;
}

std::vector<UnifiedFeatureFrame> makeBaselineSequence(const std::size_t count, const double intervalMs = 80.0)
{
    std::vector<UnifiedFeatureFrame> frames;
    frames.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        frames.push_back(makeFrame(static_cast<double>(i) * intervalMs));
    }
    return frames;
}

measurements::HandTopologySummary makeTopology(
    const float palmTravel,
    const float palmOrientationRangeRadians,
    const std::uint32_t startFingerStateMask,
    const std::uint32_t endFingerStateMask,
    const std::uint32_t fingerStateTransitionCount,
    const float topologyChangeScore,
    const std::uint32_t signedPalmAreaSignChanges = 0,
    const float palmCompressionDrop = 0.0F,
    const float palmTurnScore = 0.0F,
    const float handScaleRatioRange = 0.0F,
    const float handScaleRatioDelta = 0.0F,
    const float translationDistance = 0.0F,
    const float translationDeltaX = 0.0F,
    const float translationDeltaY = 0.0F)
{
    measurements::HandTopologySummary summary{};
    summary.valid = true;
    summary.palmTravel = palmTravel;
    summary.palmOrientationRangeRadians = palmOrientationRangeRadians;
    summary.startFingerStateMask = startFingerStateMask;
    summary.endFingerStateMask = endFingerStateMask;
    summary.fingerStateTransitionCount = fingerStateTransitionCount;
    summary.topologyChangeScore = topologyChangeScore;
    summary.signedPalmAreaSignChanges = signedPalmAreaSignChanges;
    summary.palmCompressionDrop = palmCompressionDrop;
    summary.palmTurnScore = palmTurnScore;
    summary.handScaleRatioRange = handScaleRatioRange;
    summary.handScaleRatioDelta = handScaleRatioDelta;
    summary.translationDistance = translationDistance;
    summary.translationDeltaX = translationDeltaX;
    summary.translationDeltaY = translationDeltaY;
    return summary;
}

// A template whose overall change score comfortably satisfies every gate's
// "template is specific enough to test" precondition, with otherwise
// deliberately weak sub-signals so each test can isolate one gate at a time
// by overriding only the field(s) it exercises.
measurements::HandTopologySummary makeGenericTemplate()
{
    return makeTopology(
        /*palmTravel=*/1.0F,
        /*palmOrientationRangeRadians=*/1.0F,
        /*startFingerStateMask=*/0b00000U,
        /*endFingerStateMask=*/0b00000U,
        /*fingerStateTransitionCount=*/0U,
        /*topologyChangeScore=*/1.0F);
}

// ---------------------------------------------------------------------
// Topology rejection gates.
// ---------------------------------------------------------------------

void testTopologyLowChangeTemplateBypassesAllGates()
{
    const auto candidate = makeTopology(0.0F, 0.0F, 0b00001U, 0b11111U, 5, 0.0F);
    const auto templateSummary = makeTopology(0.0F, 0.0F, 0b11111U, 0b00001U, 5, 0.10F); // < 0.32
    const auto reason = recognition::topologyRejectionReason(candidate, templateSummary);
    require(reason.empty(), "A template with topologyChangeScore below the minimum must never gate a candidate.");
}

void testTopologyInvalidSummariesBypassGate()
{
    measurements::HandTopologySummary invalidCandidate{};
    invalidCandidate.valid = false;
    const auto templateSummary = makeGenericTemplate();
    require(recognition::topologyRejectionReason(invalidCandidate, templateSummary).empty(),
        "An invalid (null-equivalent) candidate summary must not be gated.");

    measurements::HandTopologySummary invalidTemplate{};
    invalidTemplate.valid = false;
    const auto candidate = makeGenericTemplate();
    require(recognition::topologyRejectionReason(candidate, invalidTemplate).empty(),
        "An invalid (null-equivalent) template summary must not be gated.");
}

void testTopologyRejectsFingerStateMismatchOnStrongPalmTurnTemplate()
{
    auto templateSummary = makeGenericTemplate();
    templateSummary.palmTurnScore = 0.9F; // >= kMinimumPalmTurnScore (0.55)
    templateSummary.startFingerStateMask = 0b11111U;
    templateSummary.endFingerStateMask = 0b11111U;
    auto candidate = makeGenericTemplate();
    candidate.palmTurnScore = 0.9F;
    candidate.startFingerStateMask = 0b00000U; // mismatched vs template's 0b11111
    candidate.endFingerStateMask = 0b11111U;
    const auto reason = recognition::topologyRejectionReason(candidate, templateSummary);
    require(contains(reason, "Finger state mismatch"),
        "A palm-turn template must reject a candidate whose finger-state masks differ.");
}

void testTopologyRejectsWeakPalmTurn()
{
    auto templateSummary = makeGenericTemplate();
    templateSummary.palmTurnScore = 0.9F;
    templateSummary.startFingerStateMask = 0b11111U;
    templateSummary.endFingerStateMask = 0b11111U;
    auto candidate = makeGenericTemplate();
    candidate.startFingerStateMask = 0b11111U;
    candidate.endFingerStateMask = 0b11111U;
    candidate.palmTurnScore = 0.1F; // < template * 0.55
    const auto reason = recognition::topologyRejectionReason(candidate, templateSummary);
    require(contains(reason, "Palm turn too weak"), "A weak palm-turn candidate must be rejected.");
}

void testTopologyRejectsWhenAreaDidNotCrossSides()
{
    auto templateSummary = makeGenericTemplate();
    templateSummary.palmTurnScore = 0.9F;
    templateSummary.startFingerStateMask = 0b11111U;
    templateSummary.endFingerStateMask = 0b11111U;
    templateSummary.signedPalmAreaSignChanges = 1;
    auto candidate = makeGenericTemplate();
    candidate.startFingerStateMask = 0b11111U;
    candidate.endFingerStateMask = 0b11111U;
    candidate.palmTurnScore = 0.9F;
    candidate.signedPalmAreaSignChanges = 0;
    const auto reason = recognition::topologyRejectionReason(candidate, templateSummary);
    require(reason == "Palm area did not cross sides",
        "A palm-turn template that crosses sides must reject a candidate that never crosses.");
}

void testTopologyRejectsWeakCompressionDrop()
{
    auto templateSummary = makeGenericTemplate();
    templateSummary.palmTurnScore = 0.9F;
    templateSummary.startFingerStateMask = 0b11111U;
    templateSummary.endFingerStateMask = 0b11111U;
    templateSummary.palmCompressionDrop = 0.8F; // >= kMinimumPalmCompressionDrop (0.30)
    auto candidate = makeGenericTemplate();
    candidate.startFingerStateMask = 0b11111U;
    candidate.endFingerStateMask = 0b11111U;
    candidate.palmTurnScore = 0.9F;
    candidate.palmCompressionDrop = 0.1F; // < template * 0.55
    const auto reason = recognition::topologyRejectionReason(candidate, templateSummary);
    require(contains(reason, "Palm compression too small"),
        "A candidate with too little compression drop must be rejected against a compressing template.");
}

void testTopologyRejectsFingerFinalStateUnchanged()
{
    auto templateSummary = makeGenericTemplate();
    templateSummary.fingerStateTransitionCount = 3;
    templateSummary.startFingerStateMask = 0b11111U;
    templateSummary.endFingerStateMask = 0b00000U; // differs: grab-like
    auto candidate = makeGenericTemplate();
    candidate.fingerStateTransitionCount = 3;
    candidate.startFingerStateMask = 0b11111U;
    candidate.endFingerStateMask = 0b11111U; // unchanged
    const auto reason = recognition::topologyRejectionReason(candidate, templateSummary);
    require(contains(reason, "Finger final state did not change"),
        "A template whose finger state changes must reject a candidate whose finger state stays constant.");
}

void testTopologyRejectsStrictFinger01Mismatch()
{
    auto templateSummary = makeGenericTemplate();
    templateSummary.fingerStateTransitionCount = 3;
    templateSummary.startFingerStateMask = 0b11111U;
    templateSummary.endFingerStateMask = 0b00000U;
    auto candidate = makeGenericTemplate();
    candidate.fingerStateTransitionCount = 3;
    // Thumb/index (bits 0,1) differ at the start: strict mask 0b00011 distance > 0.
    candidate.startFingerStateMask = 0b11100U;
    candidate.endFingerStateMask = 0b00000U;
    const auto reason = recognition::topologyRejectionReason(candidate, templateSummary);
    require(contains(reason, "Finger 0/1 state mismatch"),
        "A thumb/index mismatch must be rejected even when the final state differs.");
}

void testTopologyRejectsGeneralFingerStateMismatch()
{
    auto templateSummary = makeGenericTemplate();
    templateSummary.fingerStateTransitionCount = 3;
    templateSummary.startFingerStateMask = 0b11111U;
    templateSummary.endFingerStateMask = 0b00000U;
    auto candidate = makeGenericTemplate();
    candidate.fingerStateTransitionCount = 3;
    candidate.startFingerStateMask = 0b11111U;
    // Middle/ring/pinky (bits 2,3,4) differ by 2 bits at the end: strict
    // mask 0b11100 distance > 1.
    candidate.endFingerStateMask = 0b01100U;
    const auto reason = recognition::topologyRejectionReason(candidate, templateSummary);
    require(contains(reason, "Finger state mismatch"),
        "A middle/ring/pinky mismatch beyond one bit must be rejected.");
}

void testTopologyRejectsWeakHandSizeChange()
{
    auto templateSummary = makeGenericTemplate();
    templateSummary.handScaleRatioRange = 0.5F; // >= kMinimumHandScaleRatioRange (0.22)
    templateSummary.handScaleRatioDelta = 0.5F;
    auto candidate = makeGenericTemplate();
    candidate.handScaleRatioRange = 0.05F; // < template * 0.55
    candidate.handScaleRatioDelta = 0.05F;
    const auto reason = recognition::topologyRejectionReason(candidate, templateSummary);
    require(contains(reason, "Hand size change too small"),
        "A candidate with too little scale-ratio range must be rejected against an approach/retreat template.");
}

void testTopologyRejectsOppositeHandSizeDirection()
{
    auto templateSummary = makeGenericTemplate();
    templateSummary.handScaleRatioRange = 0.5F;
    templateSummary.handScaleRatioDelta = 0.4F; // approaching (growing)
    auto candidate = makeGenericTemplate();
    candidate.handScaleRatioRange = 0.5F;
    candidate.handScaleRatioDelta = -0.4F; // retreating (shrinking): opposite sign
    const auto reason = recognition::topologyRejectionReason(candidate, templateSummary);
    require(contains(reason, "Hand size moved opposite direction"),
        "A candidate whose size change direction opposes the template must be rejected.");
}

void testTopologyRejectsWeakTranslation()
{
    auto templateSummary = makeGenericTemplate();
    templateSummary.translationDistance = 0.5F; // >= kMinimumTranslationDistance (0.22)
    templateSummary.translationDeltaX = 0.5F;
    templateSummary.translationDeltaY = 0.0F;
    auto candidate = makeGenericTemplate();
    candidate.translationDistance = 0.05F; // < template * 0.45
    candidate.translationDeltaX = 0.05F;
    candidate.translationDeltaY = 0.0F;
    const auto reason = recognition::topologyRejectionReason(candidate, templateSummary);
    require(contains(reason, "Translation too small"),
        "A candidate with too little translation must be rejected against a translating template.");
}

void testTopologyRejectsOppositeTranslationDirection()
{
    auto templateSummary = makeGenericTemplate();
    templateSummary.translationDistance = 0.5F;
    templateSummary.translationDeltaX = 1.0F;
    templateSummary.translationDeltaY = 0.0F;
    auto candidate = makeGenericTemplate();
    candidate.translationDistance = 0.6F;
    candidate.translationDeltaX = -1.0F; // opposite X direction
    candidate.translationDeltaY = 0.0F;
    const auto reason = recognition::topologyRejectionReason(candidate, templateSummary);
    require(contains(reason, "Translation moved opposite direction"),
        "A candidate translating opposite to the template's direction must be rejected.");
}

void testTopologyRejectsWeakOverallChange()
{
    auto templateSummary = makeGenericTemplate();
    templateSummary.topologyChangeScore = 1.0F;
    auto candidate = makeGenericTemplate();
    candidate.topologyChangeScore = 0.1F; // < template * 0.55
    const auto reason = recognition::topologyRejectionReason(candidate, templateSummary);
    require(contains(reason, "Topology change too small"),
        "A candidate with too little overall topology change must be rejected.");
}

void testTopologyRejectsWeakPalmTravel()
{
    auto templateSummary = makeGenericTemplate();
    templateSummary.palmTravel = 1.0F; // >= kMinimumPalmTravel (0.22)
    auto candidate = makeGenericTemplate();
    candidate.palmTravel = 0.05F; // < template * 0.45
    const auto reason = recognition::topologyRejectionReason(candidate, templateSummary);
    require(contains(reason, "Palm travel too small"),
        "A candidate with too little palm travel must be rejected against a traveling template.");
}

void testTopologyRejectsWeakPalmAngleChange()
{
    auto templateSummary = makeGenericTemplate();
    templateSummary.palmOrientationRangeRadians = 1.0F; // >= kMinimumPalmOrientationRangeRadians (0.45)
    auto candidate = makeGenericTemplate();
    candidate.palmOrientationRangeRadians = 0.05F; // < template * 0.50
    const auto reason = recognition::topologyRejectionReason(candidate, templateSummary);
    require(contains(reason, "Palm angle change too small"),
        "A candidate with too little palm rotation must be rejected against a rotating template.");
}

void testTopologyRejectsFingerTopologyUnchanged()
{
    auto templateSummary = makeGenericTemplate();
    templateSummary.fingerStateTransitionCount = 2;
    // Force every earlier gate to pass through so this is the deciding gate.
    templateSummary.startFingerStateMask = 0b00000U;
    templateSummary.endFingerStateMask = 0b00000U;
    auto candidate = makeGenericTemplate();
    candidate.fingerStateTransitionCount = 0;
    const auto reason = recognition::topologyRejectionReason(candidate, templateSummary);
    require(reason == "Finger topology did not change",
        "A candidate with zero finger-state transitions must be rejected against a template that has some.");
}

void testTopologyPassesWhenAllSignalsSatisfied()
{
    const auto templateSummary = makeGenericTemplate();
    const auto candidate = makeGenericTemplate(); // identical: every ratio gate trivially satisfied.
    const auto reason = recognition::topologyRejectionReason(candidate, templateSummary);
    require(reason.empty(), "A candidate matching the template's own topology must not be rejected.");
}

// ---------------------------------------------------------------------
// FeatureFrameDistance / score breakdown.
// ---------------------------------------------------------------------

void testFeatureFrameDistanceIdenticalFramesScoreZero()
{
    const auto frame = makeFrame(0.0);
    const auto breakdown = recognition::featureFrameDistance(frame, frame);
    requireNear(breakdown.totalScore, 0.0, 1.0e-6, "Identical frames must have zero total distance.");
    requireNear(breakdown.jointScore, 0.0, 1.0e-6, "Identical frames must have zero joint distance.");
}

void testFeatureFrameDistanceBreakdownSumMatchesWeightedTotal()
{
    auto candidate = makeFrame(0.0, 0.1F, 0.2F, 0.3F, 0.4F, 0.2F, 0.6F, 0.1F, 1.1F, 1.2F);
    candidate.values[measurements::kFingerStraightnessOffset] = 0.9F; // opens finger 0.
    auto templateFrame = makeFrame(100.0, -0.1F, -0.2F, -0.1F, 0.1F, -0.1F, 0.3F, 0.02F, 0.9F, 0.8F);
    const auto breakdown = recognition::featureFrameDistance(candidate, templateFrame);
    const double expectedTotal = (breakdown.jointScore * 0.18)
        + (breakdown.boneScore * 0.12)
        + (breakdown.curlScore * 0.08)
        + (breakdown.fingerStateScore * 0.20)
        + (breakdown.spacingScore * 0.04)
        + (breakdown.motionScore * 0.10)
        + (breakdown.palmTurnScore * 0.12)
        + (breakdown.depthScore * 0.04)
        + (breakdown.handednessScore * 0.06)
        + (breakdown.sizeScore * 0.04)
        + (breakdown.translationScore * 0.02);
    requireNear(breakdown.totalScore, expectedTotal, 1.0e-5,
        "totalScore must equal the documented weighted sum of its components.");
    require(breakdown.handednessScore == 0.0F, "handednessScore must stay 0 (dead in the ported PR algorithm).");
    require(breakdown.translationScore == 0.0F, "translationScore must stay 0 (dead in the ported PR algorithm).");
}

void testFeatureFrameDistancePerturbedLandmarksIncreaseJointScore()
{
    auto candidate = makeFrame(0.0);
    auto templateFrame = makeFrame(0.0);
    templateFrame.values[0] += 0.5F;
    templateFrame.values[1] += 0.5F;
    const auto breakdown = recognition::featureFrameDistance(candidate, templateFrame);
    require(breakdown.jointScore > 0.0F, "Perturbing landmark 0 must increase jointScore.");
    requireNear(breakdown.curlScore, 0.0, 1.0e-6, "Perturbing only landmarks must not change curlScore.");
}

void testFeatureFrameDistancePerturbedFingerStraightnessIncreasesFingerStateScore()
{
    auto candidate = makeFrame(0.0);
    auto templateFrame = makeFrame(0.0);
    templateFrame.values[measurements::kFingerStraightnessOffset] = 0.9F; // opens finger 0 in the template only.
    const auto breakdown = recognition::featureFrameDistance(candidate, templateFrame);
    require(breakdown.fingerStateScore > 0.0F,
        "A finger-straightness difference across the open threshold must increase fingerStateScore.");
}

void testFeatureFrameDistancePerturbedPalmTurnFeaturesIncreasePalmTurnScore()
{
    auto candidate = makeFrame(0.0, 0.0F, 0.0F, 0.0F, 0.0F, /*signedPalmArea=*/0.1F, /*palmCompression=*/0.5F);
    auto templateFrame = makeFrame(0.0, 0.0F, 0.0F, 0.0F, 0.0F, /*signedPalmArea=*/-0.3F, /*palmCompression=*/0.1F);
    const auto breakdown = recognition::featureFrameDistance(candidate, templateFrame);
    require(breakdown.palmTurnScore > 0.0F,
        "Perturbing signed palm area and compression must increase palmTurnScore.");
}

// ---------------------------------------------------------------------
// BoundedDtw: recurrence, path reconstruction, monotonicity, empty input.
// ---------------------------------------------------------------------

void testBoundedDtwIdenticalEqualLengthSequencesAreDiagonal()
{
    const auto sequence = makeBaselineSequence(32);
    const auto result = recognition::boundedDtw(sequence, sequence, 8);
    requireNear(result.score, 0.0, 1.0e-5, "Comparing a sequence to itself must score ~0.");
    require(result.path.size() == 32, "An identical equal-length comparison must produce a fully diagonal path.");
    require(result.path.front().candidateIndex == 0 && result.path.front().templateIndex == 0,
        "The DTW path must start at (0, 0).");
    require(result.path.back().candidateIndex == 31 && result.path.back().templateIndex == 31,
        "The DTW path must end at (n-1, m-1).");
    for (const auto& point : result.path)
    {
        require(point.candidateIndex == point.templateIndex,
            "A fully diagonal path must have matching candidate/template indices at every step.");
    }
}

void testBoundedDtwUnequalLengthsPathEndpointsAndMonotonicity()
{
    const auto candidate = makeBaselineSequence(10);
    const auto templateFrames = makeBaselineSequence(20);
    const auto result = recognition::boundedDtw(candidate, templateFrames, 15);
    require(!result.path.empty(), "A reachable band must produce a non-empty path.");
    require(result.path.front().candidateIndex == 0 && result.path.front().templateIndex == 0,
        "The DTW path must start at (0, 0) even for unequal lengths.");
    require(result.path.back().candidateIndex == 9 && result.path.back().templateIndex == 19,
        "The DTW path must end at (n-1, m-1) even for unequal lengths.");
    for (std::size_t i = 1; i < result.path.size(); ++i)
    {
        const auto& previous = result.path[i - 1];
        const auto& current = result.path[i];
        require(current.candidateIndex >= previous.candidateIndex && current.templateIndex >= previous.templateIndex,
            "Each DTW path step must be non-decreasing in both indices.");
        const auto candidateStep = current.candidateIndex - previous.candidateIndex;
        const auto templateStep = current.templateIndex - previous.templateIndex;
        require(candidateStep <= 1 && templateStep <= 1 && (candidateStep + templateStep) >= 1,
            "Each DTW path step must advance by at most one index per axis and at least one axis overall.");
    }
}

void testBoundedDtwEmptyInputReturnsMaxScore()
{
    const std::vector<UnifiedFeatureFrame> empty;
    const auto candidate = makeBaselineSequence(5);
    const auto resultBothEmpty = recognition::boundedDtw(empty, empty, 5);
    require(resultBothEmpty.score == (std::numeric_limits<float>::max)(),
        "An empty candidate and template must score float::max.");
    require(resultBothEmpty.path.empty(), "An empty comparison must produce an empty path.");

    const auto resultOneEmpty = recognition::boundedDtw(candidate, empty, 5);
    require(resultOneEmpty.score == (std::numeric_limits<float>::max)(),
        "An empty template against a non-empty candidate must score float::max.");
}

// ---------------------------------------------------------------------
// compareUnifiedSequences: end-to-end gates, warp ratio, reverse behavior,
// confidence threshold.
// ---------------------------------------------------------------------

void testCompareUnifiedSequencesIdenticalSequencesAreEligible()
{
    auto sequence = makeBaselineSequence(32);
    for (std::size_t i = 0; i < sequence.size(); ++i)
    {
        sequence[i].values[0] = static_cast<float>(i * i) * 0.001F;
    }
    const auto result = recognition::compareUnifiedSequences(sequence, sequence);
    require(result.eligible, "An identical candidate/template pair must be eligible.");
    require(result.reason == "Eligible", "An eligible comparison must report the 'Eligible' reason.");
    requireNear(result.score, 0.0, 1.0e-5, "An identical comparison must score ~0.");
    requireNear(result.warpRatio, 1.0, 1.0e-6, "An identical equal-length comparison must warp 1:1.");
    require(recognition::scoreToConfidence(result.score) >= recognition::kConfidenceThreshold,
        "A ~0 score must convert to a confidence at or above the threshold.");
}

void testCompareUnifiedSequencesTooFewFramesRejected()
{
    const auto shortSequence = makeBaselineSequence(2);
    const auto longSequence = makeBaselineSequence(32);
    const auto result = recognition::compareUnifiedSequences(shortSequence, longSequence);
    require(!result.eligible, "Fewer than 3 frames must never be eligible.");
    require(result.reason == "Too few feature frames", "The too-few-frames rejection reason must be exact.");
    require(result.score == (std::numeric_limits<float>::max)(), "A too-few-frames rejection must score float::max.");
    require(!result.hasWarpRatio, "A too-few-frames rejection must not report a warp ratio.");
}

void testCompareUnifiedSequencesEmptyInputRejected()
{
    const std::vector<UnifiedFeatureFrame> empty;
    const auto result = recognition::compareUnifiedSequences(empty, empty);
    require(!result.eligible, "Empty input must never be eligible.");
    require(result.score == (std::numeric_limits<float>::max)(), "Empty input must score float::max.");
}

void testCompareUnifiedSequencesOppositeMotionDirectionRejected()
{
    // Endpoint-to-endpoint displacement is opposite (candidate net +0.3,
    // template net -0.3), but each sequence's single nonzero step occurs at
    // a different index (candidate at index 1, template at index 31), so
    // temporalDirectionDot's index-matched cross term stays ~0 (its
    // per-index energy terms are individually well above the 0.02 floor,
    // but never coincide) and does not itself dip below -0.18. This isolates
    // HasOppositeMotionDirection (the endpoint-displacement gate) rather
    // than the temporal-direction-dot gate, which a simple monotonic
    // opposite ramp would trigger first instead.
    std::vector<UnifiedFeatureFrame> candidate;
    std::vector<UnifiedFeatureFrame> templateFrames;
    for (int i = 0; i < 32; ++i)
    {
        candidate.push_back(makeFrame(i * 80.0, i >= 1 ? 0.3F : 0.0F, 0.0F));
        templateFrames.push_back(makeFrame(i * 80.0, i <= 30 ? 0.3F : 0.0F, 0.0F));
    }
    const auto result = recognition::compareUnifiedSequences(candidate, templateFrames);
    require(!result.eligible, "Opposite net motion direction must be rejected.");
    require(result.reason == "Opposite motion direction", "The opposite-motion rejection reason must be exact.");
}

void testCompareUnifiedSequencesWarpRatioStaysBelowLimitEvenUnderMaximalZigzag()
{
    // The Sakoe-Chiba DTW path length is bounded by n + m - 1, so
    // warpRatio = pathLength / max(n, m) <= (n + m - 1) / max(n, m) < 2 for
    // any n, m > 0 - strictly below kWarpRatioLimit (2.8). This test
    // documents that the ported ceiling, while preserved exactly from the
    // PR, cannot be crossed by the DTW recurrence as implemented; it asserts
    // the bound holds even for a deliberately maximal-zigzag-inducing
    // mismatched-length pair rather than asserting the (unreachable)
    // rejection branch.
    // Both sequences ramp landmark 0's x-value monotonically in the same
    // direction (a feature the temporal/motion pre-gates never look at, so
    // they cannot fire here), so a forward alignment fits reasonably and
    // the reverse-fits-better gate does not preempt reaching the warp-ratio
    // check.
    std::vector<UnifiedFeatureFrame> candidate;
    for (int i = 0; i < 6; ++i)
    {
        auto frame = makeFrame(i * 80.0);
        frame.values[0] = static_cast<float>(i) * 0.1F;
        candidate.push_back(frame);
    }
    std::vector<UnifiedFeatureFrame> templateFrames;
    for (int i = 0; i < 40; ++i)
    {
        auto frame = makeFrame(i * 20.0);
        frame.values[0] = static_cast<float>(i) * (0.5F / 39.0F);
        templateFrames.push_back(frame);
    }
    const auto result = recognition::compareUnifiedSequences(candidate, templateFrames);
    require(result.hasWarpRatio, "A comparison reaching the warp-ratio stage must report one.");
    require(result.warpRatio < recognition::kWarpRatioLimit,
        "warpRatio must stay under kWarpRatioLimit given the DTW path-length bound.");
}

void testCompareUnifiedSequencesReversedTemplateFitsBetterIsRejected()
{
    // Candidate has a short, self-contained "hump" in one finger's
    // straightness confined to indices [3,6), flat baseline elsewhere;
    // template is the exact time-reverse of candidate, so its hump lands at
    // indices [26,29). The hump windows do not overlap and every other
    // feature is held constant, so temporalDirectionDot's cross term stays
    // ~0 (not the coarse opposite-direction gate) while a forward DTW -
    // constrained by the Sakoe-Chiba band - cannot warp a hump 23 frames
    // without exceeding the band, so it mismatches on both hump regions.
    // The exact reverse-of-template comparison is a perfect self-match.
    std::vector<UnifiedFeatureFrame> candidate;
    for (int i = 0; i < 32; ++i)
    {
        auto frame = makeFrame(i * 80.0);
        if (i >= 3 && i < 6)
        {
            frame.values[measurements::kFingerStraightnessOffset] = 0.3F;
        }
        candidate.push_back(frame);
    }
    std::vector<UnifiedFeatureFrame> templateFrames(candidate.rbegin(), candidate.rend());

    const auto result = recognition::compareUnifiedSequences(candidate, templateFrames);
    require(!result.eligible, "A candidate whose reverse fits far better must not be eligible.");
    require(contains(result.reason, "Reversed time direction fits better"),
        "The rejection reason must identify the reversed-fits-better gate.");
}

void testCompareUnifiedSequencesBelowThresholdIneligible()
{
    auto candidate = makeBaselineSequence(32);
    auto templateFrames = makeBaselineSequence(32);
    // Offset every landmark of every candidate frame by the same constant:
    // every candidate frame stays identical to every other (so forward and
    // time-reversed DTW score identically, never triggering the reverse
    // gate), while the large, uniform joint-distance gap guarantees the DTW
    // score lands well above the eligibility threshold (0.162).
    for (auto& frame : candidate)
    {
        for (std::size_t i = 0; i < (measurements::kLandmarkCount * 2); ++i)
        {
            frame.values[i] = 2.0F;
        }
    }
    const auto result = recognition::compareUnifiedSequences(candidate, templateFrames);
    require(!result.eligible, "A large, consistent joint offset must push the score below eligibility.");
    require(result.reason == "Below unified threshold", "The below-threshold rejection reason must be exact.");
}

// ---------------------------------------------------------------------
// scoreToConfidence / confidenceToScore round trip.
// ---------------------------------------------------------------------

void testScoreConfidenceRoundTrip()
{
    requireNear(recognition::scoreToConfidence(0.0F), 1.0, 1.0e-6, "A zero score must be full confidence.");
    requireNear(recognition::scoreToConfidence(0.9F), 0.0, 1.0e-6, "A 0.9 score must be zero confidence.");
    requireNear(recognition::confidenceToScore(recognition::kConfidenceThreshold),
        0.9 * (1.0 - recognition::kConfidenceThreshold), 1.0e-6,
        "confidenceToScore must invert scoreToConfidence's linear mapping.");
    requireNear(recognition::scoreToConfidence(recognition::confidenceToScore(recognition::kConfidenceThreshold)),
        recognition::kConfidenceThreshold, 1.0e-5, "The score/confidence mapping must round-trip.");
}

// ---------------------------------------------------------------------
// compareCandidateToTemplate: consumes GestureCandidateWindow + HandTopologySummary directly.
// ---------------------------------------------------------------------

measurements::GestureCandidateWindow makeCandidateWindow(const std::vector<UnifiedFeatureFrame>& frames)
{
    measurements::GestureCandidateWindow window{};
    window.valid = true;
    window.durationMs = 2000.0;
    window.sourceFrameCount = frames.size();
    window.usableFrameCount = frames.size();
    for (std::size_t i = 0; i < measurements::kUnifiedSequenceLength && i < frames.size(); ++i)
    {
        window.unifiedSequence[i] = frames[i];
    }
    return window;
}

void testCompareCandidateToTemplateShortCircuitsOnTopologyRejection()
{
    auto frames = makeBaselineSequence(measurements::kUnifiedSequenceLength);
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        frames[i].values[0] = static_cast<float>(i * i) * 0.001F;
    }
    auto window = makeCandidateWindow(frames);

    recognition::UnifiedSequenceTemplate templateSequence{};
    for (std::size_t i = 0; i < measurements::kUnifiedSequenceLength; ++i)
    {
        templateSequence.frames[i] = frames[i];
    }
    templateSequence.topology = makeGenericTemplate();
    templateSequence.topology.fingerStateTransitionCount = 2;
    templateSequence.topology.startFingerStateMask = 0b00000U;
    templateSequence.topology.endFingerStateMask = 0b00000U;

    window.topology = makeGenericTemplate();
    window.topology.fingerStateTransitionCount = 0; // triggers "Finger topology did not change"

    const auto outcome = recognition::compareCandidateToTemplate(window, templateSequence);
    require(!outcome.topologyRejectionReason.empty(), "A topology-rejected candidate must report a rejection reason.");
    require(!outcome.comparison.eligible, "A topology-rejected outcome must not be eligible.");
    require(outcome.comparison.score == (std::numeric_limits<float>::max)(),
        "A topology-rejected outcome must carry the float::max sentinel score, mirroring the PR's synthetic "
        "GestureTemplateComparison.");
    require(outcome.comparison.path.empty(),
        "A topology-rejected outcome must never have run DTW (empty path).");
}

void testCompareCandidateToTemplateRunsDtwWhenTopologyPasses()
{
    auto frames = makeBaselineSequence(measurements::kUnifiedSequenceLength);
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        frames[i].values[0] = static_cast<float>(i * i) * 0.001F;
    }
    auto window = makeCandidateWindow(frames);

    recognition::UnifiedSequenceTemplate templateSequence{};
    for (std::size_t i = 0; i < measurements::kUnifiedSequenceLength; ++i)
    {
        templateSequence.frames[i] = frames[i];
    }
    templateSequence.topology = makeGenericTemplate();
    window.topology = makeGenericTemplate();

    const auto outcome = recognition::compareCandidateToTemplate(window, templateSequence);
    require(outcome.topologyRejectionReason.empty(), "A matching topology must pass the gate.");
    require(outcome.comparison.eligible, "An identical unified sequence must be eligible once topology passes.");
}

// ---------------------------------------------------------------------
// Track-independent candidate inputs: the comparator is pure/stateless, so
// interleaving comparisons for two unrelated "tracks" must not let one
// call's input affect another's result.
// ---------------------------------------------------------------------

void testComparatorIsStatelessAcrossInterleavedTrackInputs()
{
    const auto trackASequence = makeBaselineSequence(32, 80.0);
    std::vector<UnifiedFeatureFrame> trackBSequence;
    for (int i = 0; i < 32; ++i)
    {
        trackBSequence.push_back(makeFrame(i * 50.0, static_cast<float>(i) * 0.02F, 0.0F));
    }

    const auto isolatedA = recognition::compareUnifiedSequences(trackASequence, trackASequence);
    const auto isolatedB = recognition::compareUnifiedSequences(trackBSequence, trackBSequence);

    // Interleave calls for the two "tracks" as a recognizer visiting multiple
    // active hands would.
    const auto interleavedA1 = recognition::compareUnifiedSequences(trackASequence, trackASequence);
    const auto interleavedB1 = recognition::compareUnifiedSequences(trackBSequence, trackBSequence);
    const auto interleavedA2 = recognition::compareUnifiedSequences(trackASequence, trackASequence);
    const auto interleavedB2 = recognition::compareUnifiedSequences(trackBSequence, trackBSequence);

    requireNear(interleavedA1.score, isolatedA.score, 1.0e-9, "Track A's result must not depend on call order.");
    requireNear(interleavedA2.score, isolatedA.score, 1.0e-9, "Track A's result must be repeatable across calls.");
    requireNear(interleavedB1.score, isolatedB.score, 1.0e-9, "Track B's result must not depend on call order.");
    requireNear(interleavedB2.score, isolatedB.score, 1.0e-9, "Track B's result must be repeatable across calls.");
    require(interleavedA1.eligible == isolatedA.eligible && interleavedB1.eligible == isolatedB.eligible,
        "Eligibility must not depend on interleaving with another track's comparisons.");
}
}

int main()
{
    try
    {
        testTopologyLowChangeTemplateBypassesAllGates();
        testTopologyInvalidSummariesBypassGate();
        testTopologyRejectsFingerStateMismatchOnStrongPalmTurnTemplate();
        testTopologyRejectsWeakPalmTurn();
        testTopologyRejectsWhenAreaDidNotCrossSides();
        testTopologyRejectsWeakCompressionDrop();
        testTopologyRejectsFingerFinalStateUnchanged();
        testTopologyRejectsStrictFinger01Mismatch();
        testTopologyRejectsGeneralFingerStateMismatch();
        testTopologyRejectsWeakHandSizeChange();
        testTopologyRejectsOppositeHandSizeDirection();
        testTopologyRejectsWeakTranslation();
        testTopologyRejectsOppositeTranslationDirection();
        testTopologyRejectsWeakOverallChange();
        testTopologyRejectsWeakPalmTravel();
        testTopologyRejectsWeakPalmAngleChange();
        testTopologyRejectsFingerTopologyUnchanged();
        testTopologyPassesWhenAllSignalsSatisfied();
        testFeatureFrameDistanceIdenticalFramesScoreZero();
        testFeatureFrameDistanceBreakdownSumMatchesWeightedTotal();
        testFeatureFrameDistancePerturbedLandmarksIncreaseJointScore();
        testFeatureFrameDistancePerturbedFingerStraightnessIncreasesFingerStateScore();
        testFeatureFrameDistancePerturbedPalmTurnFeaturesIncreasePalmTurnScore();
        testBoundedDtwIdenticalEqualLengthSequencesAreDiagonal();
        testBoundedDtwUnequalLengthsPathEndpointsAndMonotonicity();
        testBoundedDtwEmptyInputReturnsMaxScore();
        testCompareUnifiedSequencesIdenticalSequencesAreEligible();
        testCompareUnifiedSequencesTooFewFramesRejected();
        testCompareUnifiedSequencesEmptyInputRejected();
        testCompareUnifiedSequencesOppositeMotionDirectionRejected();
        testCompareUnifiedSequencesWarpRatioStaysBelowLimitEvenUnderMaximalZigzag();
        testCompareUnifiedSequencesReversedTemplateFitsBetterIsRejected();
        testCompareUnifiedSequencesBelowThresholdIneligible();
        testScoreConfidenceRoundTrip();
        testCompareCandidateToTemplateShortCircuitsOnTopologyRejection();
        testCompareCandidateToTemplateRunsDtwWhenTopologyPasses();
        testComparatorIsStatelessAcrossInterleavedTrackInputs();
        std::cout << "UnifiedSequenceGestureComparator tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}

#include "HandInput/Measurements/hand_measurement_extractor.h"
#include "HandInput/Measurements/hand_unified_feature_frame.h"
#include "HandPerception/MediaPipeGraph/hand_landmark_graph.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
using ryoiki::hand_perception::HandLandmarkResult;
using ryoiki::hand_perception::Landmark3f;
namespace measurements = ryoiki::hand_input::measurements;

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

// Order matches kFingerBases/kFingerTips: thumb, index, middle, ring, pinky.
// The middle-finger direction (index 2) doubles as the wrist-relative
// orientation anchor (landmark 9), exactly as in the PR prototype.
constexpr std::array<Landmark3f, 5> kFingerBaseOffsets{{
    {-30.0F, -20.0F, 0.0F},
    {40.0F, -70.0F, 0.0F},
    {0.0F, -100.0F, 0.0F},
    {-40.0F, -70.0F, 0.0F},
    {-70.0F, -40.0F, 0.0F}}};
constexpr std::array<std::size_t, 5> kBaseIndices{1, 5, 9, 13, 17};
constexpr std::array<std::size_t, 5> kTipIndices{4, 8, 12, 16, 20};
constexpr Landmark3f kWrist{300.0F, 450.0F, 0.0F};

// Builds a synthetic 21-landmark hand in upright-image pixel space. Each
// finger's tip sits at wrist + baseOffset * multiplier, so multiplier == 1
// puts the tip on top of its base (closed/curled) and multiplier == 2 puts
// it twice as far from the wrist as the base (straight/open); see
// AnalyzeFingerPose's ratio threshold (1.05..1.70 maps to 0..1). zSpread
// gives each finger a distinct, small tip Z so depth-range checks are
// non-degenerate.
std::array<Landmark3f, 21> makeHandLandmarks(
    const std::array<float, 5>& multipliers, const float zSpread = 0.0F)
{
    std::array<Landmark3f, 21> landmarks{};
    landmarks[0] = kWrist;
    for (std::size_t finger = 0; finger < 5; ++finger)
    {
        const Landmark3f base{
            kWrist.x + kFingerBaseOffsets[finger].x,
            kWrist.y + kFingerBaseOffsets[finger].y,
            0.0F};
        const Landmark3f tip{
            kWrist.x + kFingerBaseOffsets[finger].x * multipliers[finger],
            kWrist.y + kFingerBaseOffsets[finger].y * multipliers[finger],
            zSpread * (static_cast<float>(finger) - 2.0F)};
        landmarks[kBaseIndices[finger]] = base;
        landmarks[kTipIndices[finger]] = tip;
        for (std::size_t joint = 1; joint <= 2; ++joint)
        {
            const float f = static_cast<float>(joint) / 3.0F;
            landmarks[kBaseIndices[finger] + joint] = {
                base.x + (tip.x - base.x) * f,
                base.y + (tip.y - base.y) * f,
                base.z + (tip.z - base.z) * f};
        }
    }
    return landmarks;
}

constexpr std::array<float, 5> kOpenHandMultipliers{2.0F, 2.0F, 2.0F, 2.0F, 2.0F};
constexpr std::array<float, 5> kFistMultipliers{1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
// thumb closed, index+middle open, ring+pinky closed.
constexpr std::array<float, 5> kMixedMultipliers{1.0F, 2.0F, 2.0F, 1.0F, 1.0F};

measurements::UnifiedFeatureObservation makeObservation(
    const std::array<float, 5>& multipliers,
    const std::uint64_t timestampUs,
    const float handedness = 0.8F,
    const float boundingBoxArea = 0.0F,
    const float zSpread = 0.0F)
{
    measurements::UnifiedFeatureObservation observation{};
    observation.timestampUs = timestampUs;
    observation.imageLandmarks = makeHandLandmarks(multipliers, zSpread);
    observation.handedness = handedness;
    observation.boundingBoxArea = boundingBoxArea;
    return observation;
}

void testFeatureVectorLayoutConstants()
{
    using namespace measurements;
    require(kUnifiedFeatureVectorLength == 64, "Feature vector length drifted from the PR layout.");
    require(kFingerStraightnessOffset == 51, "Finger-straightness offset drifted from the PR layout.");
    require(kFingerStraightnessLength == 5, "Finger-straightness length drifted from the PR layout.");
    require(kHandednessOffset == 58, "Handedness offset drifted from the PR layout.");
    require(kSignedPalmAreaOffset == 59, "Signed-palm-area offset drifted from the PR layout.");
    require(kPalmCompressionOffset == 60, "Palm-compression offset drifted from the PR layout.");
    require(kPalmDepthRangeOffset == 61, "Palm-depth-range offset drifted from the PR layout.");
    require(kHandScaleRatioOffset == 62, "Hand-scale-ratio offset drifted from the PR layout.");
    require(kBoundingBoxAreaRatioOffset == 63, "Bounding-box-area-ratio offset drifted from the PR layout.");
    require(kUnifiedSequenceLength == 32, "Unified sequence length drifted from the PR prototype's 32.");

    const auto frame = buildFeatureFrame(makeObservation(kOpenHandMultipliers, 0));
    require(frame.values.size() == kUnifiedFeatureVectorLength,
        "buildFeatureFrame did not produce the documented flat vector length.");
}

void testBuildFeatureFrameLandmarkNormalizationIsDimensionless()
{
    const auto frame = measurements::buildFeatureFrame(makeObservation(kOpenHandMultipliers, 0));
    // Landmark 0 is the wrist itself: wrist-relative and therefore always at
    // the local origin.
    requireNear(frame.values[0], 0.0, 1.0e-5, "Wrist landmark did not normalize to the local origin.");
    requireNear(frame.values[1], 0.0, 1.0e-5, "Wrist landmark did not normalize to the local origin.");
    // Landmark 9 (the orientation anchor) always rotates onto the +X axis at
    // unit distance, by construction of the rotation-normalization.
    requireNear(frame.values[9 * 2], 1.0, 1.0e-5,
        "Orientation-anchor landmark did not normalize onto the +X axis.");
    requireNear(frame.values[9 * 2 + 1], 0.0, 1.0e-5,
        "Orientation-anchor landmark did not normalize onto the +X axis.");
    requireNear(frame.palmOrientationRadians, -1.5707963, 1.0e-4,
        "Palm orientation did not match the wrist-to-landmark-9 direction.");
}

void testAnalyzeFingerPoseDiscreteStates()
{
    using measurements::analyzeFingerPose;
    const auto open = analyzeFingerPose(makeHandLandmarks(kOpenHandMultipliers));
    require(open.stateMask == 0b1'1111U, "A fully open hand did not report all five fingers extended.");
    for (const float value : open.straightness)
    {
        require(value >= measurements::kFingerOpenThreshold, "An open finger reported low straightness.");
    }

    const auto fist = analyzeFingerPose(makeHandLandmarks(kFistMultipliers));
    require(fist.stateMask == 0U, "A closed fist reported an extended finger.");
    for (const float value : fist.straightness)
    {
        require(value < measurements::kFingerOpenThreshold, "A curled finger reported high straightness.");
    }

    const auto mixed = analyzeFingerPose(makeHandLandmarks(kMixedMultipliers));
    require(mixed.stateMask == 0b0'0110U,
        "A hand with only index and middle extended did not report the expected finger-state mask.");
}

void testBuildFeatureFrameMatchesHandMeasurementExtractorTopology()
{
    using measurements::HandMeasurementExtractor;

    HandLandmarkResult hand{};
    hand.detected = true;
    hand.confidence = 0.9F;
    hand.handedness = 0.8F;
    hand.landmarks = makeHandLandmarks(kOpenHandMultipliers, 0.02F);

    HandMeasurementExtractor extractor;
    const auto measured = extractor.extract(hand, 1, 1'000'000, 640, 480);
    require(measured.topology.valid, "Extractor rejected a well-formed synthetic hand.");

    const auto observation = measurements::makeUnifiedFeatureObservation(hand, 1'000'000);
    const auto frame = measurements::buildFeatureFrame(observation);

    requireNear(frame.palmOrientationRadians, measured.topology.palmAxisRadians, 1.0e-5,
        "Unified-frame orientation diverged from HandTopologyMeasurement::palmAxisRadians.");
    requireNear(frame.values[measurements::kHandednessOffset], measured.topology.handedness, 1.0e-6,
        "Unified-frame handedness diverged from HandTopologyMeasurement::handedness.");
    requireNear(frame.values[measurements::kSignedPalmAreaOffset], measured.topology.signedPalmArea, 1.0e-5,
        "Unified-frame signed palm area diverged from HandTopologyMeasurement::signedPalmArea.");
    requireNear(frame.values[measurements::kPalmCompressionOffset], measured.topology.palmCompression, 1.0e-5,
        "Unified-frame palm compression diverged from HandTopologyMeasurement::palmCompression.");
    requireNear(frame.values[measurements::kPalmDepthRangeOffset], measured.topology.palmDepthRange, 1.0e-5,
        "Unified-frame palm depth range diverged from HandTopologyMeasurement::palmDepthRange.");
    require(measured.topology.palmDepthRange > 0.0F,
        "Test fixture did not exercise a non-degenerate palm depth range.");

    const auto pose = measurements::analyzeFingerPose(hand.landmarks);
    require(pose.stateMask == measured.topology.fingerStateMask,
        "analyzeFingerPose diverged from HandTopologyMeasurement::fingerStateMask.");
    for (std::size_t finger = 0; finger < pose.straightness.size(); ++finger)
    {
        requireNear(pose.straightness[finger], measured.topology.fingerStraightness[finger], 1.0e-5,
            "analyzeFingerPose diverged from HandTopologyMeasurement::fingerStraightness.");
        requireNear(frame.values[measurements::kFingerStraightnessOffset + finger],
            measured.topology.fingerStraightness[finger], 1.0e-5,
            "Unified-frame straightness slot diverged from HandTopologyMeasurement::fingerStraightness.");
    }
}

void testBuildFeatureSequenceEmptyAndSingleObservation()
{
    require(measurements::buildFeatureSequence({}).empty(),
        "buildFeatureSequence did not return empty output for empty input.");

    std::vector<measurements::UnifiedFeatureObservation> single{
        makeObservation(kOpenHandMultipliers, 5'000'000, 0.8F, 1200.0F)};
    const auto frames = measurements::buildFeatureSequence(single);
    require(frames.size() == 1, "A single observation did not produce a single feature frame.");
    requireNear(frames[0].timeOffsetMs, 0.0, 1.0e-9, "The only frame in a sequence must start at time zero.");
    requireNear(frames[0].centerX, 0.0, 1.0e-6, "The only frame in a sequence must have zero translation.");
    requireNear(frames[0].centerY, 0.0, 1.0e-6, "The only frame in a sequence must have zero translation.");
    requireNear(frames[0].palmVelocity, 0.0, 1.0e-6, "The only frame in a sequence must have zero velocity.");
    requireNear(frames[0].values[measurements::kHandScaleRatioOffset], 1.0, 1.0e-6,
        "A single-frame sequence's hand-scale ratio must be the identity value.");
    requireNear(frames[0].values[measurements::kBoundingBoxAreaRatioOffset], 1.0, 1.0e-6,
        "A single-frame sequence's bounding-box-area ratio must be the identity value.");
}

void testBuildFeatureSequenceTranslationVelocityAndRatios()
{
    // Three observations of a hand at a fixed scale (palm-axis length 100)
    // that steps the wrist by (20, 0) px every 100 ms, and whose bounding
    // box area doubles by the third frame.
    std::vector<measurements::UnifiedFeatureObservation> observations;
    for (int i = 0; i < 3; ++i)
    {
        auto observation = makeObservation(
            kOpenHandMultipliers, static_cast<std::uint64_t>(i) * 100'000, 0.8F,
            i == 2 ? 2000.0F : 1000.0F);
        for (auto& landmark : observation.imageLandmarks)
        {
            landmark.x += static_cast<float>(i) * 20.0F;
        }
        observations.push_back(observation);
    }

    const auto frames = measurements::buildFeatureSequence(observations);
    require(frames.size() == 3, "buildFeatureSequence dropped observations.");

    requireNear(frames[0].centerX, 0.0, 1.0e-5, "First frame of a sequence must have zero cumulative translation.");
    requireNear(frames[0].palmVelocity, 0.0, 1.0e-5, "First frame of a sequence must have zero velocity.");
    // Wrist step is (20, 0) px over a palm-axis scale of 100 px, so each step
    // contributes 0.2 dimensionless units of +X translation.
    requireNear(frames[1].centerX, 0.2, 1.0e-4, "Cumulative translation after one step was incorrect.");
    requireNear(frames[1].centerY, 0.0, 1.0e-4, "Cumulative Y translation should stay zero for a pure-X step.");
    requireNear(frames[2].centerX, 0.4, 1.0e-4, "Cumulative translation after two steps was incorrect.");
    // 0.2 dimensionless units over 0.1 s = 2.0 units/s.
    requireNear(frames[1].palmVelocity, 2.0, 1.0e-3, "Per-step velocity was incorrect.");
    requireNear(frames[2].palmVelocity, 2.0, 1.0e-3, "Per-step velocity was incorrect.");

    requireNear(frames[0].values[measurements::kHandScaleRatioOffset], 1.0, 1.0e-5,
        "The first frame's hand-scale ratio must be the identity value.");
    requireNear(frames[0].values[measurements::kBoundingBoxAreaRatioOffset], 1.0, 1.0e-5,
        "The first frame's bounding-box-area ratio must be the identity value.");
    requireNear(frames[2].values[measurements::kBoundingBoxAreaRatioOffset], 2.0, 1.0e-4,
        "Bounding-box-area ratio did not track the doubled area.");
}

void testBuildFeatureSequenceIsPureAcrossInterleavedTracks()
{
    // If this builder is later invoked once per tracked hand inside
    // MultiHandMeasurementStage, each track's sequence must be unaffected by
    // any other track's calls. Since buildFeatureSequence carries no shared
    // or static state, interleaving calls for two independent "tracks" must
    // reproduce identical per-track results.
    std::vector<measurements::UnifiedFeatureObservation> trackA;
    std::vector<measurements::UnifiedFeatureObservation> trackB;
    for (int i = 0; i < 4; ++i)
    {
        auto a = makeObservation(kOpenHandMultipliers, static_cast<std::uint64_t>(i) * 50'000, 0.9F);
        for (auto& landmark : a.imageLandmarks) landmark.x += static_cast<float>(i) * 5.0F;
        trackA.push_back(a);

        auto b = makeObservation(kFistMultipliers, static_cast<std::uint64_t>(i) * 80'000, 0.1F);
        for (auto& landmark : b.imageLandmarks) landmark.y += static_cast<float>(i) * 30.0F;
        trackB.push_back(b);
    }

    const auto firstA = measurements::buildFeatureSequence(trackA);
    const auto firstB = measurements::buildFeatureSequence(trackB);
    const auto secondA = measurements::buildFeatureSequence(trackA);

    require(firstA.size() == secondA.size(), "Interleaved calls changed track A's frame count.");
    for (std::size_t i = 0; i < firstA.size(); ++i)
    {
        requireNear(firstA[i].centerX, secondA[i].centerX, 1.0e-9,
            "Track B's call perturbed track A's translation.");
        requireNear(firstA[i].palmVelocity, secondA[i].palmVelocity, 1.0e-9,
            "Track B's call perturbed track A's velocity.");
        for (std::size_t v = 0; v < measurements::kUnifiedFeatureVectorLength; ++v)
        {
            requireNear(firstA[i].values[v], secondA[i].values[v], 1.0e-9,
                "Track B's call perturbed one of track A's feature values.");
        }
    }
    // Sanity: the two tracks are genuinely distinct (different handedness
    // slot and different finger-state mask), not accidentally aliased.
    require(firstA[0].values[measurements::kHandednessOffset]
            != firstB[0].values[measurements::kHandednessOffset],
        "Track A and track B unexpectedly shared handedness.");
    require(measurements::readFingerStateMask(firstA[0].values)
            != measurements::readFingerStateMask(firstB[0].values),
        "Track A (open) and track B (fist) unexpectedly shared a finger-state mask.");
}

void testInterpolateAngleWrapsShortestPath()
{
    measurements::UnifiedFeatureFrame a{};
    a.palmOrientationRadians = 3.0F;
    measurements::UnifiedFeatureFrame b{};
    b.palmOrientationRadians = -3.0F;
    const auto mid = measurements::interpolate(a, b, 0.0, 0.5F);
    // The shortest path from 3.0 to -3.0 rad crosses +-pi, landing near
    // +-pi at the midpoint, not near zero (the naive linear-blend result).
    requireNear(std::abs(mid.palmOrientationRadians), 3.14159265, 0.02,
        "Orientation interpolation did not take the shortest angular path across the +-pi wrap.");
}

void testInterpolateLerpsTranslationScaleAndArea()
{
    measurements::UnifiedFeatureFrame a{};
    a.centerX = 0.0F;
    a.centerY = 0.0F;
    a.palmVelocity = 1.0F;
    a.values[measurements::kHandScaleRatioOffset] = 1.0F;
    a.values[measurements::kBoundingBoxAreaRatioOffset] = 1.0F;

    measurements::UnifiedFeatureFrame b{};
    b.centerX = 10.0F;
    b.centerY = -4.0F;
    b.palmVelocity = 3.0F;
    b.values[measurements::kHandScaleRatioOffset] = 2.0F;
    b.values[measurements::kBoundingBoxAreaRatioOffset] = 4.0F;

    const auto result = measurements::interpolate(a, b, 123.0, 0.25F);
    requireNear(result.timeOffsetMs, 123.0, 1.0e-9, "Interpolate did not adopt the requested time offset.");
    requireNear(result.centerX, 2.5, 1.0e-5, "Translation X did not interpolate linearly.");
    requireNear(result.centerY, -1.0, 1.0e-5, "Translation Y did not interpolate linearly.");
    requireNear(result.palmVelocity, 1.5, 1.0e-5, "Velocity did not interpolate linearly.");
    requireNear(result.values[measurements::kHandScaleRatioOffset], 1.25, 1.0e-5,
        "Hand-scale ratio did not interpolate linearly.");
    requireNear(result.values[measurements::kBoundingBoxAreaRatioOffset], 1.75, 1.0e-5,
        "Bounding-box-area ratio did not interpolate linearly.");
}

void testInterpolateRecomputesDiscreteFingerStateFromInterpolatedValues()
{
    measurements::UnifiedFeatureFrame a{};
    a.values[measurements::kFingerStraightnessOffset] = 0.30F; // closed
    measurements::UnifiedFeatureFrame b{};
    b.values[measurements::kFingerStraightnessOffset] = 0.90F; // open

    const auto low = measurements::interpolate(a, b, 0.0, 0.3F); // 0.30 + 0.6*0.3 = 0.48
    require((measurements::readFingerStateMask(low.values) & 1U) == 0U,
        "Finger state opened before the interpolated straightness crossed the threshold.");
    const auto high = measurements::interpolate(a, b, 0.0, 0.6F); // 0.30 + 0.6*0.6 = 0.66
    require((measurements::readFingerStateMask(high.values) & 1U) == 1U,
        "Finger state did not open once the interpolated straightness crossed the threshold.");
}

void testResampleEmptyAndZeroLength()
{
    require(measurements::resample({}, measurements::kUnifiedSequenceLength).empty(),
        "Resampling empty input must return empty output.");
    std::vector<measurements::UnifiedFeatureFrame> frames(3);
    require(measurements::resample(frames, 0).empty(),
        "Resampling to zero length must return empty output.");
}

void testResampleSingleFrameRepeatsUnchanged()
{
    measurements::UnifiedFeatureFrame source{};
    source.timeOffsetMs = 42.0;
    source.centerX = 7.0F;
    source.values[5] = 1.25F;

    const auto result = measurements::resample({source}, 6);
    require(result.size() == 6, "A single-frame sequence did not repeat to the requested length.");
    for (const auto& frame : result)
    {
        requireNear(frame.timeOffsetMs, source.timeOffsetMs, 1.0e-9,
            "A repeated single frame must keep its original time offset.");
        requireNear(frame.centerX, source.centerX, 1.0e-9, "A repeated single frame must be unchanged.");
        requireNear(frame.values[5], source.values[5], 1.0e-9, "A repeated single frame must be unchanged.");
    }
}

void testResampleUniformTimestampsAndEndpoints()
{
    measurements::UnifiedFeatureFrame start{};
    start.timeOffsetMs = 0.0;
    start.centerX = 0.0F;
    start.values[10] = 0.0F;

    measurements::UnifiedFeatureFrame end{};
    end.timeOffsetMs = 1000.0;
    end.centerX = 100.0F;
    end.values[10] = 10.0F;

    const auto result = measurements::resample({start, end}, 5);
    require(result.size() == 5, "Resample did not produce the requested sample count.");
    requireNear(result.front().timeOffsetMs, 0.0, 1.0e-6, "The resampled sequence must start at time zero.");
    requireNear(result.back().timeOffsetMs, 1000.0, 1.0e-6,
        "The resampled sequence's last sample must land on the source duration.");
    for (std::size_t i = 0; i < result.size(); ++i)
    {
        const double expectedTime = 250.0 * static_cast<double>(i);
        requireNear(result[i].timeOffsetMs, expectedTime, 1.0e-6,
            "Resample did not lay out timestamps uniformly.");
        requireNear(result[i].centerX, expectedTime / 10.0, 1.0e-4,
            "Resample did not linearly interpolate a value between uniformly spaced samples.");
    }
}

void testBuildUnifiedSequenceProducesExactly32Samples()
{
    std::vector<measurements::UnifiedFeatureObservation> observations;
    // Deliberately non-uniform timestamps: this exercises the search for the
    // bracketing pair of source frames inside resample.
    const std::array<std::uint64_t, 6> timestampsUs{0, 30'000, 45'000, 120'000, 121'000, 260'000};
    for (std::size_t i = 0; i < timestampsUs.size(); ++i)
    {
        auto observation = makeObservation(kOpenHandMultipliers, timestampsUs[i]);
        for (auto& landmark : observation.imageLandmarks)
        {
            landmark.x += static_cast<float>(i) * 3.0F;
        }
        observations.push_back(observation);
    }

    const auto sequence = measurements::buildFeatureSequence(observations);
    const auto unified = measurements::buildUnifiedSequence(sequence);
    require(unified.size() == measurements::kUnifiedSequenceLength,
        "BuildUnifiedSequence must always resample to the fixed 32-sample length.");
    requireNear(unified.front().timeOffsetMs, 0.0, 1.0e-6, "Unified sequence must start at time zero.");
    requireNear(unified.back().timeOffsetMs, sequence.back().timeOffsetMs, 1.0e-6,
        "Unified sequence must end at the source sequence's duration.");
    for (std::size_t i = 1; i < unified.size(); ++i)
    {
        require(unified[i].timeOffsetMs >= unified[i - 1].timeOffsetMs,
            "Unified sequence timestamps must be non-decreasing.");
    }
}

void testMakeUnifiedFeatureObservationWiresBoxAndHandedness()
{
    HandLandmarkResult hand{};
    hand.detected = true;
    hand.handedness = 0.65F;
    hand.landmarks = makeHandLandmarks(kOpenHandMultipliers);
    hand.box.left = 10.0F;
    hand.box.top = 20.0F;
    hand.box.right = 110.0F;
    hand.box.bottom = 220.0F;

    const auto observation = measurements::makeUnifiedFeatureObservation(hand, 42);
    require(observation.timestampUs == 42, "makeUnifiedFeatureObservation dropped the caller-supplied timestamp.");
    requireNear(observation.handedness, 0.65, 1.0e-6, "makeUnifiedFeatureObservation dropped handedness.");
    requireNear(observation.boundingBoxArea, 100.0 * 200.0, 1.0e-3,
        "makeUnifiedFeatureObservation computed the wrong bounding-box area.");
    requireNear(observation.imageLandmarks[9].y, hand.landmarks[9].y, 1.0e-6,
        "makeUnifiedFeatureObservation did not copy image landmarks.");
}
}

int main()
{
    try
    {
        testFeatureVectorLayoutConstants();
        testBuildFeatureFrameLandmarkNormalizationIsDimensionless();
        testAnalyzeFingerPoseDiscreteStates();
        testBuildFeatureFrameMatchesHandMeasurementExtractorTopology();
        testBuildFeatureSequenceEmptyAndSingleObservation();
        testBuildFeatureSequenceTranslationVelocityAndRatios();
        testBuildFeatureSequenceIsPureAcrossInterleavedTracks();
        testInterpolateAngleWrapsShortestPath();
        testInterpolateLerpsTranslationScaleAndArea();
        testInterpolateRecomputesDiscreteFingerStateFromInterpolatedValues();
        testResampleEmptyAndZeroLength();
        testResampleSingleFrameRepeatsUnchanged();
        testResampleUniformTimestampsAndEndpoints();
        testBuildUnifiedSequenceProducesExactly32Samples();
        testMakeUnifiedFeatureObservationWiresBoxAndHandedness();
        std::cout << "HandUnifiedFeatureFrame tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}

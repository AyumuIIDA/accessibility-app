using RyoikiTenkai.Core;
using RyoikiTenkai.Vision;
using Xunit;

namespace RyoikiTenkai.Tests;

public sealed class GestureTemplateFactoryTests
{
    [Fact]
    public void Create_NormalizesPositionAndScale()
    {
        var source = GestureTemplateFactory.Create(CreateWaveSamples(shiftX: 100, scale: 80, direction: 1));
        var translated = GestureTemplateFactory.Create(CreateWaveSamples(shiftX: 420, scale: 160, direction: 1));

        Assert.NotNull(source);
        Assert.NotNull(translated);
        var score = WindowGestureRecognizer.Score(source.Samples, translated.Samples);

        Assert.True(score < 0.08f, $"Expected similar normalized gestures, got score {score}");
    }

    [Fact]
    public void Score_SeparatesOppositeMotion()
    {
        var right = GestureTemplateFactory.Create(CreateWaveSamples(shiftX: 100, scale: 100, direction: 1));
        var left = GestureTemplateFactory.Create(CreateWaveSamples(shiftX: 100, scale: 100, direction: -1));

        Assert.NotNull(right);
        Assert.NotNull(left);
        var score = WindowGestureRecognizer.Score(right.Samples, left.Samples);

        Assert.True(score > 0.25f, $"Expected opposite motion to score worse, got score {score}");
    }

    [Fact]
    public void Create_ReturnsNullWhenTooFewTrackedFrames()
    {
        var template = GestureTemplateFactory.Create(CreateWaveSamples(count: 3));

        Assert.Null(template);
    }

    [Fact]
    public void TryCreate_LowMotionTakeRejectsStablePose()
    {
        var result = GestureTemplateFactory.TryCreate(CreateHoldSamples(shiftX: 100, scale: 80));

        Assert.Null(result.Template);
        Assert.Contains("stable pose", result.FailureReason);
    }

    [Fact]
    public void Create_HighMotionTakeCreatesUnifiedTemplate()
    {
        var template = GestureTemplateFactory.Create(CreateWaveSamples());

        Assert.NotNull(template);
        Assert.Equal(GestureKind.Dynamic, template.Kind);
        Assert.Equal(GestureFeatureExtractor.UnifiedSequenceLength, template.FeatureFrames!.Count);
        Assert.NotNull(template.ActiveSegment);
        Assert.NotNull(template.Topology);
        Assert.True(template.Topology.TopologyChangeScore >= GestureTemplateFactory.MinimumTopologyChangeScore);
    }

    [Fact]
    public void TryCreate_ReportsLowConfidenceFailureDetails()
    {
        var samples = CreateWaveSamples()
            .Select(x => x with { Confidence = 0.2f })
            .ToList();

        var result = GestureTemplateFactory.TryCreate(samples);

        Assert.Null(result.Template);
        Assert.Equal(samples.Count, result.SourceFrameCount);
        Assert.Equal(samples.Count, result.ValidLandmarkFrameCount);
        Assert.Equal(0, result.HighConfidenceFrameCount);
        Assert.Contains("confidence", result.FailureReason);
    }

    [Fact]
    public void Create_StoresFullHandSkeletonFramesForPlayback()
    {
        var samples = CreateWaveSamples();

        var template = GestureTemplateFactory.Create(samples);

        Assert.NotNull(template);
        Assert.NotNull(template.SkeletonFrames);
        Assert.Equal(samples.Count, template.SkeletonFrames.Count);
        Assert.All(template.SkeletonFrames, frame => Assert.Equal(21, frame.Landmarks.Count));
    }

    [Fact]
    public void TryCreate_RejectsLowFrameRateGestureWindows()
    {
        var samples = CreateWaveSamples(count: 40, intervalMilliseconds: 100);

        var result = GestureTemplateFactory.TryCreate(samples);

        Assert.Null(result.Template);
        Assert.Contains("usable fps", result.FailureReason);
    }

    [Fact]
    public void Create_PalmTurnAcceptsTopologyWithoutWristTravel()
    {
        var template = GestureTemplateFactory.Create(CreatePalmTurnSamples());

        Assert.NotNull(template);
        Assert.Equal(GestureKind.Dynamic, template.Kind);
        Assert.NotNull(template.FeatureTrack);
        Assert.NotNull(template.Topology);
        Assert.True(template.Topology.PalmTurnScore >= GestureTemplateFactory.MinimumPalmTurnScore);
        Assert.True(template.Topology.SignedPalmAreaSignChanges > 0);
        Assert.True(template.Topology.PalmCompressionDrop >= GestureTemplateFactory.MinimumPalmCompressionDrop);
    }

    [Fact]
    public void Create_GrabAcceptsPalmToFistFingerCurlTopology()
    {
        var template = GestureTemplateFactory.Create(CreateGrabSamples());

        Assert.NotNull(template);
        Assert.Equal(GestureKind.Dynamic, template.Kind);
        Assert.NotNull(template.Topology);
        Assert.True(template.Topology.FingerStateTransitionCount > 0);
        Assert.True(template.Topology.FingerStraightnessRangeMax >= GestureTemplateFactory.MinimumFingerStraightnessRange);
    }

    [Fact]
    public void Create_ApproachAcceptsHandSizeTopology()
    {
        var template = GestureTemplateFactory.Create(CreateApproachSamples());

        Assert.NotNull(template);
        Assert.Equal(GestureKind.Dynamic, template.Kind);
        Assert.NotNull(template.Topology);
        Assert.True(template.Topology.HandScaleRatioRange >= GestureTemplateFactory.MinimumHandScaleRatioRange);
        Assert.True(template.Topology.HandScaleRatioDelta > 0);
    }

    [Fact]
    public void Create_TwoHandGestureStoresHandCountAndRelativeTopology()
    {
        var result = MultiHandGestureFeatureExtractor.TryCreate(CreateTwoHandPinchSamples());

        Assert.NotNull(result.Template);
        Assert.Equal(2, result.Template.HandCount);
        Assert.NotNull(result.Template.MultiHandSummary);
        Assert.True(result.Template.MultiHandSummary.RelativeDistanceRange >= GestureTemplateFactory.MinimumHandScaleRatioRange);
        Assert.True(result.Template.MultiHandSummary.RelativeDistanceDelta < 0);
    }

    [Fact]
    public void TwoHandCoverage_IgnoresStraySecondHandDuringOneHandRecording()
    {
        var oneHand = CreateWaveSamples();
        var mixed = oneHand
            .Select(sample => new GestureFrameSetSample(sample.Timestamp, [sample]))
            .ToList();
        mixed[10] = mixed[10] with
        {
            Hands =
            [
                mixed[10].Hands[0],
                CreateSample(mixed[10].Timestamp, 280, 240, 80, 0.05f)
            ]
        };

        Assert.False(MultiHandGestureFeatureExtractor.HasPredominantTwoHandCoverage(mixed, oneHand.Count));

        var result = GestureTemplateFactory.TryCreate(oneHand);
        Assert.NotNull(result.Template);
        Assert.Equal(1, result.Template.HandCount);
    }

    [Fact]
    public void TwoHandCoverage_RejectsDuplicateDetectionsOfSameHand()
    {
        var oneHand = CreateWaveSamples();
        var duplicatePairs = oneHand
            .Select(sample => new GestureFrameSetSample(
                sample.Timestamp,
                [
                    sample,
                    sample with { Confidence = 0.94f, Handedness = 0.02f }
                ]))
            .ToList();

        Assert.Equal(0, MultiHandGestureFeatureExtractor.CountUsableTwoHandFrames(duplicatePairs));
        Assert.False(MultiHandGestureFeatureExtractor.HasPredominantTwoHandCoverage(duplicatePairs, oneHand.Count));

        var result = MultiHandGestureFeatureExtractor.TryCreate(duplicatePairs);
        Assert.Null(result.Template);
        Assert.Contains("two visible hands", result.FailureReason);
    }

    [Fact]
    public void TwoHandCoverage_AcceptsMostlyTwoHandRecording()
    {
        var twoHand = CreateTwoHandPinchSamples();
        for (var i = 0; i < 8; i++)
        {
            twoHand[i] = twoHand[i] with { Hands = [twoHand[i].Hands[0]] };
        }

        Assert.True(MultiHandGestureFeatureExtractor.HasPredominantTwoHandCoverage(twoHand, twoHand.Count));
    }

    internal static List<GestureFrameSample> CreateWaveSamples(
        int count = 60,
        float shiftX = 100,
        float scale = 100,
        int direction = 1,
        int intervalMilliseconds = 33,
        float handedness = 0)
    {
        var start = new DateTimeOffset(2026, 7, 17, 0, 0, 0, TimeSpan.Zero);
        var samples = new List<GestureFrameSample>();
        for (var i = 0; i < count; i++)
        {
            var progress = count == 1 ? 0 : i / (float)(count - 1);
            var centerX = shiftX + (direction * progress * scale * 0.8f);
            var centerY = 240 + MathF.Sin(progress * MathF.PI * 2) * scale * 0.08f;
            samples.Add(new GestureFrameSample(
                Timestamp: start + TimeSpan.FromMilliseconds(i * intervalMilliseconds),
                Landmarks: CreateHand(centerX, centerY, scale),
                Confidence: 0.95f,
                BoundingBox: null,
                Handedness: handedness));
        }

        return samples;
    }

    internal static List<GestureFrameSample> CreateApproachSamples(
        int count = 62,
        float shiftX = 100,
        float startScale = 80,
        float endScale = 145,
        int intervalMilliseconds = 33)
    {
        var start = new DateTimeOffset(2026, 7, 17, 0, 0, 0, TimeSpan.Zero);
        var samples = new List<GestureFrameSample>();
        for (var i = 0; i < count; i++)
        {
            var progress = count == 1 ? 0 : i / (float)(count - 1);
            var scale = startScale + ((endScale - startScale) * progress);
            var landmarks = CreateHand(shiftX, 240, scale);
            var minX = landmarks.Min(x => x.X);
            var maxX = landmarks.Max(x => x.X);
            var minY = landmarks.Min(x => x.Y);
            var maxY = landmarks.Max(x => x.Y);
            samples.Add(new GestureFrameSample(
                Timestamp: start + TimeSpan.FromMilliseconds(i * intervalMilliseconds),
                Landmarks: landmarks,
                Confidence: 0.95f,
                BoundingBox: new HandBox(minX, minY, maxX, maxY),
                Handedness: 0.9f));
        }

        return samples;
    }

    internal static List<GestureFrameSample> CreateTranslateSamples(
        int count = 62,
        float startX = 80,
        float deltaX = 90,
        float centerY = 240,
        float scale = 100,
        int intervalMilliseconds = 33)
    {
        var start = new DateTimeOffset(2026, 7, 17, 0, 0, 0, TimeSpan.Zero);
        var samples = new List<GestureFrameSample>();
        for (var i = 0; i < count; i++)
        {
            var progress = count == 1 ? 0 : i / (float)(count - 1);
            samples.Add(CreateSample(
                start + TimeSpan.FromMilliseconds(i * intervalMilliseconds),
                startX + (deltaX * progress),
                centerY,
                scale,
                0.9f));
        }

        return samples;
    }

    internal static List<GestureFrameSetSample> CreateTwoHandPinchSamples(
        int count = 62,
        float leftStartX = 60,
        float rightStartX = 240,
        float endGap = 70,
        float scale = 80,
        int intervalMilliseconds = 33)
    {
        var start = new DateTimeOffset(2026, 7, 17, 0, 0, 0, TimeSpan.Zero);
        var samples = new List<GestureFrameSetSample>();
        var center = (leftStartX + rightStartX) / 2f;
        for (var i = 0; i < count; i++)
        {
            var progress = count == 1 ? 0 : i / (float)(count - 1);
            var gap = (rightStartX - leftStartX) + ((endGap - (rightStartX - leftStartX)) * progress);
            var leftX = center - (gap / 2f);
            var rightX = center + (gap / 2f);
            samples.Add(new GestureFrameSetSample(
                start + TimeSpan.FromMilliseconds(i * intervalMilliseconds),
                [
                    CreateSample(start + TimeSpan.FromMilliseconds(i * intervalMilliseconds), leftX, 240, scale, 0.05f),
                    CreateSample(start + TimeSpan.FromMilliseconds(i * intervalMilliseconds), rightX, 240, scale, 0.95f)
                ]));
        }

        return samples;
    }

    private static GestureFrameSample CreateSample(
        DateTimeOffset timestamp,
        float centerX,
        float centerY,
        float scale,
        float handedness)
    {
        var landmarks = CreateHand(centerX, centerY, scale);
        var minX = landmarks.Min(x => x.X);
        var maxX = landmarks.Max(x => x.X);
        var minY = landmarks.Min(x => x.Y);
        var maxY = landmarks.Max(x => x.Y);
        return new GestureFrameSample(
            timestamp,
            landmarks,
            0.95f,
            new HandBox(minX, minY, maxX, maxY),
            handedness);
    }

    internal static List<GestureFrameSample> CreatePalmTurnSamples(
        int count = 62,
        float shiftX = 100,
        float scale = 100,
        int intervalMilliseconds = 33)
    {
        var start = new DateTimeOffset(2026, 7, 17, 0, 0, 0, TimeSpan.Zero);
        var baseHand = CreateHand(shiftX, 240, scale);
        var centerX = shiftX;
        var samples = new List<GestureFrameSample>();
        for (var i = 0; i < count; i++)
        {
            var progress = count == 1 ? 0 : i / (float)(count - 1);
            var angle = progress * MathF.PI;
            var xFactor = MathF.Cos(angle);
            var zFactor = MathF.Sin(angle) * 0.9f;
            var landmarks = baseHand
                .Select(point =>
                {
                    var dx = point.X - centerX;
                    return new HandLandmark(
                        centerX + (dx * xFactor),
                        point.Y,
                        dx * zFactor / scale);
                })
                .ToList();
            var minX = landmarks.Min(x => x.X);
            var maxX = landmarks.Max(x => x.X);
            var minY = landmarks.Min(x => x.Y);
            var maxY = landmarks.Max(x => x.Y);
            samples.Add(new GestureFrameSample(
                Timestamp: start + TimeSpan.FromMilliseconds(i * intervalMilliseconds),
                Landmarks: landmarks,
                Confidence: 0.95f,
                BoundingBox: new HandBox(minX, minY, maxX, maxY),
                Handedness: 0.92f));
        }

        return samples;
    }

    internal static List<GestureFrameSample> CreateGrabSamples(
        int count = 62,
        float shiftX = 100,
        float scale = 100,
        int intervalMilliseconds = 33)
    {
        var start = new DateTimeOffset(2026, 7, 17, 0, 0, 0, TimeSpan.Zero);
        var samples = new List<GestureFrameSample>();
        for (var i = 0; i < count; i++)
        {
            var progress = count == 1 ? 0 : i / (float)(count - 1);
            var landmarks = CreateHand(shiftX, 240, scale);
            CurlFingerTowardWrist(landmarks, 4, progress);
            CurlFingerTowardWrist(landmarks, 8, progress);
            CurlFingerTowardWrist(landmarks, 12, progress);
            CurlFingerTowardWrist(landmarks, 16, progress);
            CurlFingerTowardWrist(landmarks, 20, progress);
            samples.Add(new GestureFrameSample(
                Timestamp: start + TimeSpan.FromMilliseconds(i * intervalMilliseconds),
                Landmarks: landmarks,
                Confidence: 0.95f,
                BoundingBox: null,
                Handedness: 0.9f));
        }

        return samples;
    }

    internal static List<GestureFrameSample> CreateHoldSamples(
        int count = 60,
        float shiftX = 100,
        float scale = 100,
        float jitter = 0,
        int intervalMilliseconds = 33)
    {
        var start = new DateTimeOffset(2026, 7, 17, 0, 0, 0, TimeSpan.Zero);
        var samples = new List<GestureFrameSample>();
        for (var i = 0; i < count; i++)
        {
            var dx = jitter == 0 ? 0 : MathF.Sin(i * 0.71f) * jitter;
            var dy = jitter == 0 ? 0 : MathF.Cos(i * 0.43f) * jitter;
            samples.Add(new GestureFrameSample(
                Timestamp: start + TimeSpan.FromMilliseconds(i * intervalMilliseconds),
                Landmarks: CreateHand(shiftX + dx, 240 + dy, scale),
                Confidence: 0.95f,
                BoundingBox: null,
                Handedness: 0));
        }

        return samples;
    }

    internal static List<HandLandmark> CreateHand(float centerX, float centerY, float scale)
    {
        var points = Enumerable.Range(0, 21)
            .Select(_ => new HandLandmark(centerX, centerY, 0))
            .ToList();

        points[0] = new HandLandmark(centerX, centerY + (scale / 2), 0);
        points[4] = new HandLandmark(centerX - (scale * 0.55f), centerY - (scale * 0.15f), 0);
        points[5] = new HandLandmark(centerX - (scale * 0.22f), centerY - (scale * 0.05f), 0);
        points[8] = new HandLandmark(centerX - (scale * 0.18f), centerY - scale, 0);
        points[9] = new HandLandmark(centerX, centerY - (scale / 2), 0);
        points[12] = new HandLandmark(centerX, centerY - (scale * 1.05f), 0);
        points[13] = new HandLandmark(centerX + (scale * 0.18f), centerY - (scale * 0.05f), 0);
        points[16] = new HandLandmark(centerX + (scale * 0.2f), centerY - scale, 0);
        points[17] = new HandLandmark(centerX + (scale * 0.35f), centerY, 0);
        points[20] = new HandLandmark(centerX + (scale * 0.38f), centerY - (scale * 0.85f), 0);
        return points;
    }

    private static void CurlFingerTowardWrist(List<HandLandmark> landmarks, int tipIndex, float progress)
    {
        var wrist = landmarks[0];
        var basePoint = landmarks[tipIndex - 3];
        var curl = Math.Clamp(progress, 0, 1);
        landmarks[tipIndex - 2] = Lerp(landmarks[tipIndex - 2], wrist, curl * 0.45f);
        landmarks[tipIndex - 1] = Lerp(landmarks[tipIndex - 1], wrist, curl * 0.60f);
        landmarks[tipIndex] = Lerp(landmarks[tipIndex], basePoint, curl * 0.92f);
    }

    private static HandLandmark Lerp(HandLandmark a, HandLandmark b, float t)
    {
        return new HandLandmark(
            a.X + ((b.X - a.X) * t),
            a.Y + ((b.Y - a.Y) * t),
            a.Z + ((b.Z - a.Z) * t));
    }
}

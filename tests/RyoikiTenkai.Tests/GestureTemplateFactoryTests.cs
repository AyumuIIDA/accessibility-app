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
    public void Create_LowMotionTakeCreatesStaticTemplate()
    {
        var template = GestureTemplateFactory.Create(CreateHoldSamples(shiftX: 100, scale: 80));

        Assert.NotNull(template);
        Assert.Equal(GestureKind.Static, template.Kind);
        Assert.True(template.MotionSummary?.MotionScore < GestureFeatureExtractor.StaticMotionThreshold);
        Assert.Single(template.FeatureFrames!);
    }

    [Fact]
    public void Create_HighMotionTakeCreatesDynamicTemplate()
    {
        var template = GestureTemplateFactory.Create(CreateWaveSamples());

        Assert.NotNull(template);
        Assert.Equal(GestureKind.Dynamic, template.Kind);
        Assert.Equal(GestureFeatureExtractor.DynamicResampledLength, template.FeatureFrames!.Count);
        Assert.NotNull(template.ActiveSegment);
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
        var samples = CreateWaveSamples(count: 20, intervalMilliseconds: 100);

        var result = GestureTemplateFactory.TryCreate(samples);

        Assert.Null(result.Template);
        Assert.Contains("40 usable frames", result.FailureReason);
    }

    internal static List<GestureFrameSample> CreateWaveSamples(
        int count = 60,
        float shiftX = 100,
        float scale = 100,
        int direction = 1,
        int intervalMilliseconds = 33)
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
                Handedness: 0));
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
}

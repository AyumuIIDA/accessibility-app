using RyoikiTenkai.Core;
using RyoikiTenkai.Vision;
using Xunit;

namespace RyoikiTenkai.Tests;

public sealed class WindowGestureRecognizerDebugTests
{
    [Fact]
    public void DebugSnapshot_ReportsInactiveGestureReason()
    {
        var samples = GestureTemplateFactoryTests.CreateWaveSamples();
        var template = GestureTemplateFactory.Create(samples);
        Assert.NotNull(template);

        var recognizer = new WindowGestureRecognizer();
        recognizer.SetDefinitions([
            new GestureDefinition(
                Id: "wave",
                DisplayName: "Wave",
                Type: "template",
                Templates: [template],
                CreatedAt: DateTimeOffset.UtcNow)
        ]);

        foreach (var sample in samples)
        {
            recognizer.Recognize(sample);
        }

        var snapshot = recognizer.GetDebugSnapshot();

        Assert.Contains(snapshot.Scores, x => x.GestureId == "wave" && x.Reason == "Needs 3 examples");
        Assert.Null(snapshot.BestGestureId);
    }

    [Fact]
    public void DebugSnapshot_ReportsBestScoreAndStableState()
    {
        var samples = GestureTemplateFactoryTests.CreateWaveSamples(count: 61);
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateWaveSamples());
        Assert.NotNull(template);

        var recognizer = new WindowGestureRecognizer();
        recognizer.SetDefinitions([
            new GestureDefinition(
                Id: "wave",
                DisplayName: "Wave",
                Type: "template",
                Templates: [template, template, template],
                CreatedAt: DateTimeOffset.UtcNow)
        ]);

        GestureRecognitionResult? result = null;
        foreach (var sample in samples)
        {
            result = recognizer.Recognize(sample);
            if (result is not null)
            {
                break;
            }
        }

        var snapshot = recognizer.GetDebugSnapshot();

        Assert.Equal("wave", snapshot.BestGestureId);
        Assert.NotNull(snapshot.CandidateTemplate);
        Assert.Equal(32, snapshot.CandidateTemplate.Samples.Count);
        Assert.Contains(snapshot.Scores, x => x.IsBest && x.Confidence >= snapshot.MatchThreshold);
        Assert.Contains("Confirmed", snapshot.TriggerState);
    }

    [Fact]
    public void Recognize_DoesNotReportStaticGestureWithoutCustomDefinitions()
    {
        var recognizer = new WindowGestureRecognizer();

        foreach (var sample in GestureTemplateFactoryTests.CreateWaveSamples())
        {
            recognizer.Recognize(sample);
        }

        var snapshot = recognizer.GetDebugSnapshot();

        Assert.Null(snapshot.BestGestureId);
        Assert.Empty(snapshot.Scores);
    }

    [Fact]
    public void WindowBuffer_DropsFramesOlderThanTwoSecondWindow()
    {
        var buffer = new GestureWindowBuffer(TimeSpan.FromSeconds(2));
        var start = new DateTimeOffset(2026, 7, 17, 0, 0, 0, TimeSpan.Zero);
        var samples = GestureTemplateFactoryTests.CreateWaveSamples(count: 5)
            .Select((sample, index) => sample with
            {
                Timestamp = start + TimeSpan.FromMilliseconds(index * 700)
            })
            .ToList();

        foreach (var sample in samples)
        {
            buffer.Add(sample);
        }

        var snapshot = buffer.CreateSnapshot();

        Assert.Equal(3, snapshot.FrameCount);
        Assert.Equal(3, snapshot.UsableFrameCount);
        Assert.Equal(1400, snapshot.DurationMilliseconds);
        Assert.True(snapshot.EffectiveFps > 2);
    }

    [Fact]
    public void DebugSnapshot_ReportsConfirmedMatch()
    {
        var samples = GestureTemplateFactoryTests.CreateWaveSamples(count: 61);
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateWaveSamples());
        Assert.NotNull(template);

        var recognizer = new WindowGestureRecognizer();
        recognizer.SetDefinitions([
            new GestureDefinition(
                Id: "wave",
                DisplayName: "Wave",
                Type: "template",
                Templates: [template, template, template],
                CreatedAt: DateTimeOffset.UtcNow)
        ]);

        GestureRecognitionResult? result = null;
        foreach (var sample in samples)
        {
            result = recognizer.Recognize(sample);
            if (result is not null)
            {
                break;
            }
        }

        var snapshot = recognizer.GetDebugSnapshot();

        Assert.NotNull(result);
        Assert.NotNull(snapshot.ConfirmedMatch);
        Assert.Equal("wave", snapshot.ConfirmedMatch.GestureId);
        Assert.True(snapshot.UsableFrameCount >= GestureTemplateFactory.MinimumUsableSampleCount);
        Assert.True(snapshot.BufferEffectiveFps > 0);
        Assert.NotNull(snapshot.BestTemplate);
    }

    [Fact]
    public void DebugSnapshot_ReportsWindowFramesForRollingBuffer()
    {
        var recognizer = new WindowGestureRecognizer();
        var samples = GestureTemplateFactoryTests.CreateHoldSamples(count: 12);

        foreach (var sample in samples)
        {
            recognizer.Recognize(sample);
        }

        var snapshot = recognizer.GetDebugSnapshot();

        Assert.NotNull(snapshot.WindowFrames);
        Assert.Equal(snapshot.BufferFrameCount, snapshot.WindowFrames.Count);
        Assert.Equal(12, snapshot.WindowFrames.Count);
        Assert.All(snapshot.WindowFrames, frame =>
        {
            Assert.True(frame.IsUsable);
            Assert.False(frame.IsAcceptedMatch);
        });
    }

    [Fact]
    public void DebugSnapshot_MarksConfirmedWindowFramesAsAcceptedMatch()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateHoldSamples());
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("hold", template);

        var result = Feed(recognizer, GestureTemplateFactoryTests.CreateHoldSamples(count: 62));
        var snapshot = recognizer.GetDebugSnapshot();

        Assert.NotNull(result);
        Assert.NotNull(snapshot.WindowFrames);
        Assert.Contains(snapshot.WindowFrames, x => x.IsAcceptedMatch);
        Assert.All(snapshot.WindowFrames.Where(x => x.IsAcceptedMatch), frame => Assert.True(frame.IsUsable));
    }

    [Fact]
    public void Reset_ClearsDebugWindowFrames()
    {
        var recognizer = new WindowGestureRecognizer();
        foreach (var sample in GestureTemplateFactoryTests.CreateHoldSamples(count: 5))
        {
            recognizer.Recognize(sample);
        }

        recognizer.Reset("No hand for 500 ms");
        var snapshot = recognizer.GetDebugSnapshot();

        Assert.Equal("No hand for 500 ms", snapshot.TriggerState);
        Assert.Equal(0, snapshot.BufferFrameCount);
        Assert.Empty(snapshot.WindowFrames ?? []);
    }

    [Fact]
    public void DebugSnapshot_ReportsLowFrameRateCandidateReason()
    {
        var samples = GestureTemplateFactoryTests.CreateWaveSamples(count: 20, intervalMilliseconds: 100);
        var recognizer = new WindowGestureRecognizer();

        foreach (var sample in samples)
        {
            recognizer.Recognize(sample);
        }

        var snapshot = recognizer.GetDebugSnapshot();

        Assert.Null(snapshot.BestGestureId);
        Assert.Contains("40 usable frames", snapshot.CandidateFailureReason);
        Assert.True(snapshot.UsableEffectiveFps < 30);
    }

    [Fact]
    public void Recognize_StaticPoseMatchesAcrossScaleAndPosition()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateHoldSamples(shiftX: 100, scale: 80));
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("hold", template);

        var result = Feed(recognizer, GestureTemplateFactoryTests.CreateHoldSamples(count: 62, shiftX: 420, scale: 160));

        Assert.NotNull(result);
        Assert.Equal("hold", result.GestureId);
        Assert.Equal("custom-static", result.Source);
    }

    [Fact]
    public void Recognize_StaticPoseToleratesSmallJitter()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateHoldSamples());
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("hold", template);

        var result = Feed(recognizer, GestureTemplateFactoryTests.CreateHoldSamples(count: 62, jitter: 0.4f));

        Assert.NotNull(result);
        Assert.Equal("hold", result.GestureId);
    }

    [Fact]
    public void Recognize_UnrelatedStaticPoseReturnsUnknown()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateHoldSamples());
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("hold", template);

        var result = Feed(recognizer, CreateFistHoldSamples(count: 62));

        Assert.Null(result);
        Assert.Null(recognizer.GetDebugSnapshot().BestGestureId);
    }

    [Fact]
    public void Recognize_RequiresStableConsecutiveStaticFrames()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateHoldSamples());
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("hold", template);
        GestureRecognitionResult? result = null;

        foreach (var sample in GestureTemplateFactoryTests.CreateHoldSamples(count: 59))
        {
            result = recognizer.Recognize(sample);
        }

        Assert.Null(result);
        Assert.Contains("Waiting for stable match", recognizer.GetDebugSnapshot().TriggerState);
    }

    [Fact]
    public void Recognize_DynamicGestureMatchesWhenPerformedFaster()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateWaveSamples());
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("wave", template);

        var result = Feed(recognizer, GestureTemplateFactoryTests.CreateWaveSamples(count: 62, intervalMilliseconds: 32));

        Assert.NotNull(result);
        Assert.Equal("wave", result.GestureId);
        Assert.Equal("custom-dynamic", result.Source);
        Assert.True(recognizer.GetDebugSnapshot().DtwWarpRatio <= 2.6f);
    }

    [Fact]
    public void Recognize_DynamicOppositeDirectionReturnsUnknown()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateWaveSamples(direction: 1));
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("wave", template);

        var result = Feed(recognizer, GestureTemplateFactoryTests.CreateWaveSamples(count: 62, direction: -1));

        Assert.Null(result);
        Assert.Null(recognizer.GetDebugSnapshot().BestGestureId);
    }

    [Fact]
    public void Create_DynamicTemplateRemovesInactivePadding()
    {
        var padded = GestureTemplateFactoryTests.CreateHoldSamples(count: 10)
            .Concat(GestureTemplateFactoryTests.CreateWaveSamples(count: 42).Select((sample, index) => sample with
            {
                Timestamp = new DateTimeOffset(2026, 7, 17, 0, 0, 0, TimeSpan.Zero) + TimeSpan.FromMilliseconds((index + 10) * 33)
            }))
            .Concat(GestureTemplateFactoryTests.CreateHoldSamples(count: 10).Select((sample, index) => sample with
            {
                Timestamp = new DateTimeOffset(2026, 7, 17, 0, 0, 0, TimeSpan.Zero) + TimeSpan.FromMilliseconds((index + 52) * 33)
            }))
            .ToList();

        var template = GestureTemplateFactory.Create(padded);

        Assert.NotNull(template);
        Assert.Equal(GestureKind.Dynamic, template.Kind);
        Assert.NotNull(template.ActiveSegment);
        Assert.True(template.ActiveSegment.StartIndex > 0);
        Assert.True(template.ActiveSegment.EndIndex < padded.Count - 1);
    }

    private static WindowGestureRecognizer CreateRecognizer(string id, GestureTemplate template)
    {
        var recognizer = new WindowGestureRecognizer();
        recognizer.SetDefinitions([
            new GestureDefinition(
                Id: id,
                DisplayName: id,
                Type: "template",
                Templates: [template, template, template],
                CreatedAt: DateTimeOffset.UtcNow)
        ]);
        return recognizer;
    }

    private static GestureRecognitionResult? Feed(
        WindowGestureRecognizer recognizer,
        IReadOnlyList<GestureFrameSample> samples)
    {
        GestureRecognitionResult? result = null;
        foreach (var sample in samples)
        {
            result = recognizer.Recognize(sample);
            if (result is not null)
            {
                return result;
            }
        }

        return result;
    }

    private static List<GestureFrameSample> CreateFistHoldSamples(int count)
    {
        return GestureTemplateFactoryTests.CreateHoldSamples(count: count)
            .Select(sample =>
            {
                var landmarks = GestureTemplateFactoryTests.CreateHand(100, 240, 100);
                landmarks[4] = new HandLandmark(95, 230, 0);
                landmarks[8] = new HandLandmark(96, 228, 0);
                landmarks[12] = new HandLandmark(100, 227, 0);
                landmarks[16] = new HandLandmark(104, 228, 0);
                landmarks[20] = new HandLandmark(108, 230, 0);
                return sample with { Landmarks = landmarks };
            })
            .ToList();
    }
}

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
    public void Recognize_DoesNotReportGestureWithoutCustomDefinitions()
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
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateWaveSamples());
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("wave", template);

        var result = Feed(recognizer, GestureTemplateFactoryTests.CreateWaveSamples(count: 62));
        var snapshot = recognizer.GetDebugSnapshot();

        Assert.NotNull(result);
        Assert.NotNull(snapshot.WindowFrames);
        Assert.Contains(snapshot.WindowFrames, x => x.IsAcceptedMatch);
        Assert.All(snapshot.WindowFrames.Where(x => x.IsAcceptedMatch), frame => Assert.True(frame.IsUsable));
    }

    [Fact]
    public void Recognize_OneHandTemplateToleratesHandednessJitter()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateWaveSamples(handedness: 0.05f));
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("wave", template);
        var samples = GestureTemplateFactoryTests.CreateWaveSamples(count: 62)
            .Select((sample, index) => sample with { Handedness = index % 2 == 0 ? 0.05f : 0.95f })
            .ToList();

        var result = Feed(recognizer, samples);

        Assert.NotNull(result);
        Assert.Equal("wave", result.GestureId);
    }

    [Fact]
    public void MultiHandDebugSnapshot_MarksConfirmedWindowFramesAsAcceptedMatch()
    {
        var template = MultiHandGestureFeatureExtractor.TryCreate(GestureTemplateFactoryTests.CreateTwoHandPinchSamples()).Template;
        Assert.NotNull(template);
        var recognizer = CreateTwoHandRecognizer("pinch", template);

        GestureRecognitionResult? result = null;
        foreach (var sample in GestureTemplateFactoryTests.CreateTwoHandPinchSamples())
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
        Assert.NotNull(snapshot.WindowFrames);
        Assert.NotEmpty(snapshot.WindowFrames);
        Assert.Contains(snapshot.WindowFrames, x => x.IsAcceptedMatch);
        Assert.True(snapshot.WindowFrames.Count(x => x.IsAcceptedMatch) > 1);
    }

    [Fact]
    public void MultiHandRecognizer_SuppressesOverlappingDuplicateSegments()
    {
        var template = MultiHandGestureFeatureExtractor.TryCreate(GestureTemplateFactoryTests.CreateTwoHandPinchSamples()).Template;
        Assert.NotNull(template);
        var recognizer = CreateTwoHandRecognizer("pinch", template);
        var samples = GestureTemplateFactoryTests.CreateTwoHandPinchSamples();
        var last = samples[^1];
        for (var i = 1; i <= 90; i++)
        {
            samples.Add(last with { Timestamp = last.Timestamp + TimeSpan.FromMilliseconds(i * 33) });
        }

        var matches = 0;
        foreach (var sample in samples)
        {
            if (recognizer.Recognize(sample) is not null)
            {
                matches++;
            }
        }

        Assert.Equal(1, matches);
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
        Assert.Contains("No gesture recordings", snapshot.CandidateFailureReason);
        Assert.True(snapshot.UsableEffectiveFps < 30);
    }

    [Fact]
    public void Recognize_MovementSequenceMatchesAcrossScaleAndPosition()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateWaveSamples(shiftX: 100, scale: 80));
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("wave", template);

        var result = Feed(recognizer, GestureTemplateFactoryTests.CreateWaveSamples(count: 62, shiftX: 420, scale: 160));

        Assert.NotNull(result);
        Assert.Equal("wave", result.GestureId);
        Assert.Equal("custom-unified", result.Source);
    }

    [Fact]
    public void Create_StablePoseReturnsNoTemplate()
    {
        var result = GestureTemplateFactory.TryCreate(GestureTemplateFactoryTests.CreateHoldSamples(count: 62, jitter: 0.4f));

        Assert.Null(result.Template);
        Assert.Contains("stable pose", result.FailureReason);
    }

    [Fact]
    public void Recognize_UnrelatedPoseSequenceReturnsUnknown()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateWaveSamples());
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("wave", template);

        var result = Feed(recognizer, CreateFistHoldSamples(count: 62));

        Assert.Null(result);
        Assert.Null(recognizer.GetDebugSnapshot().BestGestureId);
    }

    [Fact]
    public void Recognize_SuppressesDuplicateMovementSegment()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateWaveSamples());
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("wave", template);
        GestureRecognitionResult? result = null;

        foreach (var sample in GestureTemplateFactoryTests.CreateWaveSamples(count: 80))
        {
            result = recognizer.Recognize(sample);
        }

        Assert.Null(result);
        Assert.Contains("Duplicate segment suppressed", recognizer.GetDebugSnapshot().TriggerState);
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
        Assert.Equal("custom-unified", result.Source);
        Assert.True(recognizer.GetDebugSnapshot().DtwWarpRatio <= 2.6f);
    }

    [Fact]
    public void Recognize_PalmTurnRequiresPalmTurnTopology()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreatePalmTurnSamples());
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("palm_flip", template);

        var result = Feed(recognizer, GestureTemplateFactoryTests.CreatePalmTurnSamples(count: 64, shiftX: 360, scale: 150));

        Assert.NotNull(result);
        Assert.Equal("palm_flip", result.GestureId);
        Assert.NotNull(recognizer.GetDebugSnapshot().ScoreBreakdown);
        Assert.True(recognizer.GetDebugSnapshot().CandidateTemplate?.Topology?.PalmTurnScore >= GestureTemplateFactory.MinimumPalmTurnScore);
    }

    [Fact]
    public void Recognize_OpenPalmHoldDoesNotMatchPalmTurn()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreatePalmTurnSamples());
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("palm_flip", template);

        var result = Feed(recognizer, GestureTemplateFactoryTests.CreateHoldSamples(count: 64, shiftX: 360, scale: 150));

        Assert.Null(result);
        Assert.Null(recognizer.GetDebugSnapshot().BestGestureId);
        Assert.Contains(recognizer.GetDebugSnapshot().Scores, score => score.Reason.Contains("Palm turn", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public void Recognize_PeaceTurnDoesNotMatchOpenPalmTurn()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreatePalmTurnSamples());
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("palm_flip", template);

        var result = Feed(recognizer, GestureTemplateFactoryTests.CreatePalmTurnSamples(count: 64).Select(sample => sample with
        {
            Landmarks = CreatePeaceHand(sample.Landmarks)
        }).ToList());

        Assert.Null(result);
        Assert.Null(recognizer.GetDebugSnapshot().BestGestureId);
        Assert.Contains(recognizer.GetDebugSnapshot().Scores, score =>
            score.Reason.Contains("Finger state mismatch", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public void Recognize_ApproachRequiresHandSizeGrowth()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateApproachSamples());
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("approach", template);

        var result = Feed(recognizer, GestureTemplateFactoryTests.CreateApproachSamples(count: 64, startScale: 80, endScale: 145));

        Assert.NotNull(result);
        Assert.Equal("approach", result.GestureId);
        Assert.True(recognizer.GetDebugSnapshot().CandidateTemplate?.Topology?.HandScaleRatioRange >= GestureTemplateFactory.MinimumHandScaleRatioRange);
    }

    [Fact]
    public void Recognize_ApproachMatchesAcrossPositionAndStartingSize()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateApproachSamples(shiftX: 80, startScale: 70, endScale: 130));
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("approach", template);

        var result = Feed(recognizer, GestureTemplateFactoryTests.CreateApproachSamples(count: 64, shiftX: 420, startScale: 110, endScale: 205));

        Assert.NotNull(result);
        Assert.Equal("approach", result.GestureId);
    }

    [Fact]
    public void Recognize_TranslationMatchesAcrossStartingPosition()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateTranslateSamples(startX: 60, deltaX: 90));
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("translate", template);

        var result = Feed(recognizer, GestureTemplateFactoryTests.CreateTranslateSamples(count: 64, startX: 420, deltaX: 90));

        Assert.NotNull(result);
        Assert.Equal("translate", result.GestureId);
    }

    [Fact]
    public void Recognize_RetreatDoesNotMatchApproach()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateApproachSamples(startScale: 80, endScale: 145));
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("approach", template);

        var result = Feed(recognizer, GestureTemplateFactoryTests.CreateApproachSamples(count: 64, startScale: 145, endScale: 80));

        Assert.Null(result);
        Assert.Null(recognizer.GetDebugSnapshot().BestGestureId);
        Assert.Contains(recognizer.GetDebugSnapshot().Scores, score =>
            score.Reason.Contains("Hand size", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public void Recognize_OneHandTemplateMatchesOppositeHandSide()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateWaveSamples(handedness: 0.9f));
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("right_wave", template);

        var result = Feed(recognizer, GestureTemplateFactoryTests.CreateWaveSamples(count: 64, handedness: 0.05f));

        Assert.NotNull(result);
        Assert.Equal("right_wave", result.GestureId);
    }

    [Fact]
    public void Recognize_TwoHandGestureMatchesPairedHands()
    {
        var template = MultiHandGestureFeatureExtractor.TryCreate(GestureTemplateFactoryTests.CreateTwoHandPinchSamples()).Template;
        Assert.NotNull(template);
        var recognizer = CreateTwoHandRecognizer("two_hand_pinch", template);

        var result = Feed(recognizer, GestureTemplateFactoryTests.CreateTwoHandPinchSamples(count: 64));

        Assert.NotNull(result);
        Assert.Equal("two_hand_pinch", result.GestureId);
        Assert.Equal("custom-two-hand", result.Source);
        Assert.Equal(2, recognizer.GetDebugSnapshot().CandidateTemplate?.HandCount);
    }

    [Fact]
    public void Recognize_TwoHandGestureRejectsOppositeDistanceDirection()
    {
        var template = MultiHandGestureFeatureExtractor.TryCreate(GestureTemplateFactoryTests.CreateTwoHandPinchSamples()).Template;
        Assert.NotNull(template);
        var recognizer = CreateTwoHandRecognizer("two_hand_pinch", template);

        var result = Feed(recognizer, GestureTemplateFactoryTests.CreateTwoHandPinchSamples(count: 64, leftStartX: 115, rightStartX: 185, endGap: 180));

        Assert.Null(result);
        Assert.Null(recognizer.GetDebugSnapshot().BestGestureId);
        Assert.Contains(recognizer.GetDebugSnapshot().Scores, score =>
            score.Reason.Contains("opposite direction", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public void Recognize_TwoHandGestureRejectsReverseTimeOrder()
    {
        var template = MultiHandGestureFeatureExtractor.TryCreate(GestureTemplateFactoryTests.CreateTwoHandPinchSamples()).Template;
        Assert.NotNull(template);
        var recognizer = CreateTwoHandRecognizer("two_hand_pinch", template);

        var result = Feed(recognizer, ReverseTime(GestureTemplateFactoryTests.CreateTwoHandPinchSamples(count: 64)));

        Assert.Null(result);
        Assert.Null(recognizer.GetDebugSnapshot().BestGestureId);
        Assert.Contains(recognizer.GetDebugSnapshot().Scores, score =>
            score.Reason.Contains("Reversed", StringComparison.OrdinalIgnoreCase)
            || score.Reason.Contains("opposite direction", StringComparison.OrdinalIgnoreCase));
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
    public void Recognize_DynamicReverseTimeOrderReturnsUnknown()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateGrabSamples());
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("grab", template);

        var result = Feed(recognizer, ReverseTime(GestureTemplateFactoryTests.CreateGrabSamples(count: 64)));

        Assert.Null(result);
        Assert.Null(recognizer.GetDebugSnapshot().BestGestureId);
        Assert.Contains(recognizer.GetDebugSnapshot().Scores, score =>
            score.Reason.Contains("Opposite time direction", StringComparison.OrdinalIgnoreCase)
            || score.Reason.Contains("Reversed", StringComparison.OrdinalIgnoreCase)
            || score.Reason.Contains("Finger", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public void TopologyRejection_GrabRejectsPistolFingerState()
    {
        var candidate = new GestureTopologySummary(
            PalmTravel: 0.036f,
            PalmOrientationRangeRadians: 0.056f,
            HandednessRange: 0.048f,
            FingerStateTransitionCount: 4,
            StartFingerStateMask: 15,
            EndFingerStateMask: 15,
            TopologyChangeScore: 1.086f,
            PalmTurnScore: 0.126f,
            FingerStraightnessRangeMax: 0.322f);
        var grabTemplate = new GestureTopologySummary(
            PalmTravel: 0.088f,
            PalmOrientationRangeRadians: 0.082f,
            HandednessRange: 0.060f,
            FingerStateTransitionCount: 4,
            StartFingerStateMask: 31,
            EndFingerStateMask: 1,
            TopologyChangeScore: 1.964f,
            PalmTurnScore: 0.213f,
            FingerStraightnessRangeMax: 1.0f);

        var reason = WindowGestureRecognizer.TopologyRejectionReason(candidate, grabTemplate);

        Assert.Contains("Finger final state did not change", reason);
        Assert.Contains("11110->11110", reason);
        Assert.Contains("11111->10000", reason);
    }

    [Fact]
    public void TopologyRejection_FingerZeroAndOneMustMatchExactly()
    {
        var candidate = new GestureTopologySummary(
            PalmTravel: 0.09f,
            PalmOrientationRangeRadians: 0.08f,
            HandednessRange: 0.05f,
            FingerStateTransitionCount: 4,
            StartFingerStateMask: 31,
            EndFingerStateMask: 3,
            TopologyChangeScore: 1.9f,
            PalmTurnScore: 0.2f,
            FingerStraightnessRangeMax: 1.0f);
        var grabTemplate = new GestureTopologySummary(
            PalmTravel: 0.09f,
            PalmOrientationRangeRadians: 0.08f,
            HandednessRange: 0.05f,
            FingerStateTransitionCount: 4,
            StartFingerStateMask: 31,
            EndFingerStateMask: 1,
            TopologyChangeScore: 1.9f,
            PalmTurnScore: 0.2f,
            FingerStraightnessRangeMax: 1.0f);

        var reason = WindowGestureRecognizer.TopologyRejectionReason(candidate, grabTemplate);

        Assert.Contains("Finger 0/1 state mismatch", reason);
        Assert.Contains("11111->11000", reason);
        Assert.Contains("11111->10000", reason);
    }

    [Fact]
    public void Recognize_PalmTurnReverseTimeOrderReturnsUnknown()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreatePalmTurnSamples());
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("palm_flip", template);

        var result = Feed(recognizer, ReverseTime(GestureTemplateFactoryTests.CreatePalmTurnSamples(count: 64)));

        Assert.Null(result);
        Assert.Null(recognizer.GetDebugSnapshot().BestGestureId);
        Assert.Contains(recognizer.GetDebugSnapshot().Scores, score =>
            score.Reason.Contains("Opposite time direction", StringComparison.OrdinalIgnoreCase)
            || score.Reason.Contains("Reversed", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public void Recognize_StaticActionlessTemplateIsInactive()
    {
        var topology = new GestureTopologySummary(
            PalmTravel: 0.02f,
            PalmOrientationRangeRadians: 0.02f,
            HandednessRange: 0,
            FingerStateTransitionCount: 0,
            StartFingerStateMask: 3,
            EndFingerStateMask: 3,
            TopologyChangeScore: 0.95f,
            PalmTurnScore: 0.20f,
            FingerStraightnessRangeMax: 0,
            HandScaleRatioRange: 0,
            TranslationDistance: 0);
        var holdSequence = GestureFeatureExtractor.Extract(GestureTemplateFactoryTests.CreateHoldSamples());
        var template = new GestureTemplate(
            Samples: [],
            SourceFrameCount: 60,
            DurationMilliseconds: 2000,
            AverageConfidence: 0.95f,
            Kind: GestureKind.Static,
            FeatureFrames: GestureFeatureExtractor.BuildUnifiedSequence(holdSequence),
            MotionSummary: new GestureMotionSummary(0.02f, 0, 0, 2000, 60),
            Topology: topology);
        var recognizer = CreateRecognizer("screenshot", template);

        var result = Feed(recognizer, GestureTemplateFactoryTests.CreateHoldSamples(count: 64));

        Assert.Null(result);
        Assert.Null(recognizer.GetDebugSnapshot().BestGestureId);
        Assert.Contains(recognizer.GetDebugSnapshot().Scores, score =>
            score.GestureId == "screenshot" && score.Reason.Contains("Needs 3 examples", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public void Recognize_RejectsPeaceSignWhenTemplateIsOpenPalm()
    {
        var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateWaveSamples(direction: 1));
        Assert.NotNull(template);
        var recognizer = CreateRecognizer("palm_flip", template);

        var result = Feed(recognizer, CreatePeaceWaveSamples(direction: 1));

        Assert.Null(result);
        Assert.Null(recognizer.GetDebugSnapshot().BestGestureId);
        Assert.Contains(recognizer.GetDebugSnapshot().Scores, score =>
            score.Breakdown is not null && score.Breakdown.FingerStateScore > 0);
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

    private static MultiHandWindowGestureRecognizer CreateTwoHandRecognizer(string id, GestureTemplate template)
    {
        var recognizer = new MultiHandWindowGestureRecognizer();
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

    private static GestureRecognitionResult? Feed(
        MultiHandWindowGestureRecognizer recognizer,
        IReadOnlyList<GestureFrameSetSample> samples)
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

    private static List<GestureFrameSample> ReverseTime(IReadOnlyList<GestureFrameSample> samples)
    {
        if (samples.Count == 0)
        {
            return [];
        }

        var first = samples[0].Timestamp;
        return samples
            .AsEnumerable()
            .Reverse()
            .Select((sample, index) => sample with { Timestamp = first + TimeSpan.FromMilliseconds(index * 33) })
            .ToList();
    }

    private static List<GestureFrameSetSample> ReverseTime(IReadOnlyList<GestureFrameSetSample> samples)
    {
        if (samples.Count == 0)
        {
            return [];
        }

        var first = samples[0].Timestamp;
        return samples
            .AsEnumerable()
            .Reverse()
            .Select((sample, index) => sample with { Timestamp = first + TimeSpan.FromMilliseconds(index * 33) })
            .ToList();
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

    private static List<GestureFrameSample> CreatePeaceWaveSamples(int direction)
    {
        return GestureTemplateFactoryTests.CreateWaveSamples(direction: direction)
            .Select(sample => sample with { Landmarks = CreatePeaceHand(sample.Landmarks) })
            .ToList();
    }

    private static List<HandLandmark> CreatePeaceHand(IReadOnlyList<HandLandmark> source)
    {
        var landmarks = source.ToList();
        CurlFinger(landmarks, 4);
        CurlFinger(landmarks, 16);
        CurlFinger(landmarks, 20);
        return landmarks;
    }

    private static void CurlFinger(List<HandLandmark> landmarks, int tipIndex)
    {
        var wrist = landmarks[0];
        var basePoint = landmarks[tipIndex - 3];
        landmarks[tipIndex - 2] = Lerp(basePoint, wrist, 0.28f);
        landmarks[tipIndex - 1] = Lerp(basePoint, wrist, 0.38f);
        landmarks[tipIndex] = Lerp(basePoint, wrist, 0.48f);
    }

    private static HandLandmark Lerp(HandLandmark a, HandLandmark b, float t)
    {
        return new HandLandmark(
            a.X + ((b.X - a.X) * t),
            a.Y + ((b.Y - a.Y) * t),
            a.Z + ((b.Z - a.Z) * t));
    }
}

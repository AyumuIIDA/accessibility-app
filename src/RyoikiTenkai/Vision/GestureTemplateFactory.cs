using RyoikiTenkai.Core;

namespace RyoikiTenkai.Vision;

internal static class GestureTemplateFactory
{
    private static readonly int[] LandmarkIds = [0, 4, 5, 8, 9, 12, 13, 16, 17, 20];
    internal const int MinimumUsableSampleCount = 40;
    internal const double MinimumSampleRateFps = 20;
    internal const double MinimumDurationMilliseconds = 1900;
    internal const float MinimumTopologyChangeScore = 0.32f;
    internal const float MinimumPalmTravel = 0.22f;
    internal const float MinimumPalmOrientationRangeRadians = 0.45f;
    internal const float MinimumHandednessRange = 0.20f;
    internal const float MinimumPalmTurnScore = 0.55f;
    internal const float MinimumPalmCompressionDrop = 0.30f;
    internal const float MinimumFingerStraightnessRange = 0.30f;
    private const float MinimumConfidence = 0.35f;
    private const int ResampledLength = 32;

    public static GestureTemplate? Create(IReadOnlyList<GestureFrameSample> sourceSamples)
    {
        return TryCreate(sourceSamples).Template;
    }

    public static GestureTemplateCreationResult TryCreate(IReadOnlyList<GestureFrameSample> sourceSamples)
    {
        var samples = sourceSamples
            .Where(x => x.Landmarks.Count >= 21 && x.Confidence >= MinimumConfidence)
            .OrderBy(x => x.Timestamp)
            .ToList();
        var orderedSourceSamples = sourceSamples
            .OrderBy(x => x.Timestamp)
            .ToList();
        var validLandmarkFrameCount = orderedSourceSamples.Count(x => x.Landmarks.Count >= 21);
        var highConfidenceFrameCount = orderedSourceSamples.Count(x =>
            x.Landmarks.Count >= 21 && x.Confidence >= MinimumConfidence);
        var sourceDurationMilliseconds = orderedSourceSamples.Count < 2
            ? 0
            : Math.Max(0, (orderedSourceSamples[^1].Timestamp - orderedSourceSamples[0].Timestamp).TotalMilliseconds);
        var usableEffectiveFps = sourceDurationMilliseconds <= 0
            ? 0
            : highConfidenceFrameCount / (sourceDurationMilliseconds / 1000.0);

        if (samples.Count < MinimumUsableSampleCount
            || sourceDurationMilliseconds < MinimumDurationMilliseconds
            || usableEffectiveFps < MinimumSampleRateFps)
        {
            return new GestureTemplateCreationResult(
                Template: null,
                SourceFrameCount: sourceSamples.Count,
                ValidLandmarkFrameCount: validLandmarkFrameCount,
                HighConfidenceFrameCount: highConfidenceFrameCount,
                MinimumConfidence: MinimumConfidence,
                DurationMilliseconds: sourceDurationMilliseconds,
                FailureReason: CreateFailureReason(
                    sourceSamples.Count,
                    validLandmarkFrameCount,
                    highConfidenceFrameCount,
                    sourceDurationMilliseconds,
                    usableEffectiveFps));
        }

        var normalized = Normalize(samples);
        var resampled = Resample(normalized, ResampledLength);
        var skeletonFrames = NormalizeSkeleton(samples);
        var featureSequence = GestureFeatureExtractor.Extract(samples);
        var featureFrames = GestureFeatureExtractor.BuildUnifiedSequence(featureSequence);
        var topology = GestureFeatureExtractor.SummarizeTopology(featureSequence.Frames);
        var featureTrack = GestureFeatureExtractor.BuildFeatureTrack(samples);
        var kind = GestureFeatureExtractor.DetectKind(featureSequence.Motion);
        if (kind == GestureKind.Static && topology.PalmTurnScore >= MinimumPalmTurnScore)
        {
            kind = GestureKind.Dynamic;
        }
        if (kind == GestureKind.Static
            && (topology.FingerStateTransitionCount > 0
                || topology.FingerStraightnessRangeMax >= MinimumFingerStraightnessRange))
        {
            kind = GestureKind.Dynamic;
        }
        if (!HasMeaningfulTopologyChange(topology))
        {
            return new GestureTemplateCreationResult(
                Template: null,
                SourceFrameCount: sourceSamples.Count,
                ValidLandmarkFrameCount: validLandmarkFrameCount,
                HighConfidenceFrameCount: highConfidenceFrameCount,
                MinimumConfidence: MinimumConfidence,
                DurationMilliseconds: sourceDurationMilliseconds,
                FailureReason: CreateTopologyFailureReason(topology));
        }

        var duration = (samples[^1].Timestamp - samples[0].Timestamp).TotalMilliseconds;
        var template = new GestureTemplate(
            Samples: resampled,
            SourceFrameCount: samples.Count,
            DurationMilliseconds: Math.Max(0, duration),
            AverageConfidence: (float)samples.Average(x => x.Confidence),
            SkeletonFrames: skeletonFrames,
            Kind: kind,
            FeatureFrames: featureFrames,
            MotionSummary: featureSequence.Motion,
            SourceSkeletonFrameCount: skeletonFrames.Count,
            ActiveSegment: featureSequence.ActiveSegment,
            Topology: topology,
            FeatureTrack: featureTrack);
        return new GestureTemplateCreationResult(
            Template: template,
            SourceFrameCount: sourceSamples.Count,
            ValidLandmarkFrameCount: validLandmarkFrameCount,
            HighConfidenceFrameCount: highConfidenceFrameCount,
            MinimumConfidence: MinimumConfidence,
            DurationMilliseconds: Math.Max(0, duration),
            FailureReason: string.Empty);
    }

    internal static GestureTemplate NormalizeLegacyTemplate(GestureTemplate template, string definitionType)
    {
        var kind = template.Kind;
        if (template.FeatureFrames is not { Count: > 0 }
            && StringComparer.OrdinalIgnoreCase.Equals(definitionType, "template"))
        {
            kind = GestureKind.Dynamic;
        }

        if (template.FeatureFrames is { Count: > 0 })
        {
            return template with { Kind = kind };
        }

        var featureFrames = template.Samples
            .Select(x => new GestureFeatureFrame(
                x.TimeOffsetMilliseconds,
                x.CenterX,
                x.CenterY,
                PalmOrientationRadians: 0,
                PalmVelocity: 0,
                x.Values,
                GestureFeatureExtractor.ReadFingerStraightness(x.Values),
                GestureFeatureExtractor.ReadFingerStateMask(x.Values)))
            .ToList();

        var motion = template.MotionSummary ?? new GestureMotionSummary(
            kind == GestureKind.Dynamic ? GestureFeatureExtractor.StaticMotionThreshold + 0.01f : 0,
            0,
            0,
            template.DurationMilliseconds,
            template.SourceFrameCount);

        return template with
        {
            Kind = kind,
            FeatureFrames = featureFrames,
            MotionSummary = motion,
            Topology = template.Topology ?? GestureFeatureExtractor.SummarizeTopology(featureFrames),
            FeatureTrack = template.FeatureTrack ?? new GestureFeatureTrack(
                featureFrames.Select(x => x.Features ?? new GestureFrameFeatures(
                    x.TimeOffsetMilliseconds,
                    x.CenterX,
                    x.CenterY,
                    x.PalmOrientationRadians,
                    x.PalmVelocity,
                    GestureFeatureExtractor.ReadSignedPalmArea(x),
                    GestureFeatureExtractor.ReadPalmCompression(x),
                    GestureFeatureExtractor.ReadPalmDepthRange(x),
                    0,
                    GestureFeatureExtractor.ReadHandedness(x),
                    0,
                    x.FingerStraightness ?? GestureFeatureExtractor.ReadFingerStraightness(x.Values),
                    x.FingerStateMask)).ToList(),
                GestureFeatureExtractor.BuildFeatureSummary(featureFrames))
        };
    }

    private static bool HasMeaningfulTopologyChange(GestureTopologySummary topology)
    {
        return topology.TopologyChangeScore >= MinimumTopologyChangeScore
            || topology.PalmTravel >= MinimumPalmTravel
            || topology.PalmOrientationRangeRadians >= MinimumPalmOrientationRangeRadians
            || topology.HandednessRange >= MinimumHandednessRange
            || topology.PalmTurnScore >= MinimumPalmTurnScore
            || topology.FingerStraightnessRangeMax >= MinimumFingerStraightnessRange
            || topology.FingerStateTransitionCount > 0;
    }

    private static string CreateTopologyFailureReason(GestureTopologySummary topology)
    {
        return
            "Captured a stable pose, not a gesture movement. " +
            $"topology score={topology.TopologyChangeScore:0.000}, travel={topology.PalmTravel:0.000}, " +
            $"palm angle={topology.PalmOrientationRangeRadians * 180 / MathF.PI:0.0} deg, " +
            $"handedness range={topology.HandednessRange:0.000}, palm turn={topology.PalmTurnScore:0.000}, " +
            $"area crossings={topology.SignedPalmAreaSignChanges}, compression drop={topology.PalmCompressionDrop:0.000}, " +
            $"finger transitions={topology.FingerStateTransitionCount}, finger curl range={topology.FingerStraightnessRangeMax:0.000}. " +
            "Move/flip/change shape more during the capture window; for grab, start with an open palm and finish with a closed fist inside the capture window.";
    }

    private static string CreateFailureReason(
        int sourceFrameCount,
        int validLandmarkFrameCount,
        int highConfidenceFrameCount,
        double durationMilliseconds,
        double usableEffectiveFps)
    {
        if (sourceFrameCount == 0)
        {
            return "No hand landmark frames were captured during the CAPTURING window.";
        }

        if (validLandmarkFrameCount == 0)
        {
            return $"Captured {sourceFrameCount} frame(s), but none contained the required 21 hand landmarks.";
        }

        if (durationMilliseconds < MinimumDurationMilliseconds)
        {
            return
                $"Captured {durationMilliseconds:0} ms, but gesture windows need about 2 seconds ({MinimumDurationMilliseconds:0}+ ms).";
        }

        if (highConfidenceFrameCount < MinimumUsableSampleCount)
        {
            return
                $"Captured {validLandmarkFrameCount} landmark frame(s), but only {highConfidenceFrameCount} met confidence >= {MinimumConfidence:0.00}; need {MinimumUsableSampleCount} usable frames over about 2 seconds for resampling.";
        }

        if (usableEffectiveFps < MinimumSampleRateFps)
        {
            return
                $"Captured {usableEffectiveFps:0.0} usable fps, but gesture recordings need {MinimumSampleRateFps:0.0}+ fps for reliable frame-by-frame recognition.";
        }

        return
            $"Captured {sourceFrameCount} frame(s) over {durationMilliseconds:0} ms, but only {highConfidenceFrameCount} usable frame(s) remained; need {MinimumUsableSampleCount}.";
    }

    internal static List<GestureTemplateSample> Normalize(IReadOnlyList<GestureFrameSample> samples)
    {
        var firstTimestamp = samples[0].Timestamp;
        var firstCenter = PalmCenter(samples[0].Landmarks);
        var scale = Math.Max(1f, (float)samples.Select(x => PalmScale(x.Landmarks)).Average());

        return samples.Select(sample =>
        {
            var center = PalmCenter(sample.Landmarks);
            var values = new float[LandmarkIds.Length * 2];
            for (var i = 0; i < LandmarkIds.Length; i++)
            {
                var point = sample.Landmarks[LandmarkIds[i]];
                values[i * 2] = (point.X - center.X) / scale;
                values[(i * 2) + 1] = (point.Y - center.Y) / scale;
            }

            return new GestureTemplateSample(
                TimeOffsetMilliseconds: (sample.Timestamp - firstTimestamp).TotalMilliseconds,
                CenterX: (center.X - firstCenter.X) / scale,
                CenterY: (center.Y - firstCenter.Y) / scale,
                Values: values);
        }).ToList();
    }

    internal static List<GestureSkeletonFrame> NormalizeSkeleton(IReadOnlyList<GestureFrameSample> samples)
    {
        var firstTimestamp = samples[0].Timestamp;
        var firstCenter = PalmCenter(samples[0].Landmarks);
        var scale = Math.Max(1f, (float)samples.Select(x => PalmScale(x.Landmarks)).Average());

        return samples.Select(sample =>
        {
            var center = PalmCenter(sample.Landmarks);
            var centerX = center.X - firstCenter.X;
            var centerY = center.Y - firstCenter.Y;
            var points = sample.Landmarks
                .Take(21)
                .Select(point => new GestureSkeletonPoint(
                    ((point.X - center.X) + centerX) / scale,
                    ((point.Y - center.Y) + centerY) / scale,
                    point.Z / scale))
                .ToList();

            return new GestureSkeletonFrame(
                TimeOffsetMilliseconds: (sample.Timestamp - firstTimestamp).TotalMilliseconds,
                Landmarks: points);
        }).ToList();
    }

    internal static List<GestureTemplateSample> Resample(IReadOnlyList<GestureTemplateSample> samples, int length)
    {
        if (samples.Count == 0 || length <= 0)
        {
            return [];
        }

        if (samples.Count == 1)
        {
            return Enumerable.Range(0, length).Select(_ => samples[0]).ToList();
        }

        var duration = Math.Max(1, samples[^1].TimeOffsetMilliseconds);
        var result = new List<GestureTemplateSample>(length);
        for (var i = 0; i < length; i++)
        {
            var targetTime = duration * i / Math.Max(1, length - 1);
            var right = 1;
            while (right < samples.Count && samples[right].TimeOffsetMilliseconds < targetTime)
            {
                right++;
            }

            if (right >= samples.Count)
            {
                result.Add(samples[^1] with { TimeOffsetMilliseconds = targetTime });
                continue;
            }

            var left = Math.Max(0, right - 1);
            var a = samples[left];
            var b = samples[right];
            var span = Math.Max(1, b.TimeOffsetMilliseconds - a.TimeOffsetMilliseconds);
            var t = (float)((targetTime - a.TimeOffsetMilliseconds) / span);
            result.Add(Interpolate(a, b, targetTime, t));
        }

        return result;
    }

    private static GestureTemplateSample Interpolate(
        GestureTemplateSample a,
        GestureTemplateSample b,
        double timeOffsetMilliseconds,
        float t)
    {
        var values = new float[Math.Min(a.Values.Length, b.Values.Length)];
        for (var i = 0; i < values.Length; i++)
        {
            values[i] = Lerp(a.Values[i], b.Values[i], t);
        }

        return new GestureTemplateSample(
            TimeOffsetMilliseconds: timeOffsetMilliseconds,
            CenterX: Lerp(a.CenterX, b.CenterX, t),
            CenterY: Lerp(a.CenterY, b.CenterY, t),
            Values: values);
    }

    private static (float X, float Y) PalmCenter(IReadOnlyList<HandLandmark> points)
    {
        return ((points[0].X + points[9].X) / 2, (points[0].Y + points[9].Y) / 2);
    }

    private static float PalmScale(IReadOnlyList<HandLandmark> points)
    {
        var dx = points[0].X - points[9].X;
        var dy = points[0].Y - points[9].Y;
        return MathF.Sqrt((dx * dx) + (dy * dy));
    }

    private static float Lerp(float a, float b, float t)
    {
        return a + ((b - a) * Math.Clamp(t, 0, 1));
    }
}

internal sealed record GestureTemplateCreationResult(
    GestureTemplate? Template,
    int SourceFrameCount,
    int ValidLandmarkFrameCount,
    int HighConfidenceFrameCount,
    float MinimumConfidence,
    double DurationMilliseconds,
    string FailureReason);

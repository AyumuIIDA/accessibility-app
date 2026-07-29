using RyoikiTenkai.Core;

namespace RyoikiTenkai.Vision;

internal static class MultiHandGestureFeatureExtractor
{
    private const float MinimumConfidence = 0.35f;
    private const float MinimumTwoHandCoverageRatio = 0.70f;
    private const float MinimumRelativeHandDistance = 0.65f;
    private const int RelativeFeatureLength = 4;

    public static bool IsDistinctTwoHandTemplate(GestureTemplate template)
    {
        return template.HandCount >= 2
            && template.MultiHandSummary is { MeanRelativeDistance: >= MinimumRelativeHandDistance };
    }

    public static int CountUsableTwoHandFrames(IReadOnlyList<GestureFrameSetSample> sourceSamples)
    {
        var count = 0;
        foreach (var sample in sourceSamples)
        {
            if (CreateOrderedPair(sample) is not null)
            {
                count++;
            }
        }

        return count;
    }

    public static bool HasPredominantTwoHandCoverage(
        IReadOnlyList<GestureFrameSetSample> sourceSamples,
        int capturedFrameCount)
    {
        var usableTwoHandFrames = CountUsableTwoHandFrames(sourceSamples);
        if (usableTwoHandFrames < GestureTemplateFactory.MinimumUsableSampleCount)
        {
            return false;
        }

        var denominator = Math.Max(1, Math.Max(capturedFrameCount, sourceSamples.Count));
        return usableTwoHandFrames / (float)denominator >= MinimumTwoHandCoverageRatio;
    }

    public static GestureFeatureSequence Extract(IReadOnlyList<GestureFrameSetSample> sourceSamples)
    {
        var ordered = sourceSamples
            .Select(CreateOrderedPair)
            .Where(x => x is not null)
            .Select(x => x!)
            .OrderBy(x => x.Timestamp)
            .ToList();
        if (ordered.Count == 0)
        {
            return new GestureFeatureSequence([], new GestureMotionSummary(0, 0, 0, 0, 0), null);
        }

        var lowSequence = GestureFeatureExtractor.Extract(ordered.Select(x => x.Low).ToList());
        var highSequence = GestureFeatureExtractor.Extract(ordered.Select(x => x.High).ToList());
        var count = Math.Min(lowSequence.Frames.Count, highSequence.Frames.Count);
        if (count == 0)
        {
            return new GestureFeatureSequence([], new GestureMotionSummary(0, 0, 0, 0, 0), null);
        }

        var firstDistance = Math.Max(0.001f, Distance(PalmCenter(ordered[0].Low), PalmCenter(ordered[0].High)));
        var frames = new List<GestureFeatureFrame>(count);
        var relativeDistanceRangeMin = float.MaxValue;
        var relativeDistanceRangeMax = 0f;
        for (var i = 0; i < count; i++)
        {
            var low = lowSequence.Frames[i];
            var high = highSequence.Frames[i];
            var rawLow = ordered[Math.Min(i, ordered.Count - 1)].Low;
            var rawHigh = ordered[Math.Min(i, ordered.Count - 1)].High;
            var lowCenter = PalmCenter(rawLow);
            var highCenter = PalmCenter(rawHigh);
            var avgScale = Math.Max(1f, (PalmScale(rawLow) + PalmScale(rawHigh)) / 2f);
            var relativeX = (highCenter.X - lowCenter.X) / avgScale;
            var relativeY = (highCenter.Y - lowCenter.Y) / avgScale;
            var relativeDistance = MathF.Sqrt((relativeX * relativeX) + (relativeY * relativeY));
            var relativeAngle = MathF.Atan2(relativeY, relativeX);
            var relativeDistanceRatio = Distance(lowCenter, highCenter) / firstDistance;
            relativeDistanceRangeMin = Math.Min(relativeDistanceRangeMin, relativeDistanceRatio);
            relativeDistanceRangeMax = Math.Max(relativeDistanceRangeMax, relativeDistanceRatio);

            var values = new float[low.Values.Length + high.Values.Length + RelativeFeatureLength];
            Array.Copy(low.Values, 0, values, 0, low.Values.Length);
            Array.Copy(high.Values, 0, values, low.Values.Length, high.Values.Length);
            var offset = low.Values.Length + high.Values.Length;
            values[offset++] = relativeX;
            values[offset++] = relativeY;
            values[offset++] = relativeDistanceRatio;
            values[offset] = relativeAngle;

            var centerX = (low.CenterX + high.CenterX) / 2f;
            var centerY = (low.CenterY + high.CenterY) / 2f;
            frames.Add(new GestureFeatureFrame(
                low.TimeOffsetMilliseconds,
                centerX,
                centerY,
                relativeAngle,
                Math.Max(low.PalmVelocity, high.PalmVelocity),
                values));
        }

        var duration = frames.Count < 2 ? 0 : Math.Max(0, frames[^1].TimeOffsetMilliseconds);
        var path = 0f;
        var peakVelocity = 0f;
        var velocitySum = 0f;
        for (var i = 1; i < frames.Count; i++)
        {
            var dx = frames[i].CenterX - frames[i - 1].CenterX;
            var dy = frames[i].CenterY - frames[i - 1].CenterY;
            var distance = MathF.Sqrt((dx * dx) + (dy * dy));
            path += distance;
            var dt = Math.Max(0.001f, (float)((frames[i].TimeOffsetMilliseconds - frames[i - 1].TimeOffsetMilliseconds) / 1000.0));
            var velocity = distance / dt;
            peakVelocity = Math.Max(peakVelocity, velocity);
            velocitySum += velocity;
        }

        var durationSeconds = Math.Max(0.001f, (float)(duration / 1000.0));
        var averageVelocity = frames.Count < 2 ? 0 : velocitySum / (frames.Count - 1);
        var relativeRange = relativeDistanceRangeMax - (relativeDistanceRangeMin == float.MaxValue ? 0 : relativeDistanceRangeMin);
        var motionScore = Math.Max(path / durationSeconds, averageVelocity) + relativeRange;
        return new GestureFeatureSequence(
            frames,
            new GestureMotionSummary(motionScore, averageVelocity, peakVelocity, duration, frames.Count),
            null);
    }

    public static GestureMultiHandFeatureSummary Summarize(IReadOnlyList<GestureFeatureFrame> frames)
    {
        if (frames.Count == 0)
        {
            return new GestureMultiHandFeatureSummary(0, 0, 0, 0, 0, 0, 0);
        }

        var offset = frames[0].Values.Length - RelativeFeatureLength;
        var distances = frames.Select(x => x.Values.Length > offset + 2 ? x.Values[offset + 2] : 1).ToList();
        var relativeDistances = frames
            .Select(x =>
            {
                var dx = x.Values.Length > offset ? x.Values[offset] : 0;
                var dy = x.Values.Length > offset + 1 ? x.Values[offset + 1] : 0;
                return MathF.Sqrt((dx * dx) + (dy * dy));
            })
            .ToList();
        var angles = UnwrapAngles(frames.Select(x => x.Values.Length > offset + 3 ? x.Values[offset + 3] : 0).ToList());
        var dx = frames[^1].Values[offset] - frames[0].Values[offset];
        var dy = frames[^1].Values[offset + 1] - frames[0].Values[offset + 1];
        var relativeTranslation = MathF.Sqrt((dx * dx) + (dy * dy));
        var distanceRange = distances.Max() - distances.Min();
        var distanceDelta = distances[^1] - distances[0];
        var angleRange = angles.Count == 0 ? 0 : angles.Max() - angles.Min();
        var lowHandedness = frames.Select(x => x.Values.Length > GestureFeatureExtractor.HandednessOffset ? x.Values[GestureFeatureExtractor.HandednessOffset] : 0).Average();
        var singleLength = (frames[0].Values.Length - RelativeFeatureLength) / 2;
        var highHandedness = frames.Select(x =>
        {
            var handednessOffset = singleLength + GestureFeatureExtractor.HandednessOffset;
            return x.Values.Length > handednessOffset ? x.Values[handednessOffset] : 0;
        }).Average();
        var topology = relativeTranslation
            + (distanceRange * 0.75f)
            + (MathF.Abs(distanceDelta) * 0.55f)
            + (angleRange * 0.35f);
        return new GestureMultiHandFeatureSummary(
            relativeTranslation,
            distanceRange,
            distanceDelta,
            angleRange,
            lowHandedness,
            highHandedness,
            topology,
            relativeDistances.Count == 0 ? 0 : relativeDistances.Average());
    }

    public static GestureTemplateCreationResult TryCreate(IReadOnlyList<GestureFrameSetSample> sourceSamples)
    {
        var samples = sourceSamples
            .Select(CreateOrderedPair)
            .Where(x => x is not null)
            .Select(x => x!)
            .OrderBy(x => x.Timestamp)
            .ToList();
        var sourceDurationMilliseconds = samples.Count < 2
            ? 0
            : Math.Max(0, (samples[^1].Timestamp - samples[0].Timestamp).TotalMilliseconds);
        var usableFps = sourceDurationMilliseconds <= 0
            ? 0
            : samples.Count * 1000.0 / sourceDurationMilliseconds;
        if (samples.Count < GestureTemplateFactory.MinimumUsableSampleCount
            || sourceDurationMilliseconds < GestureTemplateFactory.MinimumDurationMilliseconds
            || usableFps < GestureTemplateFactory.MinimumSampleRateFps)
        {
            return new GestureTemplateCreationResult(
                null,
                sourceSamples.Count,
                samples.Count,
                samples.Count,
                MinimumConfidence,
                sourceDurationMilliseconds,
                $"Need two visible hands for the whole capture window. Captured {samples.Count} usable two-hand frame(s), {usableFps:0.0} usable fps.");
        }

        var sequence = Extract(sourceSamples);
        var featureFrames = GestureFeatureExtractor.BuildUnifiedSequence(sequence);
        var summary = Summarize(sequence.Frames);
        if (summary.TopologyChangeScore < GestureTemplateFactory.MinimumTopologyChangeScore)
        {
            return new GestureTemplateCreationResult(
                null,
                sourceSamples.Count,
                samples.Count,
                samples.Count,
                MinimumConfidence,
                sourceDurationMilliseconds,
                $"Captured stable two-hand pose, not a two-hand gesture. relativeMove={summary.RelativeTranslationDistance:0.000}, distanceRange={summary.RelativeDistanceRange:0.000}, angleRange={summary.RelativeAngleRangeRadians:0.000}.");
        }

        var lowSamples = samples.Select(x => x.Low).ToList();
        var first = samples[0].Timestamp;
        var template = new GestureTemplate(
            Samples: GestureTemplateFactory.Resample(GestureTemplateFactory.Normalize(lowSamples), GestureFeatureExtractor.UnifiedSequenceLength),
            SourceFrameCount: samples.Count,
            DurationMilliseconds: sourceDurationMilliseconds,
            AverageConfidence: (float)samples.Average(x => (x.Low.Confidence + x.High.Confidence) / 2f),
            SkeletonFrames: GestureTemplateFactory.NormalizeSkeleton(lowSamples),
            Kind: GestureKind.Dynamic,
            FeatureFrames: featureFrames,
            MotionSummary: sequence.Motion,
            SourceSkeletonFrameCount: samples.Count * 2,
            HandCount: 2,
            MultiHandSummary: summary);
        return new GestureTemplateCreationResult(template, sourceSamples.Count, samples.Count, samples.Count, MinimumConfidence, (samples[^1].Timestamp - first).TotalMilliseconds, string.Empty);
    }

    private static OrderedPair? CreateOrderedPair(GestureFrameSetSample sample)
    {
        var hands = sample.Hands
            .Where(x => x.Landmarks.Count >= 21 && x.Confidence >= MinimumConfidence)
            .OrderByDescending(x => x.Confidence)
            .Take(2)
            .ToList();
        if (hands.Count < 2)
        {
            return null;
        }

        var handednessGap = MathF.Abs(hands[0].Handedness - hands[1].Handedness);
        hands = handednessGap >= 0.20f
            ? hands.OrderBy(x => x.Handedness).ToList()
            : hands.OrderBy(x => PalmCenter(x).X).ToList();
        if (RelativeHandDistance(hands[0], hands[1]) < MinimumRelativeHandDistance)
        {
            return null;
        }

        return new OrderedPair(sample.Timestamp, hands[0], hands[1]);
    }

    private static float RelativeHandDistance(GestureFrameSample low, GestureFrameSample high)
    {
        var lowCenter = PalmCenter(low);
        var highCenter = PalmCenter(high);
        var averageScale = Math.Max(1f, (PalmScale(low) + PalmScale(high)) / 2f);
        return Distance(lowCenter, highCenter) / averageScale;
    }

    private static (float X, float Y) PalmCenter(GestureFrameSample sample)
    {
        return ((sample.Landmarks[0].X + sample.Landmarks[9].X) / 2, (sample.Landmarks[0].Y + sample.Landmarks[9].Y) / 2);
    }

    private static float PalmScale(GestureFrameSample sample)
    {
        return Distance(sample.Landmarks[0], sample.Landmarks[9]);
    }

    private static float Distance((float X, float Y) a, (float X, float Y) b)
    {
        var dx = a.X - b.X;
        var dy = a.Y - b.Y;
        return MathF.Sqrt((dx * dx) + (dy * dy));
    }

    private static float Distance(HandLandmark a, HandLandmark b)
    {
        var dx = a.X - b.X;
        var dy = a.Y - b.Y;
        return MathF.Sqrt((dx * dx) + (dy * dy));
    }

    private static List<float> UnwrapAngles(IReadOnlyList<float> angles)
    {
        if (angles.Count == 0)
        {
            return [];
        }

        var result = new List<float>(angles.Count) { angles[0] };
        var previous = angles[0];
        var offset = 0f;
        for (var i = 1; i < angles.Count; i++)
        {
            var current = angles[i];
            var delta = current - previous;
            if (delta > MathF.PI)
            {
                offset -= MathF.PI * 2;
            }
            else if (delta < -MathF.PI)
            {
                offset += MathF.PI * 2;
            }

            result.Add(current + offset);
            previous = current;
        }

        return result;
    }

    private sealed record OrderedPair(DateTimeOffset Timestamp, GestureFrameSample Low, GestureFrameSample High);
}

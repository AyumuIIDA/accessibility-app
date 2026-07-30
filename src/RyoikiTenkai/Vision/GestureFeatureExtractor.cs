using RyoikiTenkai.Core;

namespace RyoikiTenkai.Vision;

internal static class GestureFeatureExtractor
{
    internal const int DynamicResampledLength = 32;
    internal const int UnifiedSequenceLength = 32;
    internal const int StaticFeatureLength = 1;
    internal const float StaticMotionThreshold = 0.18f;
    internal const float ActiveVelocityThreshold = 0.09f;
    internal const int FingerStraightnessOffset = (21 * 2) + 5 + 4;
    internal const int FingerStraightnessLength = 5;
    internal const int HandednessOffset = FingerStraightnessOffset + FingerStraightnessLength + 2;
    internal const int SignedPalmAreaOffset = HandednessOffset + 1;
    internal const int PalmCompressionOffset = SignedPalmAreaOffset + 1;
    internal const int PalmDepthRangeOffset = PalmCompressionOffset + 1;
    internal const int HandScaleRatioOffset = PalmDepthRangeOffset + 1;
    internal const int BoundingBoxAreaRatioOffset = HandScaleRatioOffset + 1;
    private const float MinimumConfidence = 0.35f;
    private const float FingerOpenThreshold = 0.55f;
    private static readonly int[] FingerBases = [1, 5, 9, 13, 17];
    private static readonly int[] FingerTips = [4, 8, 12, 16, 20];

    public static GestureFeatureSequence Extract(IReadOnlyList<GestureFrameSample> sourceSamples)
    {
        var samples = sourceSamples
            .Where(x => x.Landmarks.Count >= 21 && x.Confidence >= MinimumConfidence)
            .OrderBy(x => x.Timestamp)
            .ToList();
        if (samples.Count == 0)
        {
            return new GestureFeatureSequence([], new GestureMotionSummary(0, 0, 0, 0, 0), null);
        }

        var firstTimestamp = samples[0].Timestamp;
        var rawFrames = new List<RawFeatureFrame>(samples.Count);
        for (var i = 0; i < samples.Count; i++)
        {
            var sample = samples[i];
            var wrist = sample.Landmarks[0];
            var indexMcp = sample.Landmarks[9];
            var scale = Math.Max(1f, Distance(wrist, indexMcp));
            var orientation = MathF.Atan2(indexMcp.Y - wrist.Y, indexMcp.X - wrist.X);
            var cos = MathF.Cos(-orientation);
            var sin = MathF.Sin(-orientation);
            var centerX = (indexMcp.X - wrist.X) / scale;
            var centerY = (indexMcp.Y - wrist.Y) / scale;
            var fingerPose = AnalyzeFingerPose(sample.Landmarks);
            var timeOffsetMilliseconds = (sample.Timestamp - firstTimestamp).TotalMilliseconds;
            var features = AnalyzePalmTurn(sample, scale, orientation, timeOffsetMilliseconds, 0, 0, 0, fingerPose);
            var values = new float[(21 * 2) + 5 + 4 + 5 + 2 + 1 + 5];
            var offset = 0;
            for (var landmarkIndex = 0; landmarkIndex < 21; landmarkIndex++)
            {
                var point = sample.Landmarks[landmarkIndex];
                var x = (point.X - wrist.X) / scale;
                var y = (point.Y - wrist.Y) / scale;
                values[offset++] = (x * cos) - (y * sin);
                values[offset++] = (x * sin) + (y * cos);
            }

            for (var fingerIndex = 0; fingerIndex < FingerTips.Length; fingerIndex++)
            {
                var basePoint = sample.Landmarks[FingerBases[fingerIndex]];
                var tipPoint = sample.Landmarks[FingerTips[fingerIndex]];
                values[offset++] = Math.Clamp(Distance(basePoint, tipPoint) / scale, 0, 3);
            }

            for (var fingerIndex = 1; fingerIndex < FingerTips.Length; fingerIndex++)
            {
                var previous = sample.Landmarks[FingerTips[fingerIndex - 1]];
                var current = sample.Landmarks[FingerTips[fingerIndex]];
                values[offset++] = Math.Clamp(Distance(previous, current) / scale, 0, 3);
            }

            foreach (var straightness in fingerPose.Straightness)
            {
                values[offset++] = straightness;
            }

            values[offset++] = MathF.Sin(orientation);
            values[offset++] = MathF.Cos(orientation);
            values[offset++] = sample.Handedness;
            values[offset++] = features.SignedPalmArea;
            values[offset++] = features.PalmCompression;
            values[offset++] = features.PalmDepthRange;
            values[offset++] = 1;
            values[offset] = 1;

            rawFrames.Add(new RawFeatureFrame(
                timeOffsetMilliseconds,
                sample.Timestamp,
                wrist.X,
                wrist.Y,
                scale,
                orientation,
                centerX,
                centerY,
                values,
                fingerPose.Straightness,
                fingerPose.StateMask,
                features,
                features.BoundingBoxArea));
        }

        var peakVelocity = 0f;
        var velocitySum = 0f;
        var velocityTerms = 0;
        var pathLength = 0f;
        var previousCenterX = 0f;
        var previousCenterY = 0f;
        var firstScale = Math.Max(0.001f, rawFrames[0].Scale);
        var firstArea = Math.Max(0.001f, rawFrames[0].BoundingBoxArea);
        for (var i = 0; i < rawFrames.Count; i++)
        {
            var velocity = 0f;
            if (i > 0)
            {
                var previous = rawFrames[i - 1];
                var current = rawFrames[i];
                var dt = Math.Max(0.001, (current.Timestamp - previous.Timestamp).TotalSeconds);
                var avgScale = Math.Max(1f, (previous.Scale + current.Scale) / 2f);
                var dx = (current.WristX - previous.WristX) / avgScale;
                var dy = (current.WristY - previous.WristY) / avgScale;
                var distance = MathF.Sqrt((dx * dx) + (dy * dy));
                pathLength += distance;
                velocity = distance / (float)dt;
                velocitySum += velocity;
                velocityTerms++;
                previousCenterX += dx;
                previousCenterY += dy;
            }

            peakVelocity = Math.Max(peakVelocity, velocity);
            var scaleRatio = rawFrames[i].Scale / firstScale;
            var areaRatio = rawFrames[i].BoundingBoxArea <= 0 || firstArea <= 0.001f
                ? scaleRatio * scaleRatio
                : rawFrames[i].BoundingBoxArea / firstArea;
            rawFrames[i].Values[HandScaleRatioOffset] = scaleRatio;
            rawFrames[i].Values[BoundingBoxAreaRatioOffset] = areaRatio;
            rawFrames[i] = rawFrames[i] with
            {
                CenterX = previousCenterX,
                CenterY = previousCenterY,
                Velocity = velocity,
                Features = rawFrames[i].Features with
                {
                    PalmCenterX = previousCenterX,
                    PalmCenterY = previousCenterY,
                    PalmVelocity = velocity,
                    HandScaleRatio = scaleRatio,
                    BoundingBoxAreaRatio = areaRatio,
                    TranslationX = previousCenterX,
                    TranslationY = previousCenterY,
                    TranslationDistance = MathF.Sqrt((previousCenterX * previousCenterX) + (previousCenterY * previousCenterY))
                }
            };
        }

        var duration = rawFrames.Count < 2 ? 0 : Math.Max(0, rawFrames[^1].TimeOffsetMilliseconds);
        var durationSeconds = Math.Max(0.001f, (float)(duration / 1000.0));
        var averageVelocity = velocityTerms == 0 ? 0 : velocitySum / velocityTerms;
        var motionScore = Math.Max(pathLength / durationSeconds, averageVelocity);
        var frames = rawFrames
            .Select(x => new GestureFeatureFrame(
                x.TimeOffsetMilliseconds,
                x.CenterX,
                x.CenterY,
                x.Orientation,
                x.Velocity,
                x.Values,
                x.FingerStraightness,
                x.FingerStateMask,
                x.Features))
            .ToList();
        var summary = new GestureMotionSummary(motionScore, averageVelocity, peakVelocity, duration, frames.Count);
        return new GestureFeatureSequence(frames, summary, FindActiveSegment(frames));
    }

    public static GestureKind DetectKind(GestureMotionSummary motion)
    {
        return motion.MotionScore < StaticMotionThreshold ? GestureKind.Static : GestureKind.Dynamic;
    }

    public static List<GestureFeatureFrame> BuildUnifiedSequence(GestureFeatureSequence sequence)
    {
        return Resample(sequence.Frames, UnifiedSequenceLength);
    }

    public static GestureTopologySummary SummarizeTopology(IReadOnlyList<GestureFeatureFrame> frames)
    {
        if (frames.Count == 0)
        {
            return new GestureTopologySummary(0, 0, 0, 0, 0, 0, 0);
        }

        var palmTravel = 0f;
        for (var i = 1; i < frames.Count; i++)
        {
            var dx = frames[i].CenterX - frames[i - 1].CenterX;
            var dy = frames[i].CenterY - frames[i - 1].CenterY;
            palmTravel += MathF.Sqrt((dx * dx) + (dy * dy));
        }

        var unwrappedAngles = UnwrapAngles(frames.Select(x => x.PalmOrientationRadians).ToList());
        var orientationRange = unwrappedAngles.Count == 0
            ? 0
            : unwrappedAngles.Max() - unwrappedAngles.Min();
        var handednessValues = frames.Select(ReadHandedness).ToList();
        var handednessRange = handednessValues.Count == 0
            ? 0
            : handednessValues.Max() - handednessValues.Min();
        var transitions = 0;
        for (var i = 1; i < frames.Count; i++)
        {
            transitions += CountSetBits(frames[i - 1].FingerStateMask ^ frames[i].FingerStateMask);
        }

        var fingerStraightnessRangeMax = MaxFingerStraightnessRange(frames);
        var signedAreas = frames.Select(ReadSignedPalmArea).ToList();
        var areaRange = signedAreas.Count == 0 ? 0 : signedAreas.Max() - signedAreas.Min();
        var areaSignChanges = CountSignChanges(signedAreas);
        var compressionValues = frames.Select(ReadPalmCompression).Where(x => x > 0).ToList();
        var compressionMin = compressionValues.Count == 0 ? 0 : compressionValues.Min();
        var compressionMax = compressionValues.Count == 0 ? 0 : compressionValues.Max();
        var compressionDrop = compressionMax <= 0 ? 0 : Math.Clamp((compressionMax - compressionMin) / compressionMax, 0, 1);
        var depthMax = frames.Select(ReadPalmDepthRange).DefaultIfEmpty(0).Max();
        var scaleRatios = frames.Select(ReadHandScaleRatio).ToList();
        var scaleRatioRange = scaleRatios.Count == 0 ? 0 : scaleRatios.Max() - scaleRatios.Min();
        var scaleRatioDelta = scaleRatios.Count < 2 ? 0 : scaleRatios[^1] - scaleRatios[0];
        var areaRatios = frames.Select(ReadBoundingBoxAreaRatio).ToList();
        var areaRatioRange = areaRatios.Count == 0 ? 0 : areaRatios.Max() - areaRatios.Min();
        var areaRatioDelta = areaRatios.Count < 2 ? 0 : areaRatios[^1] - areaRatios[0];
        var translationDeltaX = frames[^1].CenterX - frames[0].CenterX;
        var translationDeltaY = frames[^1].CenterY - frames[0].CenterY;
        var translationDistance = MathF.Sqrt((translationDeltaX * translationDeltaX) + (translationDeltaY * translationDeltaY));
        var palmTurnScore = (areaSignChanges > 0 ? 0.38f : 0)
            + (compressionDrop * 0.42f)
            + Math.Clamp(areaRange * 8f, 0, 0.35f)
            + Math.Clamp(depthMax * 2.5f, 0, 0.30f);
        var sizeChangeScore = Math.Clamp(Math.Max(MathF.Abs(scaleRatioDelta), MathF.Abs(areaRatioDelta) * 0.5f), 0, 1.2f);
        var changeScore = palmTravel
            + (orientationRange * 0.6f)
            + (handednessRange * 0.8f)
            + (transitions * 0.15f)
            + (fingerStraightnessRangeMax * 0.75f)
            + palmTurnScore
            + (sizeChangeScore * 0.55f);
        return new GestureTopologySummary(
            palmTravel,
            orientationRange,
            handednessRange,
            transitions,
            frames[0].FingerStateMask,
            frames[^1].FingerStateMask,
            changeScore,
            areaRange,
            areaSignChanges,
            compressionMin,
            compressionDrop,
            depthMax,
            palmTurnScore,
            fingerStraightnessRangeMax,
            handednessValues.Count == 0 ? 0 : handednessValues.Average(),
            scaleRatioRange,
            scaleRatioDelta,
            areaRatioRange,
            areaRatioDelta,
            translationDeltaX,
            translationDeltaY,
            translationDistance);
    }

    public static GestureFeatureTrack BuildFeatureTrack(IReadOnlyList<GestureFrameSample> sourceSamples)
    {
        var sequence = Extract(sourceSamples);
        var frames = sequence.Frames
            .Select(frame => frame.Features ?? FeatureFrameToFeatures(frame))
            .ToList();
        return new GestureFeatureTrack(frames, BuildFeatureSummary(sequence.Frames));
    }

    public static GestureFeatureSummary BuildFeatureSummary(IReadOnlyList<GestureFeatureFrame> frames)
    {
        var topology = SummarizeTopology(frames);
        var compressionValues = frames.Select(ReadPalmCompression).Where(x => x > 0).ToList();
        return new GestureFeatureSummary(
            topology.PalmTravel,
            topology.PalmOrientationRangeRadians,
            topology.HandednessRange,
            topology.FingerStateTransitionCount,
            topology.StartFingerStateMask,
            topology.EndFingerStateMask,
            topology.SignedPalmAreaRange,
            topology.SignedPalmAreaSignChanges,
            topology.PalmCompressionMin,
            compressionValues.Count == 0 ? 0 : compressionValues.Max(),
            topology.PalmCompressionDrop,
            topology.PalmDepthRangeMax,
            topology.PalmTurnScore,
            topology.FingerStraightnessRangeMax,
            topology.TopologyChangeScore,
            topology.HandednessMean,
            topology.HandScaleRatioRange,
            topology.HandScaleRatioDelta,
            topology.BoundingBoxAreaRatioRange,
            topology.BoundingBoxAreaRatioDelta,
            topology.TranslationDeltaX,
            topology.TranslationDeltaY,
            topology.TranslationDistance);
    }

    public static List<GestureFeatureFrame> BuildStaticPose(IReadOnlyList<GestureFeatureFrame> frames)
    {
        if (frames.Count == 0)
        {
            return [];
        }

        var stable = frames
            .OrderBy(x => x.PalmVelocity)
            .Take(Math.Max(1, frames.Count / 2))
            .OrderBy(x => x.TimeOffsetMilliseconds)
            .ToList();
        return [AverageFrame(stable, timeOffsetMilliseconds: stable[^1].TimeOffsetMilliseconds)];
    }

    public static List<GestureFeatureFrame> BuildDynamicMotion(GestureFeatureSequence sequence)
    {
        var frames = sequence.Frames;
        if (sequence.ActiveSegment is not null)
        {
            frames = frames
                .Skip(sequence.ActiveSegment.StartIndex)
                .Take(sequence.ActiveSegment.EndIndex - sequence.ActiveSegment.StartIndex + 1)
                .ToList();
        }

        return Resample(frames, DynamicResampledLength);
    }

    public static List<GestureFeatureFrame> Resample(IReadOnlyList<GestureFeatureFrame> frames, int length)
    {
        if (frames.Count == 0 || length <= 0)
        {
            return [];
        }

        if (frames.Count == 1)
        {
            return Enumerable.Range(0, length).Select(_ => frames[0]).ToList();
        }

        var duration = Math.Max(1, frames[^1].TimeOffsetMilliseconds - frames[0].TimeOffsetMilliseconds);
        var result = new List<GestureFeatureFrame>(length);
        for (var i = 0; i < length; i++)
        {
            var targetTime = frames[0].TimeOffsetMilliseconds + (duration * i / Math.Max(1, length - 1));
            var right = 1;
            while (right < frames.Count && frames[right].TimeOffsetMilliseconds < targetTime)
            {
                right++;
            }

            if (right >= frames.Count)
            {
                result.Add(frames[^1] with { TimeOffsetMilliseconds = targetTime - frames[0].TimeOffsetMilliseconds });
                continue;
            }

            var left = Math.Max(0, right - 1);
            var a = frames[left];
            var b = frames[right];
            var span = Math.Max(1, b.TimeOffsetMilliseconds - a.TimeOffsetMilliseconds);
            var t = (float)((targetTime - a.TimeOffsetMilliseconds) / span);
            result.Add(Interpolate(a, b, targetTime - frames[0].TimeOffsetMilliseconds, t));
        }

        return result;
    }

    private static GestureActiveSegment? FindActiveSegment(IReadOnlyList<GestureFeatureFrame> frames)
    {
        var activeIndexes = frames
            .Select((frame, index) => (frame, index))
            .Where(x => x.frame.PalmVelocity >= ActiveVelocityThreshold)
            .Select(x => x.index)
            .ToList();
        if (activeIndexes.Count < 3)
        {
            return null;
        }

        var start = Math.Max(0, activeIndexes[0] - 2);
        var end = Math.Min(frames.Count - 1, activeIndexes[^1] + 2);
        var segmentFrames = frames.Skip(start).Take(end - start + 1).ToList();
        var durationSeconds = Math.Max(0.001f, (float)((segmentFrames[^1].TimeOffsetMilliseconds - segmentFrames[0].TimeOffsetMilliseconds) / 1000.0));
        var path = 0f;
        for (var i = 1; i < segmentFrames.Count; i++)
        {
            var dx = segmentFrames[i].CenterX - segmentFrames[i - 1].CenterX;
            var dy = segmentFrames[i].CenterY - segmentFrames[i - 1].CenterY;
            path += MathF.Sqrt((dx * dx) + (dy * dy));
        }

        return new GestureActiveSegment(
            start,
            end,
            frames[start].TimeOffsetMilliseconds,
            frames[end].TimeOffsetMilliseconds,
            path / durationSeconds);
    }

    private static GestureFeatureFrame AverageFrame(IReadOnlyList<GestureFeatureFrame> frames, double timeOffsetMilliseconds)
    {
        var length = frames.Min(x => x.Values.Length);
        var values = new float[length];
        var centerX = 0f;
        var centerY = 0f;
        var orientationSin = 0f;
        var orientationCos = 0f;
        foreach (var frame in frames)
        {
            centerX += frame.CenterX;
            centerY += frame.CenterY;
            orientationSin += MathF.Sin(frame.PalmOrientationRadians);
            orientationCos += MathF.Cos(frame.PalmOrientationRadians);
            for (var i = 0; i < length; i++)
            {
                values[i] += frame.Values[i];
            }
        }

        var count = Math.Max(1, frames.Count);
        for (var i = 0; i < values.Length; i++)
        {
            values[i] /= count;
        }

        return new GestureFeatureFrame(
            timeOffsetMilliseconds,
            centerX / count,
            centerY / count,
            MathF.Atan2(orientationSin / count, orientationCos / count),
            frames.Average(x => x.PalmVelocity),
            values,
            ReadFingerStraightness(values),
            ReadFingerStateMask(values),
            FeatureFrameToFeatures(timeOffsetMilliseconds, centerX / count, centerY / count, MathF.Atan2(orientationSin / count, orientationCos / count), frames.Average(x => x.PalmVelocity), values));
    }

    private static GestureFeatureFrame Interpolate(
        GestureFeatureFrame a,
        GestureFeatureFrame b,
        double timeOffsetMilliseconds,
        float t)
    {
        var values = new float[Math.Min(a.Values.Length, b.Values.Length)];
        for (var i = 0; i < values.Length; i++)
        {
            values[i] = Lerp(a.Values[i], b.Values[i], t);
        }

        return new GestureFeatureFrame(
            timeOffsetMilliseconds,
            Lerp(a.CenterX, b.CenterX, t),
            Lerp(a.CenterY, b.CenterY, t),
            LerpAngle(a.PalmOrientationRadians, b.PalmOrientationRadians, t),
            Lerp(a.PalmVelocity, b.PalmVelocity, t),
            values,
            ReadFingerStraightness(values),
            ReadFingerStateMask(values),
            InterpolateFeatures(a, b, timeOffsetMilliseconds, t, values));
    }

    public static GestureFingerPose AnalyzeFingerPose(IReadOnlyList<HandLandmark> landmarks)
    {
        if (landmarks.Count < 21)
        {
            return new GestureFingerPose([0, 0, 0, 0, 0], 0);
        }

        var wrist = landmarks[0];
        var straightness = new List<float>(FingerTips.Length);
        var mask = 0;
        for (var fingerIndex = 0; fingerIndex < FingerTips.Length; fingerIndex++)
        {
            var basePoint = landmarks[FingerBases[fingerIndex]];
            var tipPoint = landmarks[FingerTips[fingerIndex]];
            var baseDistance = Math.Max(0.001f, Distance(wrist, basePoint));
            var tipDistance = Distance(wrist, tipPoint);
            var ratio = tipDistance / baseDistance;
            var value = Math.Clamp((ratio - 1.05f) / 0.65f, 0, 1);
            straightness.Add(value);
            if (value >= FingerOpenThreshold)
            {
                mask |= 1 << fingerIndex;
            }
        }

        return new GestureFingerPose(straightness, mask);
    }

    public static string FormatFingerMask(int mask)
    {
        return string.Create(5, mask, (span, state) =>
        {
            for (var i = 0; i < span.Length; i++)
            {
                span[i] = (state & (1 << i)) == 0 ? '0' : '1';
            }
        });
    }

    internal static List<float> ReadFingerStraightness(float[] values)
    {
        if (values.Length < FingerStraightnessOffset + FingerStraightnessLength)
        {
            return [0, 0, 0, 0, 0];
        }

        return values
            .Skip(FingerStraightnessOffset)
            .Take(FingerStraightnessLength)
            .ToList();
    }

    internal static int ReadFingerStateMask(float[] values)
    {
        var straightness = ReadFingerStraightness(values);
        var mask = 0;
        for (var i = 0; i < straightness.Count; i++)
        {
            if (straightness[i] >= FingerOpenThreshold)
            {
                mask |= 1 << i;
            }
        }

        return mask;
    }

    internal static float ReadHandedness(GestureFeatureFrame frame)
    {
        return frame.Values.Length > HandednessOffset ? frame.Values[HandednessOffset] : 0;
    }

    internal static float ReadSignedPalmArea(GestureFeatureFrame frame)
    {
        return frame.Values.Length > SignedPalmAreaOffset ? frame.Values[SignedPalmAreaOffset] : frame.Features?.SignedPalmArea ?? 0;
    }

    internal static float ReadPalmCompression(GestureFeatureFrame frame)
    {
        return frame.Values.Length > PalmCompressionOffset ? frame.Values[PalmCompressionOffset] : frame.Features?.PalmCompression ?? 0;
    }

    internal static float ReadPalmDepthRange(GestureFeatureFrame frame)
    {
        return frame.Values.Length > PalmDepthRangeOffset ? frame.Values[PalmDepthRangeOffset] : frame.Features?.PalmDepthRange ?? 0;
    }

    internal static float ReadHandScaleRatio(GestureFeatureFrame frame)
    {
        return frame.Values.Length > HandScaleRatioOffset ? frame.Values[HandScaleRatioOffset] : frame.Features?.HandScaleRatio ?? 1;
    }

    internal static float ReadBoundingBoxAreaRatio(GestureFeatureFrame frame)
    {
        return frame.Values.Length > BoundingBoxAreaRatioOffset ? frame.Values[BoundingBoxAreaRatioOffset] : frame.Features?.BoundingBoxAreaRatio ?? 1;
    }

    private static GestureFrameFeatures AnalyzePalmTurn(
        GestureFrameSample sample,
        float scale,
        float orientation,
        double timeOffsetMilliseconds,
        float centerX,
        float centerY,
        float velocity,
        GestureFingerPose fingerPose)
    {
        var landmarks = sample.Landmarks;
        var wrist = landmarks[0];
        var middleMcp = landmarks[9];
        var indexMcp = landmarks[5];
        var pinkyMcp = landmarks[17];
        var palmAxisLength = Math.Max(0.001f, Distance(wrist, middleMcp));
        var signedPalmArea = (((indexMcp.X - wrist.X) * (pinkyMcp.Y - wrist.Y))
            - ((indexMcp.Y - wrist.Y) * (pinkyMcp.X - wrist.X))) / Math.Max(0.001f, scale * scale);
        var palmCompression = Distance(indexMcp, pinkyMcp) / palmAxisLength;
        var zValues = landmarks.Take(21).Select(x => x.Z).ToList();
        var depthRange = (zValues.Max() - zValues.Min()) / Math.Max(0.001f, scale);
        var bboxAspect = 0f;
        var bboxArea = 0f;
        if (sample.BoundingBox is { } box)
        {
            var width = Math.Max(0, box.X2 - box.X1);
            var height = Math.Max(0.001f, box.Y2 - box.Y1);
            bboxAspect = width / height;
            bboxArea = width * height;
        }

        return new GestureFrameFeatures(
            timeOffsetMilliseconds,
            centerX,
            centerY,
            orientation,
            velocity,
            signedPalmArea,
            palmCompression,
            depthRange,
            bboxAspect,
            sample.Handedness,
            sample.Confidence,
            fingerPose.Straightness,
            fingerPose.StateMask,
            scale,
            1,
            bboxArea,
            1,
            centerX,
            centerY,
            MathF.Sqrt((centerX * centerX) + (centerY * centerY)));
    }

    private static GestureFrameFeatures FeatureFrameToFeatures(
        double timeOffsetMilliseconds,
        float centerX,
        float centerY,
        float orientation,
        float velocity,
        float[] values)
    {
        return new GestureFrameFeatures(
            timeOffsetMilliseconds,
            centerX,
            centerY,
            orientation,
            velocity,
            values.Length > SignedPalmAreaOffset ? values[SignedPalmAreaOffset] : 0,
            values.Length > PalmCompressionOffset ? values[PalmCompressionOffset] : 0,
            values.Length > PalmDepthRangeOffset ? values[PalmDepthRangeOffset] : 0,
            0,
            values.Length > HandednessOffset ? values[HandednessOffset] : 0,
            0,
            ReadFingerStraightness(values),
            ReadFingerStateMask(values),
            0,
            values.Length > HandScaleRatioOffset ? values[HandScaleRatioOffset] : 1,
            0,
            values.Length > BoundingBoxAreaRatioOffset ? values[BoundingBoxAreaRatioOffset] : 1,
            centerX,
            centerY,
            MathF.Sqrt((centerX * centerX) + (centerY * centerY)));
    }

    private static GestureFrameFeatures FeatureFrameToFeatures(GestureFeatureFrame frame)
    {
        return FeatureFrameToFeatures(
            frame.TimeOffsetMilliseconds,
            frame.CenterX,
            frame.CenterY,
            frame.PalmOrientationRadians,
            frame.PalmVelocity,
            frame.Values);
    }

    private static GestureFrameFeatures InterpolateFeatures(
        GestureFeatureFrame a,
        GestureFeatureFrame b,
        double timeOffsetMilliseconds,
        float t,
        float[] values)
    {
        var af = a.Features ?? FeatureFrameToFeatures(a);
        var bf = b.Features ?? FeatureFrameToFeatures(b);
        return new GestureFrameFeatures(
            timeOffsetMilliseconds,
            Lerp(af.PalmCenterX, bf.PalmCenterX, t),
            Lerp(af.PalmCenterY, bf.PalmCenterY, t),
            LerpAngle(af.PalmAxisRadians, bf.PalmAxisRadians, t),
            Lerp(af.PalmVelocity, bf.PalmVelocity, t),
            values.Length > SignedPalmAreaOffset ? values[SignedPalmAreaOffset] : 0,
            values.Length > PalmCompressionOffset ? values[PalmCompressionOffset] : 0,
            values.Length > PalmDepthRangeOffset ? values[PalmDepthRangeOffset] : 0,
            Lerp(af.BoundingBoxAspect, bf.BoundingBoxAspect, t),
            Lerp(af.Handedness, bf.Handedness, t),
            Lerp(af.Confidence, bf.Confidence, t),
            ReadFingerStraightness(values),
            ReadFingerStateMask(values),
            Lerp(af.HandScale, bf.HandScale, t),
            values.Length > HandScaleRatioOffset ? values[HandScaleRatioOffset] : Lerp(af.HandScaleRatio, bf.HandScaleRatio, t),
            Lerp(af.BoundingBoxArea, bf.BoundingBoxArea, t),
            values.Length > BoundingBoxAreaRatioOffset ? values[BoundingBoxAreaRatioOffset] : Lerp(af.BoundingBoxAreaRatio, bf.BoundingBoxAreaRatio, t),
            Lerp(af.TranslationX, bf.TranslationX, t),
            Lerp(af.TranslationY, bf.TranslationY, t),
            Lerp(af.TranslationDistance, bf.TranslationDistance, t));
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

    private static int CountSetBits(int value)
    {
        var count = 0;
        while (value != 0)
        {
            count += value & 1;
            value >>= 1;
        }

        return count;
    }

    private static int CountSignChanges(IReadOnlyList<float> values)
    {
        var changes = 0;
        var previous = 0;
        foreach (var value in values)
        {
            var sign = MathF.Abs(value) < 0.002f ? 0 : MathF.Sign(value);
            if (sign == 0)
            {
                continue;
            }

            if (previous != 0 && sign != previous)
            {
                changes++;
            }

            previous = sign;
        }

        return changes;
    }

    private static float MaxFingerStraightnessRange(IReadOnlyList<GestureFeatureFrame> frames)
    {
        var maxRange = 0f;
        for (var fingerIndex = 0; fingerIndex < FingerStraightnessLength; fingerIndex++)
        {
            var values = frames
                .Select(frame =>
                {
                    var straightness = frame.FingerStraightness ?? ReadFingerStraightness(frame.Values);
                    return fingerIndex < straightness.Count ? straightness[fingerIndex] : 0;
                })
                .ToList();
            if (values.Count > 0)
            {
                maxRange = Math.Max(maxRange, values.Max() - values.Min());
            }
        }

        return maxRange;
    }

    private static float Lerp(float a, float b, float t)
    {
        return a + ((b - a) * Math.Clamp(t, 0, 1));
    }

    private static float LerpAngle(float a, float b, float t)
    {
        var delta = b - a;
        while (delta > MathF.PI)
        {
            delta -= MathF.PI * 2;
        }

        while (delta < -MathF.PI)
        {
            delta += MathF.PI * 2;
        }

        return a + (delta * Math.Clamp(t, 0, 1));
    }

    private static float Distance(HandLandmark a, HandLandmark b)
    {
        var dx = a.X - b.X;
        var dy = a.Y - b.Y;
        return MathF.Sqrt((dx * dx) + (dy * dy));
    }

    private sealed record RawFeatureFrame(
        double TimeOffsetMilliseconds,
        DateTimeOffset Timestamp,
        float WristX,
        float WristY,
        float Scale,
        float Orientation,
        float CenterX,
        float CenterY,
        float[] Values,
        List<float> FingerStraightness,
        int FingerStateMask,
        GestureFrameFeatures Features,
        float BoundingBoxArea)
    {
        public float Velocity { get; init; }
    }
}

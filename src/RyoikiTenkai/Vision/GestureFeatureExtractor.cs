using RyoikiTenkai.Core;

namespace RyoikiTenkai.Vision;

internal static class GestureFeatureExtractor
{
    internal const int DynamicResampledLength = 32;
    internal const int StaticFeatureLength = 1;
    internal const float StaticMotionThreshold = 0.18f;
    internal const float ActiveVelocityThreshold = 0.09f;
    private const float MinimumConfidence = 0.35f;
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
            var values = new float[(21 * 2) + 5 + 4 + 2 + 1];
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

            values[offset++] = MathF.Sin(orientation);
            values[offset++] = MathF.Cos(orientation);
            values[offset] = sample.Handedness;

            rawFrames.Add(new RawFeatureFrame(
                (sample.Timestamp - firstTimestamp).TotalMilliseconds,
                sample.Timestamp,
                wrist.X,
                wrist.Y,
                scale,
                orientation,
                centerX,
                centerY,
                values));
        }

        var peakVelocity = 0f;
        var velocitySum = 0f;
        var velocityTerms = 0;
        var pathLength = 0f;
        var previousCenterX = 0f;
        var previousCenterY = 0f;
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
            rawFrames[i] = rawFrames[i] with
            {
                CenterX = previousCenterX,
                CenterY = previousCenterY,
                Velocity = velocity
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
                x.Values))
            .ToList();
        var summary = new GestureMotionSummary(motionScore, averageVelocity, peakVelocity, duration, frames.Count);
        return new GestureFeatureSequence(frames, summary, FindActiveSegment(frames));
    }

    public static GestureKind DetectKind(GestureMotionSummary motion)
    {
        return motion.MotionScore < StaticMotionThreshold ? GestureKind.Static : GestureKind.Dynamic;
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
            values);
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
            values);
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
        float[] Values)
    {
        public float Velocity { get; init; }
    }
}

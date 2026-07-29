using System.Text.Json;
using System.Text.Json.Serialization;
using RyoikiTenkai.Core;

namespace RyoikiTenkai.Vision;

internal sealed class GestureDebugSession
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = false,
        Converters = { new JsonStringEnumConverter() }
    };

    private readonly TimeSpan _maxDuration;
    private readonly int _maxFrameCount;
    private readonly List<GestureDebugFrame> _frames = [];
    private long _nextSequenceNumber;

    public GestureDebugSession(TimeSpan maxDuration, int maxFrameCount)
    {
        _maxDuration = maxDuration;
        _maxFrameCount = Math.Max(1, maxFrameCount);
    }

    public IReadOnlyList<GestureDebugFrame> Frames => _frames;

    public long DroppedFrameCount { get; private set; }

    public GestureDebugFrame? Latest => _frames.Count == 0 ? null : _frames[^1];

    public GestureDebugFrame AddHandFrame(
        GestureFrameSample sample,
        GestureRecognitionDebugSnapshot snapshot,
        GestureRecognitionResult? recognition,
        bool hasBoundAction,
        string source,
        string elapsedText,
        GestureFrameSetSample? frameSet = null)
    {
        var frame = new GestureDebugFrame(
            SequenceNumber: ++_nextSequenceNumber,
            Kind: GestureDebugFrameKind.Hand,
            Timestamp: sample.Timestamp,
            Source: source,
            ElapsedText: elapsedText,
            Sample: sample,
            FrameSet: frameSet,
            NormalizedSkeleton: CreateNormalizedSkeleton(sample),
            Snapshot: snapshot,
            Recognition: recognition,
            ClosestScore: FindClosestScore(snapshot),
            HasBoundAction: hasBoundAction,
            ActionState: CreateActionState(recognition, hasBoundAction),
            NoHandReason: string.Empty);
        Add(frame);
        return frame;
    }

    public GestureDebugFrame AddNoHandFrame(
        DateTimeOffset timestamp,
        string reason,
        GestureRecognitionDebugSnapshot snapshot)
    {
        var frame = new GestureDebugFrame(
            SequenceNumber: ++_nextSequenceNumber,
            Kind: GestureDebugFrameKind.NoHand,
            Timestamp: timestamp,
            Source: "no-hand",
            ElapsedText: string.Empty,
            Sample: null,
            FrameSet: null,
            NormalizedSkeleton: null,
            Snapshot: snapshot,
            Recognition: null,
            ClosestScore: null,
            HasBoundAction: false,
            ActionState: "recognizer reset",
            NoHandReason: reason);
        Add(frame);
        return frame;
    }

    public void Clear()
    {
        _frames.Clear();
        DroppedFrameCount = 0;
        _nextSequenceNumber = 0;
    }

    public GestureDebugFrame? FrameAtSliderValue(double value)
    {
        if (_frames.Count == 0)
        {
            return null;
        }

        var index = Math.Clamp((int)Math.Round(value), 0, _frames.Count - 1);
        return _frames[index];
    }

    public void ExportJsonLines(string path)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        using var writer = new StreamWriter(path, append: false);
        foreach (var frame in _frames)
        {
            writer.WriteLine(JsonSerializer.Serialize(CreateExportRecord(frame), JsonOptions));
        }
    }

    public static void AppendJsonLine(string path, GestureDebugFrame frame)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.AppendAllText(
            path,
            JsonSerializer.Serialize(CreateExportRecord(frame), JsonOptions) + Environment.NewLine);
    }

    private void Add(GestureDebugFrame frame)
    {
        _frames.Add(frame);
        Trim(frame.Timestamp);
    }

    private void Trim(DateTimeOffset latestTimestamp)
    {
        var cutoff = latestTimestamp - _maxDuration;
        var removeCount = 0;
        while (removeCount < _frames.Count
            && (_frames.Count - removeCount > _maxFrameCount
                || _frames[removeCount].Timestamp < cutoff))
        {
            removeCount++;
        }

        if (removeCount <= 0)
        {
            return;
        }

        _frames.RemoveRange(0, removeCount);
        DroppedFrameCount += removeCount;
    }

    private static GestureSkeletonFrame? CreateNormalizedSkeleton(GestureFrameSample sample)
    {
        if (sample.Landmarks.Count < 21)
        {
            return null;
        }

        return GestureTemplateFactory.NormalizeSkeleton([sample]).FirstOrDefault();
    }

    private static GestureTemplateScore? FindClosestScore(GestureRecognitionDebugSnapshot snapshot)
    {
        return snapshot.Scores
            .Where(x => x.TemplateIndex > 0 && float.IsFinite(x.Score))
            .OrderBy(x => x.Score)
            .FirstOrDefault();
    }

    private static string CreateActionState(GestureRecognitionResult? recognition, bool hasBoundAction)
    {
        if (recognition is null)
        {
            return "no confirmed gesture";
        }

        return hasBoundAction ? "binding ready" : "no binding";
    }

    private static object CreateExportRecord(GestureDebugFrame frame)
    {
        return new
        {
            frame.SequenceNumber,
            Kind = frame.Kind.ToString(),
            frame.Timestamp,
            frame.Source,
            frame.ElapsedText,
            frame.ActionState,
            frame.HasBoundAction,
            frame.NoHandReason,
            Sample = frame.Sample is null ? null : new
            {
                frame.Sample.Timestamp,
                frame.Sample.Confidence,
                frame.Sample.BoundingBox,
                frame.Sample.Handedness,
                frame.Sample.Landmarks,
                FingerPose = frame.Sample.Landmarks.Count >= 21
                    ? GestureFeatureExtractor.AnalyzeFingerPose(frame.Sample.Landmarks)
                    : null
            },
            FrameSet = frame.FrameSet is null ? null : new
            {
                frame.FrameSet.Timestamp,
                Hands = frame.FrameSet.Hands.Select(hand => new
                {
                    hand.Confidence,
                    hand.Handedness,
                    hand.BoundingBox,
                    hand.Landmarks,
                    FingerPose = hand.Landmarks.Count >= 21
                        ? GestureFeatureExtractor.AnalyzeFingerPose(hand.Landmarks)
                        : null
                }).ToList()
            },
            Snapshot = new
            {
                frame.Snapshot.TriggerState,
                Path = frame.Snapshot.DetectedPath.ToString(),
                frame.Snapshot.MotionScore,
                frame.Snapshot.BufferFrameCount,
                frame.Snapshot.UsableFrameCount,
                frame.Snapshot.BufferDurationMilliseconds,
                frame.Snapshot.BufferEffectiveFps,
                frame.Snapshot.UsableEffectiveFps,
                frame.Snapshot.CandidateFailureReason,
                frame.Snapshot.RejectionReason,
                frame.Snapshot.MatchThreshold,
                frame.Snapshot.BestGestureId,
                frame.Snapshot.BestDisplayName,
                frame.Snapshot.BestConfidence,
                frame.Snapshot.StaticPoseScore,
                frame.Snapshot.DtwScore,
                frame.Snapshot.DtwWarpRatio,
                frame.Snapshot.ScoreBreakdown,
                frame.Snapshot.ActiveSegmentStartMilliseconds,
                frame.Snapshot.ActiveSegmentEndMilliseconds,
                frame.Snapshot.ConfirmedMatch,
                frame.Snapshot.WindowFrames
            },
            ClosestScore = frame.ClosestScore,
            Scores = frame.Snapshot.Scores,
            Candidate = CreateTemplateSummary(frame.Snapshot.CandidateTemplate),
            BestTemplate = CreateTemplateSummary(frame.Snapshot.BestTemplate),
            frame.Snapshot.DtwPath
        };
    }

    private static object? CreateTemplateSummary(GestureTemplate? template)
    {
        if (template is null)
        {
            return null;
        }

        return new
        {
            Kind = template.Kind.ToString(),
            template.SourceFrameCount,
            template.SourceSkeletonFrameCount,
            template.DurationMilliseconds,
            template.AverageConfidence,
            template.MotionSummary,
            template.ActiveSegment,
            template.Topology,
            template.SourceRecordingId,
            SampleCount = template.Samples.Count,
            FeatureFrameCount = template.FeatureFrames?.Count ?? 0,
            SkeletonFrameCount = template.SkeletonFrames?.Count ?? 0,
            template.Samples,
            template.FeatureFrames,
            FingerStates = template.FeatureFrames?.Select(x => new
            {
                x.TimeOffsetMilliseconds,
                Straightness = x.FingerStraightness,
                Mask = x.FingerStateMask,
                MaskText = GestureFeatureExtractor.FormatFingerMask(x.FingerStateMask)
            }).ToList(),
            template.SkeletonFrames
        };
    }
}

internal sealed record GestureDebugFrame(
    long SequenceNumber,
    GestureDebugFrameKind Kind,
    DateTimeOffset Timestamp,
    string Source,
    string ElapsedText,
    GestureFrameSample? Sample,
    GestureFrameSetSample? FrameSet,
    GestureSkeletonFrame? NormalizedSkeleton,
    GestureRecognitionDebugSnapshot Snapshot,
    GestureRecognitionResult? Recognition,
    GestureTemplateScore? ClosestScore,
    bool HasBoundAction,
    string ActionState,
    string NoHandReason);

internal enum GestureDebugFrameKind
{
    Hand,
    NoHand
}

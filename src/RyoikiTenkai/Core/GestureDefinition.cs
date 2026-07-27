namespace RyoikiTenkai.Core;

internal enum GestureKind
{
    Static,
    Dynamic
}

internal enum GestureRecognitionPath
{
    Unknown,
    Static,
    Dynamic
}

internal sealed record GestureDefinition(
    string Id,
    string DisplayName,
    string Type,
    List<GestureTemplate> Templates,
    DateTimeOffset CreatedAt);

internal sealed record GestureTemplate(
    List<GestureTemplateSample> Samples,
    int SourceFrameCount,
    double DurationMilliseconds,
    float AverageConfidence,
    List<GestureSkeletonFrame>? SkeletonFrames = null,
    GestureKind Kind = GestureKind.Dynamic,
    List<GestureFeatureFrame>? FeatureFrames = null,
    GestureMotionSummary? MotionSummary = null,
    int SourceSkeletonFrameCount = 0,
    GestureActiveSegment? ActiveSegment = null);

internal sealed record GestureTemplateSample(
    double TimeOffsetMilliseconds,
    float CenterX,
    float CenterY,
    float[] Values);

internal sealed record GestureSkeletonFrame(
    double TimeOffsetMilliseconds,
    List<GestureSkeletonPoint> Landmarks);

internal readonly record struct GestureSkeletonPoint(
    float X,
    float Y,
    float Z);

internal sealed record GestureFeatureFrame(
    double TimeOffsetMilliseconds,
    float CenterX,
    float CenterY,
    float PalmOrientationRadians,
    float PalmVelocity,
    float[] Values);

internal sealed record GestureFeatureSequence(
    List<GestureFeatureFrame> Frames,
    GestureMotionSummary Motion,
    GestureActiveSegment? ActiveSegment);

internal sealed record GestureMotionSummary(
    float MotionScore,
    float AveragePalmVelocity,
    float PeakPalmVelocity,
    double DurationMilliseconds,
    int UsableFrameCount);

internal sealed record GestureActiveSegment(
    int StartIndex,
    int EndIndex,
    double StartMilliseconds,
    double EndMilliseconds,
    float MotionScore);

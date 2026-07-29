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
    Dynamic,
    UnifiedSequence
}

internal sealed record GestureDefinition(
    string Id,
    string DisplayName,
    string Type,
    List<GestureTemplate> Templates,
    DateTimeOffset CreatedAt,
    List<GestureRecording>? Recordings = null,
    int SchemaVersion = 3,
    DateTimeOffset? UpdatedAt = null);

internal sealed record GestureRecording(
    string Id,
    int TakeIndex,
    DateTimeOffset CapturedAt,
    double DurationMilliseconds,
    float AverageConfidence,
    GestureRecordingQuality Quality,
    List<GestureRecordingFrame> Frames,
    GestureFeatureTrack? FeatureTrack = null);

internal sealed record GestureRecordingQuality(
    bool Accepted,
    int SourceFrameCount,
    int ValidLandmarkFrameCount,
    int HighConfidenceFrameCount,
    double EffectiveFps,
    double UsableEffectiveFps,
    string Reason);

internal sealed record GestureRecordingFrame(
    double TimeOffsetMilliseconds,
    float Confidence,
    float Handedness,
    GestureRecordingBox? BoundingBox,
    List<GestureSkeletonPoint> Landmarks,
    List<float>? FingerStraightness = null,
    int FingerStateMask = 0,
    GestureFrameFeatures? Features = null);

internal sealed record GestureRecordingBox(
    float X1,
    float Y1,
    float X2,
    float Y2);

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
    GestureActiveSegment? ActiveSegment = null,
    string? SourceRecordingId = null,
    GestureTopologySummary? Topology = null,
    GestureFeatureTrack? FeatureTrack = null);

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
    float[] Values,
    List<float>? FingerStraightness = null,
    int FingerStateMask = 0,
    GestureFrameFeatures? Features = null);

internal sealed record GestureFeatureTrack(
    List<GestureFrameFeatures> Frames,
    GestureFeatureSummary Summary);

internal sealed record GestureFrameFeatures(
    double TimeOffsetMilliseconds,
    float PalmCenterX,
    float PalmCenterY,
    float PalmAxisRadians,
    float PalmVelocity,
    float SignedPalmArea,
    float PalmCompression,
    float PalmDepthRange,
    float BoundingBoxAspect,
    float Handedness,
    float Confidence,
    List<float> FingerStraightness,
    int FingerStateMask);

internal sealed record GestureFeatureSummary(
    float PalmTravel,
    float PalmAxisRangeRadians,
    float HandednessRange,
    int FingerStateTransitionCount,
    int StartFingerStateMask,
    int EndFingerStateMask,
    float SignedPalmAreaRange,
    int SignedPalmAreaSignChanges,
    float PalmCompressionMin,
    float PalmCompressionMax,
    float PalmCompressionDrop,
    float PalmDepthRangeMax,
    float PalmTurnScore,
    float FingerStraightnessRangeMax,
    float TopologyChangeScore);

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

internal sealed record GestureTopologySummary(
    float PalmTravel,
    float PalmOrientationRangeRadians,
    float HandednessRange,
    int FingerStateTransitionCount,
    int StartFingerStateMask,
    int EndFingerStateMask,
    float TopologyChangeScore,
    float SignedPalmAreaRange = 0,
    int SignedPalmAreaSignChanges = 0,
    float PalmCompressionMin = 0,
    float PalmCompressionDrop = 0,
    float PalmDepthRangeMax = 0,
    float PalmTurnScore = 0,
    float FingerStraightnessRangeMax = 0);

internal sealed record GestureScoreBreakdown(
    float JointScore,
    float BoneScore,
    float CurlScore,
    float FingerStateScore,
    float SpacingScore,
    float MotionScore,
    float PalmTurnScore,
    float DepthScore,
    float TotalScore);

internal sealed record GestureFingerPose(
    List<float> Straightness,
    int StateMask);

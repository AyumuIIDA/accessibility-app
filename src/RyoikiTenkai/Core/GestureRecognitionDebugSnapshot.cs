namespace RyoikiTenkai.Core;

internal sealed record GestureRecognitionDebugSnapshot(
    DateTimeOffset Timestamp,
    int BufferFrameCount,
    int UsableFrameCount,
    double BufferDurationMilliseconds,
    double BufferEffectiveFps,
    double UsableEffectiveFps,
    GestureTemplate? CandidateTemplate,
    string CandidateFailureReason,
    IReadOnlyList<GestureTemplateScore> Scores,
    string? BestGestureId,
    string? BestDisplayName,
    float? BestConfidence,
    GestureTemplate? BestTemplate,
    float MatchThreshold,
    string TriggerState,
    GestureConfirmedMatch? ConfirmedMatch,
    GestureRecognitionPath DetectedPath = GestureRecognitionPath.Unknown,
    float MotionScore = 0,
    double? ActiveSegmentStartMilliseconds = null,
    double? ActiveSegmentEndMilliseconds = null,
    float? StaticPoseScore = null,
    float? DtwScore = null,
    float? DtwWarpRatio = null,
    string RejectionReason = "",
    IReadOnlyList<GestureDtwPoint>? DtwPath = null,
    IReadOnlyList<GestureDebugWindowFrame>? WindowFrames = null,
    GestureScoreBreakdown? ScoreBreakdown = null);

internal sealed record GestureConfirmedMatch(
    string GestureId,
    string DisplayName,
    float Confidence,
    DateTimeOffset Timestamp);

internal sealed record GestureTemplateScore(
    string GestureId,
    string DisplayName,
    int TemplateIndex,
    float Score,
    float Confidence,
    bool IsEligible,
    string Reason,
    bool IsBest,
    GestureKind Kind = GestureKind.Dynamic,
    float? WarpRatio = null,
    GestureScoreBreakdown? Breakdown = null);

internal readonly record struct GestureDtwPoint(int CandidateIndex, int TemplateIndex);

internal sealed record GestureDebugWindowFrame(
    int Index,
    double TimeOffsetMilliseconds,
    double AgeMilliseconds,
    float Confidence,
    bool IsUsable,
    bool IsAcceptedMatch);

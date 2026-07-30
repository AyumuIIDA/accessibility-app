namespace RyoikiTenkai.Core;

internal sealed record GestureRecognitionResult(
    string GestureId,
    string DisplayName,
    float Confidence,
    string Source,
    TimeSpan MatchedWindowDuration);

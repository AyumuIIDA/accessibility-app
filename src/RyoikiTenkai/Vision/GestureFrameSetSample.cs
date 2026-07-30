namespace RyoikiTenkai.Vision;

internal sealed record GestureFrameSetSample(
    DateTimeOffset Timestamp,
    IReadOnlyList<GestureFrameSample> Hands);

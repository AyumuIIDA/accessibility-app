namespace RyoikiTenkai.Vision;

internal sealed record GestureFrameSample(
    DateTimeOffset Timestamp,
    IReadOnlyList<HandLandmark> Landmarks,
    float Confidence,
    HandBox? BoundingBox,
    float Handedness);

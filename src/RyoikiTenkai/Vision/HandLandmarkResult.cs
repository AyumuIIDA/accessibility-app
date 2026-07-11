namespace RyoikiTenkai.Vision;

internal sealed record HandLandmarkResult(
    IReadOnlyList<HandLandmark> Landmarks,
    float Confidence,
    HandBox? BoundingBox = null,
    float Handedness = 0);

internal sealed record HandBox(
    float X1,
    float Y1,
    float X2,
    float Y2);

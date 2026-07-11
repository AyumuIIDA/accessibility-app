namespace RyoikiTenkai.Vision;

internal sealed class MediaPipeLandmarkGestureModel : IFrameGestureModel
{
    private readonly IHandLandmarkModel _handLandmarkModel;

    public MediaPipeLandmarkGestureModel(IHandLandmarkModel handLandmarkModel)
    {
        _handLandmarkModel = handLandmarkModel;
    }

    public FrameGesturePrediction? Predict(CameraFrame frame)
    {
        var result = _handLandmarkModel.Detect(frame);
        if (result is null || result.Landmarks.Count < 21)
        {
            return null;
        }

        var gestureId = Classify(result.Landmarks);
        if (gestureId is null)
        {
            return null;
        }

        return new FrameGesturePrediction(gestureId, result.Confidence);
    }

    internal static string? Classify(IReadOnlyList<HandLandmark> points)
    {
        var thumbOpen = Distance(points[4], points[5]) > Distance(points[4], points[17]) * 0.55f;
        var indexOpen = IsFingerRaised(points, tip: 8, pip: 6);
        var middleOpen = IsFingerRaised(points, tip: 12, pip: 10);
        var ringOpen = IsFingerRaised(points, tip: 16, pip: 14);
        var pinkyOpen = IsFingerRaised(points, tip: 20, pip: 18);

        var indexExtended = IsFingerExtended(points, tip: 8, pip: 6, mcp: 5);
        var middleExtended = IsFingerExtended(points, tip: 12, pip: 10, mcp: 9);
        var ringFolded = IsFingerFolded(points, tip: 16, pip: 14, mcp: 13);
        var pinkyFolded = IsFingerFolded(points, tip: 20, pip: 18, mcp: 17);

        if (indexExtended && middleExtended && ringFolded && pinkyFolded)
        {
            return "peace";
        }

        var openCount = new[] { thumbOpen, indexOpen, middleOpen, ringOpen, pinkyOpen }.Count(x => x);
        if (openCount >= 4)
        {
            return "open_palm";
        }

        if (openCount <= 1)
        {
            return "fist";
        }

        var pinchDistance = Distance(points[4], points[8]);
        var palmSize = Distance(points[0], points[9]);
        if (palmSize > 0 && pinchDistance / palmSize < 0.45f)
        {
            return "pinch";
        }

        return null;
    }

    private static bool IsFingerRaised(IReadOnlyList<HandLandmark> points, int tip, int pip)
    {
        return points[tip].Y < points[pip].Y - PalmScale(points) * 0.05f;
    }

    private static bool IsFingerExtended(IReadOnlyList<HandLandmark> points, int tip, int pip, int mcp)
    {
        var palmScale = PalmScale(points);
        return points[tip].Y < points[pip].Y - palmScale * 0.04f
            && Distance(points[tip], points[mcp]) > Distance(points[pip], points[mcp]) * 1.08f;
    }

    private static bool IsFingerFolded(IReadOnlyList<HandLandmark> points, int tip, int pip, int mcp)
    {
        var palmScale = PalmScale(points);
        return points[tip].Y > points[pip].Y - palmScale * 0.02f
            || Distance(points[tip], points[mcp]) < Distance(points[pip], points[mcp]) * 1.15f;
    }

    private static float PalmScale(IReadOnlyList<HandLandmark> points)
    {
        return MathF.Max(1, Distance(points[0], points[9]));
    }

    private static float Distance(HandLandmark a, HandLandmark b)
    {
        var dx = a.X - b.X;
        var dy = a.Y - b.Y;
        var dz = a.Z - b.Z;
        return MathF.Sqrt(dx * dx + dy * dy + dz * dz);
    }
}

namespace RyoikiTenkai.Vision;

internal interface IHandLandmarkModel
{
    HandLandmarkResult? Detect(CameraFrame frame);

    IReadOnlyList<HandLandmarkResult> DetectHands(CameraFrame frame);
}

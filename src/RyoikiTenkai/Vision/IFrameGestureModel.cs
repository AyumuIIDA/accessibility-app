namespace RyoikiTenkai.Vision;

internal interface IFrameGestureModel
{
    FrameGesturePrediction? Predict(CameraFrame frame);
}

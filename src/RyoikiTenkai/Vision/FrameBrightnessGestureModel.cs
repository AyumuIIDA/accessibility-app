namespace RyoikiTenkai.Vision;

internal sealed class FrameBrightnessGestureModel : IFrameGestureModel
{
    private readonly string _gestureId;
    private double? _baselineBrightness;

    public FrameBrightnessGestureModel(string gestureId)
    {
        _gestureId = gestureId;
    }

    public FrameGesturePrediction? Predict(CameraFrame frame)
    {
        var brightness = CalculateAverageBrightness(frame.Bgra);
        _baselineBrightness ??= brightness;

        var delta = brightness - _baselineBrightness.Value;
        Console.WriteLine($"Camera model: brightness={brightness:0.0}, baseline={_baselineBrightness:0.0}, delta={delta:0.0}");

        if (delta < 18.0)
        {
            return null;
        }

        var confidence = Math.Clamp((float)(delta / 80.0), 0.1f, 0.99f);
        return new FrameGesturePrediction(_gestureId, confidence);
    }

    private static double CalculateAverageBrightness(byte[] bgra)
    {
        if (bgra.Length == 0)
        {
            return 0;
        }

        long sum = 0;
        var pixelCount = bgra.Length / 4;
        for (var i = 0; i < bgra.Length; i += 4)
        {
            var blue = bgra[i];
            var green = bgra[i + 1];
            var red = bgra[i + 2];
            sum += (red + green + blue) / 3;
        }

        return sum / (double)Math.Max(1, pixelCount);
    }
}

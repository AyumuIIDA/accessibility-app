using RyoikiTenkai.Core;
using RyoikiTenkai.Vision;

namespace RyoikiTenkai.GestureSources;

internal sealed class CameraGestureSource : IGestureSource
{
    private readonly IFrameGestureModel _model;
    private readonly int _frameCount;
    private readonly TimeSpan _interval;

    public CameraGestureSource(IFrameGestureModel model, int frameCount, TimeSpan interval)
    {
        _model = model;
        _frameCount = frameCount;
        _interval = interval;
    }

    public event Action<GestureEvent>? GestureDetected;

    public void Start()
    {
        StartAsync().GetAwaiter().GetResult();
    }

    private async Task StartAsync()
    {
        await using var camera = new CameraFrameSource();
        await camera.InitializeAsync();

        for (var i = 0; i < _frameCount; i++)
        {
            if (i > 0)
            {
                await Task.Delay(_interval);
            }

            var frame = await camera.CaptureFrameAsync();
            Console.WriteLine($"Captured frame: {frame.Width}x{frame.Height}");

            var prediction = _model.Predict(frame);
            if (prediction is null)
            {
                continue;
            }

            GestureDetected?.Invoke(new GestureEvent(
                GestureId: prediction.GestureId,
                Confidence: prediction.Confidence,
                Source: "camera"));
        }
    }
}

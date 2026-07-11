using RyoikiTenkai.Core;

namespace RyoikiTenkai.GestureSources;

internal sealed class FakeVisionGestureSource : IGestureSource
{
    private readonly string _gestureId;
    private readonly int _count;
    private readonly TimeSpan _interval;

    public FakeVisionGestureSource(string gestureId, int count, TimeSpan interval)
    {
        _gestureId = gestureId;
        _count = count;
        _interval = interval;
    }

    public event Action<GestureEvent>? GestureDetected;

    public void Start()
    {
        for (var i = 0; i < _count; i++)
        {
            if (i > 0)
            {
                Thread.Sleep(_interval);
            }

            GestureDetected?.Invoke(new GestureEvent(
                GestureId: _gestureId,
                Confidence: 0.99f,
                Source: "fake-vision"));
        }
    }
}

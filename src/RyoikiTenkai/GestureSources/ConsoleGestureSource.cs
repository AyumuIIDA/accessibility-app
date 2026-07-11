using RyoikiTenkai.Core;

namespace RyoikiTenkai.GestureSources;

internal sealed class ConsoleGestureSource : IGestureSource
{
    private readonly Func<string> _readRequired;

    public ConsoleGestureSource(Func<string> readRequired)
    {
        _readRequired = readRequired;
    }

    public event Action<GestureEvent>? GestureDetected;

    public void Start()
    {
        Console.Write("Gesture id to trigger: ");
        var gestureId = _readRequired();
        GestureDetected?.Invoke(new GestureEvent(
            GestureId: gestureId,
            Confidence: 1.0f,
            Source: "console"));
    }
}

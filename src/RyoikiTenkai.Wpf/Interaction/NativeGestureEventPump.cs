using RyoikiTenkai.Core;
using RyoikiTenkai.Wpf.Native;

namespace RyoikiTenkai.Wpf.Interaction;

internal sealed class NativeGestureEventPump
{
    private readonly NativeVisionHost _host;
    private readonly GestureCommandDispatcher _dispatcher;
    private readonly Action<string> _log;
    private ulong _cursor;
    private int _draining;

    public NativeGestureEventPump(
        NativeVisionHost host,
        GestureCommandDispatcher dispatcher,
        Action<string> log)
    {
        _host = host;
        _dispatcher = dispatcher;
        _log = log;
    }

    public void Reset() => _cursor = 0;

    public async Task PollAsync(CancellationToken cancellationToken)
    {
        if (Interlocked.Exchange(ref _draining, 1) != 0) return;
        try
        {
            while (_host.TryReadHandEvents(_cursor, out var batch))
            {
                if (batch.DroppedCount > 0)
                    _log($"Gesture event overflow: {batch.DroppedCount} event(s) were dropped.");
                for (var index = 0; index < batch.Count; index++)
                {
                    var item = batch.Events[index];
                    try
                    {
                        var result = await _dispatcher.DispatchAsync(
                            new ConfirmedGestureEvent(item.Sequence, item.Id, item.Confidence,
                                item.InputQuality, item.BeganTimestampUs, item.EndedTimestampUs),
                            cancellationToken);
                        _log(result.Executed
                            ? $"Gesture {result.GestureId} executed: {result.Message}"
                            : $"Gesture {result.GestureId} ignored: {result.Message}");
                    }
                    catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                    {
                        throw;
                    }
                    catch (Exception exception)
                    {
                        // An action may have produced an external side effect
                        // before reporting failure. Consume this event once;
                        // retrying it would be less safe than surfacing failure.
                        _log($"Gesture {item.Id} action failed: {exception.Message}");
                    }
                    finally
                    {
                        _cursor = item.Sequence;
                    }
                }
                _cursor = batch.NextSequence;
                if (batch.Count < 16) break;
            }
        }
        finally
        {
            Volatile.Write(ref _draining, 0);
        }
    }
}

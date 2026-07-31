namespace RyoikiTenkai.Core;

internal sealed record GestureCommandBinding(
    uint GestureId,
    string DisplayName,
    bool Enabled,
    ActionSpec Action);

internal interface IGestureBindingResolver
{
    GestureCommandBinding? Resolve(uint gestureId);
}

internal sealed class MutableGestureBindingResolver : IGestureBindingResolver
{
    private readonly object _gate = new();
    private Dictionary<uint, GestureCommandBinding> _bindings = [];

    public GestureCommandBinding? Resolve(uint gestureId)
    {
        lock (_gate) return _bindings.GetValueOrDefault(gestureId);
    }

    public void Replace(IEnumerable<GestureCommandBinding> bindings)
    {
        ArgumentNullException.ThrowIfNull(bindings);
        var replacement = bindings.ToDictionary(binding => binding.GestureId);
        lock (_gate) _bindings = replacement;
    }
}

internal sealed record ConfirmedGestureEvent(
    ulong Sequence,
    uint GestureId,
    float Confidence,
    float InputQuality,
    ulong BeganTimestampUs,
    ulong EndedTimestampUs);

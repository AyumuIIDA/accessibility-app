using RyoikiTenkai.Core;

namespace RyoikiTenkai.Vision;

internal sealed class GestureWindowBuffer
{
    private readonly TimeSpan _windowDuration;
    private readonly List<GestureFrameSample> _samples = [];

    public GestureWindowBuffer(TimeSpan windowDuration)
    {
        _windowDuration = windowDuration;
    }

    public IReadOnlyList<GestureFrameSample> Samples => _samples;

    public void Add(GestureFrameSample sample)
    {
        _samples.Add(sample);
        var cutoff = sample.Timestamp - _windowDuration;
        _samples.RemoveAll(x => x.Timestamp < cutoff);
    }

    public void Reset()
    {
        _samples.Clear();
    }

    public GestureWindowSnapshot CreateSnapshot()
    {
        var duration = _samples.Count < 2
            ? 0
            : Math.Max(0, (_samples[^1].Timestamp - _samples[0].Timestamp).TotalMilliseconds);
        var usableFrameCount = _samples.Count(x => x.Landmarks.Count >= 21 && x.Confidence >= 0.35f);
        var fps = duration <= 0 ? 0 : _samples.Count * 1000.0 / duration;
        var usableFps = duration <= 0 ? 0 : usableFrameCount * 1000.0 / duration;

        return new GestureWindowSnapshot(
            FrameCount: _samples.Count,
            UsableFrameCount: usableFrameCount,
            DurationMilliseconds: duration,
            EffectiveFps: fps,
            UsableEffectiveFps: usableFps);
    }
}

internal sealed record GestureWindowSnapshot(
    int FrameCount,
    int UsableFrameCount,
    double DurationMilliseconds,
    double EffectiveFps,
    double UsableEffectiveFps);

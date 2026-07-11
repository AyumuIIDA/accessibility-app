using RyoikiTenkai.Core;

namespace RyoikiTenkai.GestureSources;

internal interface IGestureSource
{
    event Action<GestureEvent>? GestureDetected;

    void Start();
}

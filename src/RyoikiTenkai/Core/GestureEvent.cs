namespace RyoikiTenkai.Core;

internal sealed record GestureEvent(
    string GestureId,
    float Confidence,
    string Source);

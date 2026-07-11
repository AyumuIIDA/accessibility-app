namespace RyoikiTenkai.Core;

internal sealed record GestureBinding(
    string GestureId,
    string DisplayName,
    ActionSpec Action);

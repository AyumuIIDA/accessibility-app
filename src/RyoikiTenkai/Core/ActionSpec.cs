namespace RyoikiTenkai.Core;

internal sealed record ActionSpec(
    string Type,
    Dictionary<string, string> Params);

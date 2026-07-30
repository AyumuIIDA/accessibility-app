namespace RyoikiTenkai.Actions;

internal sealed record HandoffPayload(
    string PayloadId,
    string FileName,
    string ContentType,
    byte[] Data,
    DateTimeOffset CreatedAt);

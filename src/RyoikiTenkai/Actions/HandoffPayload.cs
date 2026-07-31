namespace RyoikiTenkai.Actions;

internal sealed record HandoffPayload(string PayloadId, string FileName,
    string ContentType, byte[] Data, DateTimeOffset CreatedAt);

internal interface IHandoffPayloadProvider
{
    Task<HandoffPayload?> CaptureScreenshotAsync(CancellationToken cancellationToken);
    Task<string> SaveReceivedFileAsync(string fileName, ReadOnlyMemory<byte> data,
        CancellationToken cancellationToken);
    Task OpenReceivedFileAsync(string path, CancellationToken cancellationToken);
}

internal sealed record HandoffOffer(string PayloadId, string SenderName,
    string FileName, string ContentType, long Length, DateTimeOffset ExpiresAt);

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

internal enum HandoffDirection
{
    Sent,
    Received
}

/// <summary>
/// The last transfer that actually completed. A gesture carries no confirmation
/// of its own, so this is how the operator learns what left or arrived, and it
/// outlives the transient effect. <see cref="Data"/> is the transferred payload
/// itself (a single one is retained, bounded by the payload size limit) so a
/// sent file can still be shown after it stopped being advertised;
/// <see cref="LocalPath"/> is set only for received files, which are on disk.
/// </summary>
internal sealed record HandoffTransfer(HandoffDirection Direction, string PayloadId,
    string FileName, string ContentType, long Length, string PeerName,
    DateTimeOffset CompletedAt, ReadOnlyMemory<byte> Data, string? LocalPath);

/// <summary>
/// Observable phase of the gesture-driven handoff. LAN transfer has no manual
/// controls, so this is the only feedback the operator receives.
/// </summary>
internal enum HandoffState
{
    Idle,
    Advertising,
    OfferAvailable,
    Claiming,
    Completed,
    Failed
}

internal sealed record HandoffStatus(HandoffState State, string Message,
    HandoffOffer? Offer, HandoffTransfer? LastTransfer);

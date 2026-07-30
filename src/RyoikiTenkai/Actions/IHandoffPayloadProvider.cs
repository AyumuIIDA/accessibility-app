namespace RyoikiTenkai.Actions;

internal interface IHandoffPayloadProvider
{
    Task<HandoffPayload?> CaptureScreenshotAsync(CancellationToken cancellationToken);

    Task OpenReceivedFileAsync(string path, CancellationToken cancellationToken);
}

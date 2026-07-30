using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;

namespace RyoikiTenkai.Actions;

internal sealed class HandoffService : IAsyncDisposable
{
    private const int OfferPort = 49667;
    private const int MaxClaimLineBytes = 4096;
    private static readonly TimeSpan OfferTtl = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan OfferRepeatInterval = TimeSpan.FromMilliseconds(900);
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);

    private readonly IHandoffPayloadProvider _payloadProvider;
    private readonly Action<string> _log;
    private readonly string _senderId = Guid.NewGuid().ToString("N");
    private readonly string _senderName = Environment.MachineName;
    private readonly object _gate = new();
    private readonly CancellationTokenSource _cts = new();
    private readonly Task _receiveTask;

    private UdpClient? _sender;
    private TcpListener? _tcpListener;
    private Task? _tcpTask;
    private Task? _offerTask;
    private HandoffPayload? _activePayload;
    private string? _claimedPayloadId;
    private ReceivedOffer? _latestOffer;

    public HandoffService(IHandoffPayloadProvider payloadProvider, Action<string> log)
    {
        _payloadProvider = payloadProvider;
        _log = log;
        _receiveTask = Task.Run(() => ReceiveOffersAsync(_cts.Token));
    }

    public async Task GrabScreenshotAsync(CancellationToken cancellationToken)
    {
        try
        {
            var payload = await _payloadProvider.CaptureScreenshotAsync(cancellationToken).ConfigureAwait(false);
            if (payload is null)
            {
                _log("Handoff grab failed: screenshot capture returned no data.");
                return;
            }

            await EnsureTransferServerAsync(cancellationToken).ConfigureAwait(false);

            lock (_gate)
            {
                _activePayload = payload;
                _claimedPayloadId = null;
            }

            StartOfferBroadcast();
            _log($"Handoff grabbed screenshot: {payload.FileName}");
        }
        catch (Exception ex) when (ex is IOException or SocketException or OperationCanceledException or InvalidOperationException)
        {
            _log($"Handoff grab failed: {ex.Message}");
        }
    }

    public async Task ReleaseHereAsync(CancellationToken cancellationToken)
    {
        ReceivedOffer? offer;
        lock (_gate)
        {
            offer = _latestOffer;
        }

        if (offer is null || DateTimeOffset.UtcNow - offer.ReceivedAt > OfferTtl)
        {
            _log("No grabbed item available.");
            return;
        }

        try
        {
            var data = await ClaimOfferAsync(offer, cancellationToken).ConfigureAwait(false);
            var receiveDirectory = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.MyPictures),
                "RyoikiTenkai Handoff");
            Directory.CreateDirectory(receiveDirectory);

            var path = CreateUniquePath(receiveDirectory, offer.FileName);
            await File.WriteAllBytesAsync(path, data, cancellationToken).ConfigureAwait(false);
            _log($"Handoff received: {path}");
            await _payloadProvider.OpenReceivedFileAsync(path, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is IOException or SocketException or OperationCanceledException or JsonException)
        {
            _log($"Handoff release failed: {ex.Message}");
        }
    }

    public async ValueTask DisposeAsync()
    {
        _cts.Cancel();
        _sender?.Dispose();
        _tcpListener?.Stop();

        await WhenCompletedAsync(_receiveTask).ConfigureAwait(false);
        if (_tcpTask is not null)
        {
            await WhenCompletedAsync(_tcpTask).ConfigureAwait(false);
        }

        if (_offerTask is not null)
        {
            await WhenCompletedAsync(_offerTask).ConfigureAwait(false);
        }

        _cts.Dispose();
    }

    private async Task EnsureTransferServerAsync(CancellationToken cancellationToken)
    {
        if (_tcpListener is not null)
        {
            return;
        }

        var listener = new TcpListener(IPAddress.Any, 0);
        listener.Start();
        _tcpListener = listener;
        _tcpTask = Task.Run(() => ServeClaimsAsync(listener, _cts.Token), cancellationToken);
        await Task.CompletedTask.ConfigureAwait(false);
    }

    private void StartOfferBroadcast()
    {
        _offerTask ??= Task.Run(() => BroadcastOffersAsync(_cts.Token));
    }

    private async Task BroadcastOffersAsync(CancellationToken cancellationToken)
    {
        _sender ??= new UdpClient
        {
            EnableBroadcast = true
        };

        while (!cancellationToken.IsCancellationRequested)
        {
            HandoffPayload? payload;
            int port;
            lock (_gate)
            {
                payload = _activePayload;
                port = ((IPEndPoint?)_tcpListener?.LocalEndpoint)?.Port ?? 0;
            }

            if (payload is not null && port > 0 && DateTimeOffset.UtcNow - payload.CreatedAt <= OfferTtl)
            {
                var offer = new OfferMessage(
                    "handoff.offer",
                    payload.PayloadId,
                    _senderId,
                    _senderName,
                    port,
                    payload.ContentType,
                    payload.FileName,
                    payload.CreatedAt);
                var json = JsonSerializer.Serialize(offer, JsonOptions);
                var bytes = Encoding.UTF8.GetBytes(json);
                await _sender.SendAsync(bytes, new IPEndPoint(IPAddress.Broadcast, OfferPort), cancellationToken)
                    .ConfigureAwait(false);
            }

            await Task.Delay(OfferRepeatInterval, cancellationToken).ConfigureAwait(false);
        }
    }

    private async Task ReceiveOffersAsync(CancellationToken cancellationToken)
    {
        using var receiver = new UdpClient();
        try
        {
            receiver.ExclusiveAddressUse = false;
            receiver.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            receiver.Client.Bind(new IPEndPoint(IPAddress.Any, OfferPort));
        }
        catch (SocketException ex)
        {
            _log($"Handoff receive unavailable: {ex.Message}");
            return;
        }

        while (!cancellationToken.IsCancellationRequested)
        {
            UdpReceiveResult result;
            try
            {
                result = await receiver.ReceiveAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (ObjectDisposedException)
            {
                break;
            }

            OfferMessage? message;
            try
            {
                message = JsonSerializer.Deserialize<OfferMessage>(result.Buffer, JsonOptions);
            }
            catch (JsonException)
            {
                continue;
            }

            if (message is null
                || message.Kind != "handoff.offer"
                || message.SenderId == _senderId
                || string.IsNullOrWhiteSpace(message.PayloadId)
                || message.Port <= 0
                || DateTimeOffset.UtcNow - message.CreatedAtUtc > OfferTtl)
            {
                continue;
            }

            lock (_gate)
            {
                _latestOffer = new ReceivedOffer(
                    message.PayloadId,
                    message.SenderId,
                    message.SenderName,
                    result.RemoteEndPoint.Address,
                    message.Port,
                    message.ContentType,
                    message.FileName,
                    message.CreatedAtUtc,
                    DateTimeOffset.UtcNow);
            }
        }
    }

    private async Task ServeClaimsAsync(TcpListener listener, CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            TcpClient client;
            try
            {
                client = await listener.AcceptTcpClientAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (ObjectDisposedException)
            {
                break;
            }

            _ = Task.Run(() => ServeClaimAsync(client, cancellationToken), cancellationToken);
        }
    }

    private async Task ServeClaimAsync(TcpClient client, CancellationToken cancellationToken)
    {
        using var tcpClient = client;
        await using var stream = tcpClient.GetStream();

        var claimLine = await ReadLineAsync(stream, MaxClaimLineBytes, cancellationToken).ConfigureAwait(false);
        var claim = JsonSerializer.Deserialize<ClaimMessage>(claimLine, JsonOptions);
        if (claim is null || claim.Kind != "handoff.claim")
        {
            return;
        }

        HandoffPayload? payload;
        lock (_gate)
        {
            if (_activePayload is null
                || _activePayload.PayloadId != claim.PayloadId
                || DateTimeOffset.UtcNow - _activePayload.CreatedAt > OfferTtl
                || _claimedPayloadId is not null)
            {
                payload = null;
            }
            else
            {
                payload = _activePayload;
                _claimedPayloadId = payload.PayloadId;
            }
        }

        if (payload is null)
        {
            var unavailable = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(
                new TransferHeader("handoff.unavailable", string.Empty, string.Empty, 0),
                JsonOptions) + "\n");
            await stream.WriteAsync(unavailable, cancellationToken).ConfigureAwait(false);
            return;
        }

        var header = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(
            new TransferHeader("handoff.payload", payload.FileName, payload.ContentType, payload.Data.Length),
            JsonOptions) + "\n");
        await stream.WriteAsync(header, cancellationToken).ConfigureAwait(false);
        await stream.WriteAsync(payload.Data, cancellationToken).ConfigureAwait(false);
        lock (_gate)
        {
            if (_activePayload?.PayloadId == payload.PayloadId)
            {
                _activePayload = null;
            }
        }

        _log($"Handoff sent to {client.Client.RemoteEndPoint}: {payload.FileName}");
    }

    private static async Task<byte[]> ClaimOfferAsync(ReceivedOffer offer, CancellationToken cancellationToken)
    {
        using var client = new TcpClient();
        await client.ConnectAsync(offer.Address, offer.Port, cancellationToken).ConfigureAwait(false);
        await using var stream = client.GetStream();

        var claim = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(
            new ClaimMessage("handoff.claim", offer.PayloadId),
            JsonOptions) + "\n");
        await stream.WriteAsync(claim, cancellationToken).ConfigureAwait(false);

        var headerLine = await ReadLineAsync(stream, MaxClaimLineBytes, cancellationToken).ConfigureAwait(false);
        var header = JsonSerializer.Deserialize<TransferHeader>(headerLine, JsonOptions);
        if (header is null || header.Kind != "handoff.payload" || header.Length <= 0)
        {
            throw new IOException("The grabbed item is no longer available.");
        }

        var data = new byte[header.Length];
        await stream.ReadExactlyAsync(data, cancellationToken).ConfigureAwait(false);
        return data;
    }

    private static async Task<string> ReadLineAsync(Stream stream, int maxBytes, CancellationToken cancellationToken)
    {
        var bytes = new List<byte>();
        var buffer = new byte[1];
        while (bytes.Count < maxBytes)
        {
            var read = await stream.ReadAsync(buffer, cancellationToken).ConfigureAwait(false);
            if (read == 0)
            {
                break;
            }

            if (buffer[0] == (byte)'\n')
            {
                break;
            }

            bytes.Add(buffer[0]);
        }

        if (bytes.Count >= maxBytes)
        {
            throw new IOException("Protocol line was too long.");
        }

        return Encoding.UTF8.GetString(bytes.ToArray());
    }

    private static string CreateUniquePath(string directory, string fileName)
    {
        var safeName = string.Join("_", fileName.Split(Path.GetInvalidFileNameChars(), StringSplitOptions.RemoveEmptyEntries));
        if (string.IsNullOrWhiteSpace(safeName))
        {
            safeName = "handoff.png";
        }

        var path = Path.Combine(directory, safeName);
        if (!File.Exists(path))
        {
            return path;
        }

        var name = Path.GetFileNameWithoutExtension(safeName);
        var extension = Path.GetExtension(safeName);
        for (var i = 1; i < 1000; i++)
        {
            path = Path.Combine(directory, $"{name}-{i}{extension}");
            if (!File.Exists(path))
            {
                return path;
            }
        }

        return Path.Combine(directory, $"{name}-{Guid.NewGuid():N}{extension}");
    }

    private static async Task WhenCompletedAsync(Task task)
    {
        try
        {
            await task.ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is OperationCanceledException or ObjectDisposedException or SocketException)
        {
        }
    }

    private sealed record OfferMessage(
        string Kind,
        string PayloadId,
        string SenderId,
        string SenderName,
        int Port,
        string ContentType,
        string FileName,
        DateTimeOffset CreatedAtUtc);

    private sealed record ClaimMessage(string Kind, string PayloadId);

    private sealed record TransferHeader(string Kind, string FileName, string ContentType, int Length);

    private sealed record ReceivedOffer(
        string PayloadId,
        string SenderId,
        string SenderName,
        IPAddress Address,
        int Port,
        string ContentType,
        string FileName,
        DateTimeOffset CreatedAtUtc,
        DateTimeOffset ReceivedAt);
}

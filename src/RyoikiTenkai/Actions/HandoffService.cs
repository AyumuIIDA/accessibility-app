using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace RyoikiTenkai.Actions;

internal sealed class HandoffService : IAsyncDisposable
{
    internal const int DefaultOfferPort = 49667;
    internal const int MaximumPayloadBytes = 16 * 1024 * 1024;
    private const int MaximumProtocolLineBytes = 4096;
    private static readonly TimeSpan OfferTtl = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan RepeatInterval = TimeSpan.FromMilliseconds(900);
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);
    private readonly IHandoffPayloadProvider _provider;
    private readonly Action<string> _log;
    private readonly int _offerPort;
    private readonly string _senderId = Guid.NewGuid().ToString("N");
    private readonly object _gate = new();
    private readonly CancellationTokenSource _shutdown = new();
    private readonly ConcurrentDictionary<int, Task> _clients = new();
    private readonly Task _receiveTask;
    private UdpClient? _broadcast;
    private TcpListener? _listener;
    private Task? _serveTask;
    private Task? _broadcastTask;
    private HandoffPayload? _active;
    private string? _claimToken;
    private bool _claimed;
    private ReceivedOffer? _latest;
    private int _nextClientId;

    public HandoffService(IHandoffPayloadProvider provider, Action<string> log,
        int offerPort = DefaultOfferPort)
    {
        _provider = provider; _log = log; _offerPort = offerPort;
        _receiveTask = ReceiveOffersAsync(_shutdown.Token);
    }

    public HandoffOffer? LatestOffer
    {
        get { lock (_gate) return _latest is { } x && x.ExpiresAt > DateTimeOffset.UtcNow
                ? new(x.PayloadId, x.SenderName, x.FileName, x.ContentType, x.Length, x.ExpiresAt) : null; }
    }

    public async Task GrabScreenshotAsync(CancellationToken cancellationToken)
    {
        var payload = await _provider.CaptureScreenshotAsync(cancellationToken).ConfigureAwait(false);
        if (payload is null) throw new InvalidOperationException("Screenshot capture returned no data.");
        ValidatePayload(payload);
        EnsureTransferServer();
        lock (_gate)
        {
            _active = payload with { FileName = SafeFileName(payload.FileName) };
            _claimToken = Convert.ToBase64String(RandomNumberGenerator.GetBytes(32));
            _claimed = false;
        }
        _broadcastTask ??= BroadcastOffersAsync(_shutdown.Token);
        _log($"Handoff offer ready: {payload.FileName}");
    }

    public async Task<string> ReleaseHereAsync(CancellationToken cancellationToken)
    {
        ReceivedOffer offer;
        lock (_gate) offer = _latest is { ExpiresAt: var expiry } value && expiry > DateTimeOffset.UtcNow
            ? value : throw new InvalidOperationException("No unexpired handoff offer is available.");
        var (header, data) = await ClaimAsync(offer, cancellationToken).ConfigureAwait(false);
        var path = await _provider.SaveReceivedFileAsync(SafeFileName(header.FileName), data,
            cancellationToken).ConfigureAwait(false);
        await _provider.OpenReceivedFileAsync(path, cancellationToken).ConfigureAwait(false);
        _log($"Handoff received: {path}");
        return path;
    }

    private void EnsureTransferServer()
    {
        if (_listener is not null) return;
        _listener = new TcpListener(IPAddress.Any, 0);
        _listener.Start(8);
        _serveTask = ServeAsync(_listener, _shutdown.Token);
    }

    private async Task BroadcastOffersAsync(CancellationToken token)
    {
        _broadcast ??= new UdpClient { EnableBroadcast = true };
        while (!token.IsCancellationRequested)
        {
            OfferMessage? message = null;
            lock (_gate)
            {
                if (_active is { } payload && !_claimed && payload.CreatedAt + OfferTtl > DateTimeOffset.UtcNow
                    && _listener?.LocalEndpoint is IPEndPoint endpoint)
                    message = new("handoff.offer", payload.PayloadId, _senderId, Environment.MachineName,
                        endpoint.Port, payload.ContentType, payload.FileName, payload.Data.LongLength,
                        payload.CreatedAt + OfferTtl, _claimToken!);
            }
            if (message is not null)
            {
                var bytes = JsonSerializer.SerializeToUtf8Bytes(message, JsonOptions);
                if (bytes.Length <= MaximumProtocolLineBytes)
                    await _broadcast.SendAsync(bytes, new IPEndPoint(IPAddress.Broadcast, _offerPort), token)
                        .ConfigureAwait(false);
            }
            await Task.Delay(RepeatInterval, token).ConfigureAwait(false);
        }
    }

    private async Task ReceiveOffersAsync(CancellationToken token)
    {
        using var receiver = new UdpClient();
        receiver.ExclusiveAddressUse = false;
        receiver.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
        receiver.Client.Bind(new IPEndPoint(IPAddress.Any, _offerPort));
        while (!token.IsCancellationRequested)
        {
            try
            {
                var packet = await receiver.ReceiveAsync(token).ConfigureAwait(false);
                if (packet.Buffer.Length > MaximumProtocolLineBytes) continue;
                var offer = JsonSerializer.Deserialize<OfferMessage>(packet.Buffer, JsonOptions);
                if (offer is null || offer.Kind != "handoff.offer" || offer.SenderId == _senderId
                    || !ValidId(offer.PayloadId) || offer.Port is < 1 or > 65535
                    || offer.Length is <= 0 or > MaximumPayloadBytes
                    || offer.ExpiresAt <= DateTimeOffset.UtcNow || offer.ExpiresAt > DateTimeOffset.UtcNow + OfferTtl
                    || string.IsNullOrWhiteSpace(offer.ClaimToken)) continue;
                lock (_gate) _latest = new(offer.PayloadId, offer.SenderName, packet.RemoteEndPoint.Address,
                    offer.Port, offer.ContentType, SafeFileName(offer.FileName), offer.Length,
                    offer.ExpiresAt, offer.ClaimToken);
            }
            catch (OperationCanceledException) when (token.IsCancellationRequested) { break; }
            catch (JsonException) { }
            catch (SocketException) when (token.IsCancellationRequested) { break; }
        }
    }

    private async Task ServeAsync(TcpListener listener, CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            try
            {
                var client = await listener.AcceptTcpClientAsync(token).ConfigureAwait(false);
                var id = Interlocked.Increment(ref _nextClientId);
                var task = ServeClientAsync(client, token);
                _clients[id] = task;
                _ = task.ContinueWith(completed => _clients.TryRemove(id, out var ignored), CancellationToken.None,
                    TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
            }
            catch (OperationCanceledException) when (token.IsCancellationRequested) { break; }
            catch (SocketException) when (token.IsCancellationRequested) { break; }
        }
    }

    private async Task ServeClientAsync(TcpClient client, CancellationToken token)
    {
        using (client)
        await using (var stream = client.GetStream())
        {
            var claim = JsonSerializer.Deserialize<ClaimMessage>(
                await ReadLineAsync(stream, token).ConfigureAwait(false), JsonOptions);
            HandoffPayload? payload = null;
            lock (_gate)
            {
                if (claim is { Kind: "handoff.claim" } && _active is { } active && !_claimed
                    && active.CreatedAt + OfferTtl > DateTimeOffset.UtcNow
                    && CryptographicOperations.FixedTimeEquals(
                        Encoding.UTF8.GetBytes(claim.ClaimToken), Encoding.UTF8.GetBytes(_claimToken ?? ""))
                    && claim.PayloadId == active.PayloadId)
                { _claimed = true; payload = active; }
            }
            if (payload is null) { await WriteHeaderAsync(stream, new("handoff.unavailable", "", "", 0), token); return; }
            try
            {
                await WriteHeaderAsync(stream, new("handoff.payload", payload.FileName,
                    payload.ContentType, payload.Data.LongLength), token).ConfigureAwait(false);
                await stream.WriteAsync(payload.Data, token).ConfigureAwait(false);
                lock (_gate) if (_active?.PayloadId == payload.PayloadId) _active = null;
            }
            catch { lock (_gate) if (_active?.PayloadId == payload.PayloadId) _claimed = false; throw; }
        }
    }

    private static async Task<(TransferHeader Header, byte[] Data)> ClaimAsync(ReceivedOffer offer, CancellationToken token)
    {
        using var client = new TcpClient();
        await client.ConnectAsync(offer.Address, offer.Port, token).ConfigureAwait(false);
        await using var stream = client.GetStream();
        await WriteLineAsync(stream, new ClaimMessage("handoff.claim", offer.PayloadId, offer.ClaimToken), token);
        var header = JsonSerializer.Deserialize<TransferHeader>(await ReadLineAsync(stream, token), JsonOptions)
            ?? throw new IOException("Missing handoff response.");
        if (header.Kind != "handoff.payload" || header.Length is <= 0 or > MaximumPayloadBytes
            || header.Length != offer.Length) throw new IOException("Invalid handoff payload header.");
        var data = new byte[checked((int)header.Length)];
        await stream.ReadExactlyAsync(data, token).ConfigureAwait(false);
        return (header, data);
    }

    private static async Task<string> ReadLineAsync(Stream stream, CancellationToken token)
    {
        var data = new byte[MaximumProtocolLineBytes]; var count = 0;
        while (count < data.Length)
        {
            var read = await stream.ReadAsync(data.AsMemory(count, 1), token).ConfigureAwait(false);
            if (read == 0) throw new IOException("Protocol line ended unexpectedly.");
            if (data[count] == (byte)'\n') return Encoding.UTF8.GetString(data, 0, count);
            count++;
        }
        throw new IOException("Protocol line exceeded its size limit.");
    }
    private static Task WriteLineAsync<T>(Stream stream, T value, CancellationToken token) =>
        stream.WriteAsync(Encoding.UTF8.GetBytes(JsonSerializer.Serialize(value, JsonOptions) + "\n"), token).AsTask();
    private static Task WriteHeaderAsync(Stream stream, TransferHeader value, CancellationToken token) =>
        WriteLineAsync(stream, value, token);
    private static bool ValidId(string value) => value.Length is > 0 and <= 128
        && value.All(c => char.IsAsciiLetterOrDigit(c) || c is '-' or '_');
    internal static string SafeFileName(string value)
    {
        var name = Path.GetFileName(value);
        name = string.Concat(name.Where(c => !Path.GetInvalidFileNameChars().Contains(c)));
        return string.IsNullOrWhiteSpace(name) ? "handoff.bin" : name;
    }
    private static void ValidatePayload(HandoffPayload payload)
    {
        if (!ValidId(payload.PayloadId)) throw new InvalidOperationException("Invalid payload ID.");
        if (payload.Data.Length is <= 0 or > MaximumPayloadBytes) throw new InvalidOperationException("Payload size is not allowed.");
        if (payload.CreatedAt > DateTimeOffset.UtcNow + TimeSpan.FromMinutes(1)) throw new InvalidOperationException("Invalid capture time.");
    }
    public async ValueTask DisposeAsync()
    {
        _shutdown.Cancel(); _listener?.Stop(); _broadcast?.Dispose();
        var tasks = new[] { _receiveTask, _serveTask, _broadcastTask }.Where(x => x is not null).Cast<Task>()
            .Concat(_clients.Values).ToArray();
        try { await Task.WhenAll(tasks).WaitAsync(TimeSpan.FromSeconds(5)).ConfigureAwait(false); }
        catch (Exception ex) when (ex is OperationCanceledException or TimeoutException or SocketException) { }
        _shutdown.Dispose();
    }
    private sealed record OfferMessage(string Kind, string PayloadId, string SenderId, string SenderName,
        int Port, string ContentType, string FileName, long Length, DateTimeOffset ExpiresAt, string ClaimToken);
    private sealed record ClaimMessage(string Kind, string PayloadId, string ClaimToken);
    private sealed record TransferHeader(string Kind, string FileName, string ContentType, long Length);
    private sealed record ReceivedOffer(string PayloadId, string SenderName, IPAddress Address, int Port,
        string ContentType, string FileName, long Length, DateTimeOffset ExpiresAt, string ClaimToken);
}

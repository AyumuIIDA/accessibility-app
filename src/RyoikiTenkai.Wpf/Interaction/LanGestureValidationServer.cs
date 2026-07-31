using System.Net;
using System.Net.Sockets;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Collections.Concurrent;
using System.Security.Cryptography;

namespace RyoikiTenkai.Wpf.Interaction;

internal sealed class LanGestureValidationServer : IAsyncDisposable
{
    private readonly Func<object> _statusProvider;
    private readonly Func<uint, uint, bool> _registrationRequester;
    private TcpListener? _listener;
    private CancellationTokenSource? _cancellation;
    private Task? _runTask;
    private string _token = string.Empty;
    private readonly ConcurrentDictionary<int, Task> _clients = new();
    private int _nextClientId;
    private const int MaximumHeaderLineLength = 4096;
    private const int MaximumHeaderCount = 64;

    public LanGestureValidationServer(
        Func<object> statusProvider,
        Func<uint, uint, bool> registrationRequester)
    {
        _statusProvider = statusProvider;
        _registrationRequester = registrationRequester;
    }

    public bool IsRunning => _listener is not null;

    public void Start(int port, string token)
    {
        if (IsRunning) throw new InvalidOperationException("LAN API is already running.");
        if (port is < 1024 or > 65535) throw new ArgumentOutOfRangeException(nameof(port));
        if (token.Length < 12) throw new ArgumentException("Use a bearer token of at least 12 characters.", nameof(token));

        _token = token;
        _cancellation = new CancellationTokenSource();
        _listener = new TcpListener(IPAddress.Any, port);
        _listener.Start(backlog: 8);
        _runTask = RunAsync(_cancellation.Token);
    }

    public async Task StopAsync()
    {
        var cancellation = _cancellation;
        var runTask = _runTask;
        _listener?.Stop();
        cancellation?.Cancel();
        if (runTask is not null)
        {
            try { await runTask.ConfigureAwait(false); }
            catch (OperationCanceledException) { }
            catch (SocketException) when (cancellation?.IsCancellationRequested == true) { }
        }
        var clients = _clients.Values.ToArray();
        if (clients.Length > 0)
        {
            try { await Task.WhenAll(clients).WaitAsync(TimeSpan.FromSeconds(5)).ConfigureAwait(false); }
            catch (Exception ex) when (ex is OperationCanceledException or TimeoutException or SocketException or IOException) { }
        }
        _listener = null;
        _runTask = null;
        _cancellation?.Dispose();
        _cancellation = null;
        _token = string.Empty;
    }

    private async Task RunAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            var client = await _listener!.AcceptTcpClientAsync(cancellationToken).ConfigureAwait(false);
            var id = Interlocked.Increment(ref _nextClientId);
            var task = HandleClientAsync(client, cancellationToken);
            _clients[id] = task;
            _ = task.ContinueWith(completed => _clients.TryRemove(id, out var ignored), CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
        }
    }

    private async Task HandleClientAsync(TcpClient client, CancellationToken cancellationToken)
    {
        using (client)
        {
            client.ReceiveTimeout = 3000;
            client.SendTimeout = 3000;
            using var stream = client.GetStream();
            using var reader = new StreamReader(stream, Encoding.ASCII, false, 1024, leaveOpen: true);
            var requestLine = await reader.ReadLineAsync(cancellationToken).ConfigureAwait(false);
            if (string.IsNullOrWhiteSpace(requestLine) || requestLine.Length > MaximumHeaderLineLength) return;

            string? authorization = null;
            for (var headerCount = 0; headerCount < MaximumHeaderCount; headerCount++)
            {
                var line = await reader.ReadLineAsync(cancellationToken).ConfigureAwait(false);
                if (string.IsNullOrEmpty(line)) break;
                if (line.Length > MaximumHeaderLineLength) return;
                if (line.StartsWith("Authorization:", StringComparison.OrdinalIgnoreCase))
                    authorization = line["Authorization:".Length..].Trim();
            }

            if (!FixedTimeEquals(authorization, $"Bearer {_token}"))
            {
                await WriteJsonAsync(stream, 401, new { error = "unauthorized" }, cancellationToken).ConfigureAwait(false);
                return;
            }

            var parts = requestLine.Split(' ', 3, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 2 || !Uri.TryCreate("http://localhost" + parts[1], UriKind.Absolute, out var uri))
            {
                await WriteJsonAsync(stream, 400, new { error = "bad_request" }, cancellationToken).ConfigureAwait(false);
                return;
            }

            if (parts[0] == "GET" && uri.AbsolutePath == "/api/gesture/status")
            {
                await WriteJsonAsync(stream, 200, _statusProvider(), cancellationToken).ConfigureAwait(false);
                return;
            }
            if (parts[0] == "POST" && uri.AbsolutePath.StartsWith("/api/gesture/templates/", StringComparison.Ordinal))
            {
                var idText = uri.AbsolutePath["/api/gesture/templates/".Length..];
                var query = ParseQuery(uri.Query);
                if (!uint.TryParse(idText, out var templateId)
                    || !uint.TryParse(query.GetValueOrDefault("trackId", "0"), out var trackId))
                {
                    await WriteJsonAsync(stream, 400, new { error = "invalid_id" }, cancellationToken).ConfigureAwait(false);
                    return;
                }
                var accepted = _registrationRequester(trackId, templateId);
                await WriteJsonAsync(stream, accepted ? 202 : 409,
                    new { accepted, trackId, templateId }, cancellationToken).ConfigureAwait(false);
                return;
            }
            await WriteJsonAsync(stream, 404, new { error = "not_found" }, cancellationToken).ConfigureAwait(false);
        }
    }

    private static bool FixedTimeEquals(string? left, string right)
    {
        if (left is null) return false;
        var leftBytes = Encoding.UTF8.GetBytes(left);
        var rightBytes = Encoding.UTF8.GetBytes(right);
        return leftBytes.Length == rightBytes.Length
            && CryptographicOperations.FixedTimeEquals(leftBytes, rightBytes);
    }

    private static Dictionary<string, string> ParseQuery(string query) => query.TrimStart('?')
        .Split('&', StringSplitOptions.RemoveEmptyEntries)
        .Select(x => x.Split('=', 2))
        .Where(x => x.Length == 2)
        .ToDictionary(x => Uri.UnescapeDataString(x[0]), x => Uri.UnescapeDataString(x[1]), StringComparer.OrdinalIgnoreCase);

    private static async Task WriteJsonAsync(Stream stream, int status, object value, CancellationToken cancellationToken)
    {
        var body = JsonSerializer.SerializeToUtf8Bytes(value);
        var reason = status switch { 200 => "OK", 202 => "Accepted", 400 => "Bad Request", 401 => "Unauthorized", 404 => "Not Found", _ => "Conflict" };
        var headers = Encoding.ASCII.GetBytes($"HTTP/1.1 {status} {reason}\r\nContent-Type: application/json\r\nContent-Length: {body.Length}\r\nConnection: close\r\n\r\n");
        await stream.WriteAsync(headers, cancellationToken).ConfigureAwait(false);
        await stream.WriteAsync(body, cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask DisposeAsync() => await StopAsync().ConfigureAwait(false);
}

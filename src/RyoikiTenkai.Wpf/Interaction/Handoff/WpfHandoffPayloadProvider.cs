using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using RyoikiTenkai.Actions;

namespace RyoikiTenkai.Wpf.Interaction.Handoff;

internal sealed class WpfHandoffPayloadProvider : IHandoffPayloadProvider
{
    private readonly Window _window;
    public WpfHandoffPayloadProvider(Window window) => _window = window;

    public async Task<HandoffPayload?> CaptureScreenshotAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return await _window.Dispatcher.InvokeAsync(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            var width = Math.Max(1, (int)Math.Ceiling(_window.ActualWidth));
            var height = Math.Max(1, (int)Math.Ceiling(_window.ActualHeight));
            var bitmap = new RenderTargetBitmap(width, height, 96, 96, PixelFormats.Pbgra32);
            bitmap.Render(_window);
            var encoder = new PngBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(bitmap));
            using var stream = new MemoryStream();
            encoder.Save(stream);
            var now = DateTimeOffset.UtcNow;
            return new HandoffPayload(Guid.NewGuid().ToString("N"),
                $"handoff-{now:yyyyMMdd-HHmmss}.png", "image/png", stream.ToArray(), now);
        });
    }

    public async Task<string> SaveReceivedFileAsync(string fileName, ReadOnlyMemory<byte> data,
        CancellationToken cancellationToken)
    {
        var directory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyPictures),
            "RyoikiTenkai Handoff");
        Directory.CreateDirectory(directory);
        var safeName = HandoffService.SafeFileName(fileName);
        var path = Path.GetFullPath(Path.Combine(directory, safeName));
        var root = Path.GetFullPath(directory) + Path.DirectorySeparatorChar;
        if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase))
            throw new IOException("Received file path escaped the handoff directory.");
        if (File.Exists(path))
            path = Path.Combine(directory, $"{Path.GetFileNameWithoutExtension(safeName)}-{Guid.NewGuid():N}{Path.GetExtension(safeName)}");
        await File.WriteAllBytesAsync(path, data.ToArray(), cancellationToken).ConfigureAwait(false);
        return path;
    }

    public Task OpenReceivedFileAsync(string path, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
        return Task.CompletedTask;
    }
}

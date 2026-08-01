using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using RyoikiTenkai.Actions;

namespace RyoikiTenkai.Wpf.Interaction.Handoff;

internal sealed partial class WpfHandoffPayloadProvider : IHandoffPayloadProvider
{
    private const int MonitorDefaultToNearest = 0x00000002;
    private const int SrcCopy = 0x00CC0020;
    // Layered windows are most of a modern desktop; without this they copy as black.
    private const int CaptureBlt = 0x40000000;
    private const int SmXVirtualScreen = 76;
    private const int SmYVirtualScreen = 77;
    private const int SmCxVirtualScreen = 78;
    private const int SmCyVirtualScreen = 79;

    /// <summary>
    /// Keeps a 4K capture comfortably inside the 16 MiB payload limit. A screen
    /// handed to another device is a look at what is open, not an archival copy.
    /// </summary>
    private const int MaximumEdgePixels = 2560;

    private readonly Window _window;
    public WpfHandoffPayloadProvider(Window window) => _window = window;

    public async Task<HandoffPayload?> CaptureScreenshotAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return await _window.Dispatcher.InvokeAsync(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            // The payload is the display as it stands when the gesture fires:
            // whatever the operator has open, not this application's own window.
            var frame = Downscale(CaptureScreen(DisplayBounds()));
            var encoder = new PngBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(frame));
            using var stream = new MemoryStream();
            encoder.Save(stream);
            var now = DateTimeOffset.UtcNow;
            return new HandoffPayload(Guid.NewGuid().ToString("N"),
                $"handoff-{now:yyyyMMdd-HHmmss}.png", "image/png", stream.ToArray(), now);
        });
    }

    /// <summary>
    /// The display holding this window, so a multi-monitor desktop hands off the
    /// screen the operator is gesturing at rather than every screen at once.
    /// </summary>
    private ScreenBounds DisplayBounds()
    {
        var handle = new WindowInteropHelper(_window).Handle;
        if (handle != IntPtr.Zero)
        {
            var monitor = NativeMethods.MonitorFromWindow(handle, MonitorDefaultToNearest);
            var info = new MonitorInfo { Size = (uint)Marshal.SizeOf<MonitorInfo>() };
            if (monitor != IntPtr.Zero && NativeMethods.GetMonitorInfo(monitor, ref info))
            {
                return new(info.Monitor.Left, info.Monitor.Top,
                    info.Monitor.Right - info.Monitor.Left,
                    info.Monitor.Bottom - info.Monitor.Top);
            }
        }
        return new(NativeMethods.GetSystemMetrics(SmXVirtualScreen),
            NativeMethods.GetSystemMetrics(SmYVirtualScreen),
            NativeMethods.GetSystemMetrics(SmCxVirtualScreen),
            NativeMethods.GetSystemMetrics(SmCyVirtualScreen));
    }

    private static BitmapSource CaptureScreen(ScreenBounds bounds)
    {
        if (bounds.Width < 1 || bounds.Height < 1)
            throw new InvalidOperationException("The display reported no usable area.");
        var screenDc = NativeMethods.GetDC(IntPtr.Zero);
        if (screenDc == IntPtr.Zero)
            throw new InvalidOperationException("The screen device context is unavailable.");
        var memoryDc = IntPtr.Zero;
        var bitmap = IntPtr.Zero;
        try
        {
            memoryDc = NativeMethods.CreateCompatibleDC(screenDc);
            bitmap = NativeMethods.CreateCompatibleBitmap(screenDc, bounds.Width, bounds.Height);
            if (memoryDc == IntPtr.Zero || bitmap == IntPtr.Zero)
                throw new InvalidOperationException("The screenshot surface could not be allocated.");
            var previous = NativeMethods.SelectObject(memoryDc, bitmap);
            var copied = NativeMethods.BitBlt(memoryDc, 0, 0, bounds.Width, bounds.Height,
                screenDc, bounds.X, bounds.Y, SrcCopy | CaptureBlt);
            NativeMethods.SelectObject(memoryDc, previous);
            if (!copied) throw new InvalidOperationException("The screen could not be copied.");
            // Copies the pixels out, so the GDI bitmap is safe to delete below.
            var source = Imaging.CreateBitmapSourceFromHBitmap(bitmap, IntPtr.Zero,
                Int32Rect.Empty, BitmapSizeOptions.FromEmptyOptions());
            source.Freeze();
            return source;
        }
        finally
        {
            if (bitmap != IntPtr.Zero) NativeMethods.DeleteObject(bitmap);
            if (memoryDc != IntPtr.Zero) NativeMethods.DeleteDC(memoryDc);
            NativeMethods.ReleaseDC(IntPtr.Zero, screenDc);
        }
    }

    private static BitmapSource Downscale(BitmapSource source)
    {
        var longest = Math.Max(source.PixelWidth, source.PixelHeight);
        if (longest <= MaximumEdgePixels) return source;
        var scale = MaximumEdgePixels / (double)longest;
        var scaled = new TransformedBitmap(source, new ScaleTransform(scale, scale));
        scaled.Freeze();
        return scaled;
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

    private readonly record struct ScreenBounds(int X, int Y, int Width, int Height);

    [StructLayout(LayoutKind.Sequential)]
    private struct Rectangle
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MonitorInfo
    {
        public uint Size;
        public Rectangle Monitor;
        public Rectangle Work;
        public uint Flags;
    }

    private static partial class NativeMethods
    {
        [LibraryImport("user32.dll")]
        public static partial IntPtr MonitorFromWindow(IntPtr hwnd, int flags);

        [LibraryImport("user32.dll", EntryPoint = "GetMonitorInfoW")]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static partial bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);

        [LibraryImport("user32.dll")]
        public static partial int GetSystemMetrics(int index);

        [LibraryImport("user32.dll")]
        public static partial IntPtr GetDC(IntPtr hwnd);

        [LibraryImport("user32.dll")]
        public static partial int ReleaseDC(IntPtr hwnd, IntPtr dc);

        [LibraryImport("gdi32.dll")]
        public static partial IntPtr CreateCompatibleDC(IntPtr dc);

        [LibraryImport("gdi32.dll")]
        public static partial IntPtr CreateCompatibleBitmap(IntPtr dc, int width, int height);

        [LibraryImport("gdi32.dll")]
        public static partial IntPtr SelectObject(IntPtr dc, IntPtr handle);

        [LibraryImport("gdi32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static partial bool BitBlt(IntPtr destination, int x, int y, int width, int height,
            IntPtr source, int sourceX, int sourceY, int raster);

        [LibraryImport("gdi32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static partial bool DeleteObject(IntPtr handle);

        [LibraryImport("gdi32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static partial bool DeleteDC(IntPtr dc);
    }
}

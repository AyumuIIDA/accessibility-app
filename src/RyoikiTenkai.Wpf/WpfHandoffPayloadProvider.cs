using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using RyoikiTenkai.Actions;

namespace RyoikiTenkai.Wpf;

internal sealed class WpfHandoffPayloadProvider : IHandoffPayloadProvider
{
    private const int SmXVirtualScreen = 76;
    private const int SmYVirtualScreen = 77;
    private const int SmCxVirtualScreen = 78;
    private const int SmCyVirtualScreen = 79;
    private const int Srccopy = 0x00CC0020;
    private const int DibRgbColors = 0;
    private const int BiRgb = 0;

    public Task<HandoffPayload?> CaptureScreenshotAsync(CancellationToken cancellationToken)
    {
        return Task.Run(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();

            var x = GetSystemMetrics(SmXVirtualScreen);
            var y = GetSystemMetrics(SmYVirtualScreen);
            var width = GetSystemMetrics(SmCxVirtualScreen);
            var height = GetSystemMetrics(SmCyVirtualScreen);
            if (width <= 0 || height <= 0)
            {
                return null;
            }

            var pixels = CaptureBgraPixels(x, y, width, height);
            cancellationToken.ThrowIfCancellationRequested();

            var bitmap = BitmapSource.Create(
                width,
                height,
                96,
                96,
                PixelFormats.Bgra32,
                null,
                pixels,
                width * 4);
            bitmap.Freeze();

            using var stream = new MemoryStream();
            var encoder = new PngBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(bitmap));
            encoder.Save(stream);

            var now = DateTimeOffset.UtcNow;
            return new HandoffPayload(
                PayloadId: Guid.NewGuid().ToString("N"),
                FileName: $"handoff-{now:yyyyMMdd-HHmmss}.png",
                ContentType: "image/png",
                Data: stream.ToArray(),
                CreatedAt: now);
        }, cancellationToken);
    }

    public Task OpenReceivedFileAsync(string path, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        Process.Start(new ProcessStartInfo
        {
            FileName = path,
            UseShellExecute = true
        });
        return Task.CompletedTask;
    }

    private static byte[] CaptureBgraPixels(int x, int y, int width, int height)
    {
        var screenDc = GetDC(IntPtr.Zero);
        if (screenDc == IntPtr.Zero)
        {
            throw new InvalidOperationException("GetDC failed.");
        }

        var memoryDc = IntPtr.Zero;
        var bitmap = IntPtr.Zero;
        var previous = IntPtr.Zero;
        try
        {
            memoryDc = CreateCompatibleDC(screenDc);
            if (memoryDc == IntPtr.Zero)
            {
                throw new InvalidOperationException("CreateCompatibleDC failed.");
            }

            bitmap = CreateCompatibleBitmap(screenDc, width, height);
            if (bitmap == IntPtr.Zero)
            {
                throw new InvalidOperationException("CreateCompatibleBitmap failed.");
            }

            previous = SelectObject(memoryDc, bitmap);
            if (!BitBlt(memoryDc, 0, 0, width, height, screenDc, x, y, Srccopy))
            {
                throw new InvalidOperationException("BitBlt failed.");
            }

            var info = new BitmapInfo
            {
                Header = new BitmapInfoHeader
                {
                    Size = Marshal.SizeOf<BitmapInfoHeader>(),
                    Width = width,
                    Height = -height,
                    Planes = 1,
                    BitCount = 32,
                    Compression = BiRgb
                }
            };

            var pixels = new byte[checked(width * height * 4)];
            var scanLines = GetDIBits(screenDc, bitmap, 0, (uint)height, pixels, ref info, DibRgbColors);
            if (scanLines == 0)
            {
                throw new InvalidOperationException("GetDIBits failed.");
            }

            return pixels;
        }
        finally
        {
            if (previous != IntPtr.Zero && memoryDc != IntPtr.Zero)
            {
                SelectObject(memoryDc, previous);
            }

            if (bitmap != IntPtr.Zero)
            {
                DeleteObject(bitmap);
            }

            if (memoryDc != IntPtr.Zero)
            {
                DeleteDC(memoryDc);
            }

            ReleaseDC(IntPtr.Zero, screenDc);
        }
    }

    [DllImport("user32.dll")]
    private static extern int GetSystemMetrics(int nIndex);

    [DllImport("user32.dll")]
    private static extern IntPtr GetDC(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern int ReleaseDC(IntPtr hWnd, IntPtr hDc);

    [DllImport("gdi32.dll")]
    private static extern IntPtr CreateCompatibleDC(IntPtr hDc);

    [DllImport("gdi32.dll")]
    private static extern bool DeleteDC(IntPtr hDc);

    [DllImport("gdi32.dll")]
    private static extern IntPtr CreateCompatibleBitmap(IntPtr hDc, int width, int height);

    [DllImport("gdi32.dll")]
    private static extern IntPtr SelectObject(IntPtr hDc, IntPtr obj);

    [DllImport("gdi32.dll")]
    private static extern bool DeleteObject(IntPtr obj);

    [DllImport("gdi32.dll")]
    private static extern bool BitBlt(
        IntPtr hdcDest,
        int xDest,
        int yDest,
        int width,
        int height,
        IntPtr hdcSrc,
        int xSrc,
        int ySrc,
        int rasterOperation);

    [DllImport("gdi32.dll")]
    private static extern int GetDIBits(
        IntPtr hdc,
        IntPtr hbm,
        uint start,
        uint lines,
        byte[] bits,
        ref BitmapInfo bitmapInfo,
        int usage);

    [StructLayout(LayoutKind.Sequential)]
    private struct BitmapInfo
    {
        public BitmapInfoHeader Header;
        public uint Colors;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct BitmapInfoHeader
    {
        public int Size;
        public int Width;
        public int Height;
        public ushort Planes;
        public ushort BitCount;
        public int Compression;
        public int SizeImage;
        public int XPelsPerMeter;
        public int YPelsPerMeter;
        public int ClrUsed;
        public int ClrImportant;
    }
}

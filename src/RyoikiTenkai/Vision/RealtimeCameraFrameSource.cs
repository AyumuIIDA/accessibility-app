using System.Runtime.InteropServices.WindowsRuntime;
using Windows.Devices.Enumeration;
using Windows.Graphics.Imaging;
using Windows.Media.Capture;
using Windows.Media.Capture.Frames;
using Windows.Media.MediaProperties;

namespace RyoikiTenkai.Vision;

internal sealed class RealtimeCameraFrameSource : IAsyncDisposable
{
    private readonly object _gate = new();
    private MediaCapture? _mediaCapture;
    private MediaFrameReader? _reader;
    private CameraFrame? _latestFrame;
    private TaskCompletionSource<CameraFrame>? _nextFrame;
    private int _frameErrorCount;

    public event Action<string>? DiagnosticLogged;

    public async Task InitializeAsync()
    {
        var groups = await MediaFrameSourceGroup.FindAllAsync();
        Log($"MediaFrameSourceGroup count: {groups.Count}");

        var selectedGroup = groups.FirstOrDefault(group =>
            group.SourceInfos.Any(info =>
                info.SourceKind == MediaFrameSourceKind.Color &&
                info.DeviceInformation?.Name.Contains("front", StringComparison.OrdinalIgnoreCase) == true))
            ?? groups.FirstOrDefault(group => group.SourceInfos.Any(info => info.SourceKind == MediaFrameSourceKind.Color));

        if (selectedGroup is null)
        {
            throw new InvalidOperationException("No realtime camera frame source found.");
        }

        Log($"Selected source group: {selectedGroup.DisplayName}");
        foreach (var info in selectedGroup.SourceInfos.Where(x => x.SourceKind == MediaFrameSourceKind.Color))
        {
            Log($"Color source info: {info.Id}, device={info.DeviceInformation?.Name ?? "(unknown)"}");
        }

        _mediaCapture = new MediaCapture();
        await _mediaCapture.InitializeAsync(new MediaCaptureInitializationSettings
        {
            SourceGroup = selectedGroup,
            SharingMode = MediaCaptureSharingMode.ExclusiveControl,
            StreamingCaptureMode = StreamingCaptureMode.Video,
            MemoryPreference = MediaCaptureMemoryPreference.Cpu
        });
        Log("MediaCapture initialized.");

        var colorSource = _mediaCapture.FrameSources.Values
            .Where(source => source.Info.SourceKind == MediaFrameSourceKind.Color)
            .OrderByDescending(source => source.Info.DeviceInformation?.Name.Contains("front", StringComparison.OrdinalIgnoreCase) == true)
            .First();
        Log($"Selected frame source: {colorSource.Info.Id}, device={colorSource.Info.DeviceInformation?.Name ?? "(unknown)"}");
        LogSupportedFormats(colorSource);

        await TrySetPreferredFormatAsync(colorSource);

        _reader = await _mediaCapture.CreateFrameReaderAsync(colorSource);
        _reader.AcquisitionMode = MediaFrameReaderAcquisitionMode.Realtime;
        _reader.FrameArrived += OnFrameArrived;

        var status = await _reader.StartAsync();
        Log($"MediaFrameReader start status: {status}");
        if (status != MediaFrameReaderStartStatus.Success)
        {
            throw new InvalidOperationException($"Camera frame reader failed to start: {status}");
        }
    }

    public async Task<CameraFrame> WaitForFrameAsync(CancellationToken cancellationToken)
    {
        lock (_gate)
        {
            if (_latestFrame is not null)
            {
                var frame = _latestFrame;
                _latestFrame = null;
                return frame;
            }

            _nextFrame ??= new TaskCompletionSource<CameraFrame>(TaskCreationOptions.RunContinuationsAsynchronously);
        }

        await using var registration = cancellationToken.Register(() =>
        {
            lock (_gate)
            {
                _nextFrame?.TrySetCanceled(cancellationToken);
                _nextFrame = null;
            }
        });

        return await _nextFrame.Task;
    }

    private async void OnFrameArrived(MediaFrameReader sender, MediaFrameArrivedEventArgs args)
    {
        try
        {
            using var frame = sender.TryAcquireLatestFrame();
            var bitmap = frame?.VideoMediaFrame?.SoftwareBitmap;
            if (bitmap is null)
            {
                return;
            }

            var cameraFrame = await ConvertAsync(bitmap);
            TaskCompletionSource<CameraFrame>? completion;
            lock (_gate)
            {
                _latestFrame = cameraFrame;
                completion = _nextFrame;
                _nextFrame = null;
            }

            completion?.TrySetResult(cameraFrame);
        }
        catch (Exception ex)
        {
            _frameErrorCount++;
            if (_frameErrorCount <= 3)
            {
                Log("Failed to convert a camera frame: " + ex.Message);
            }
        }
    }

    private static async Task<CameraFrame> ConvertAsync(SoftwareBitmap source)
    {
        using var converted = SoftwareBitmap.Convert(source, BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied);
        var width = converted.PixelWidth;
        var height = converted.PixelHeight;
        var bytes = new byte[width * height * 4];
        var buffer = bytes.AsBuffer();
        converted.CopyToBuffer(buffer);
        await Task.CompletedTask;
        return new CameraFrame(width, height, bytes);
    }

    private async Task TrySetPreferredFormatAsync(MediaFrameSource source)
    {
        var preferred = source.SupportedFormats
            .Where(format => IsSupportedVideoSubtype(format.Subtype))
            .Where(format => format.VideoFormat.Width >= 640 && format.VideoFormat.Height >= 360)
            .OrderBy(format => Math.Abs((int)format.VideoFormat.Width - 1280))
            .ThenBy(format => Math.Abs((int)format.VideoFormat.Height - 720))
            .ThenBy(format => Math.Abs(GetFrameRate(format) - 30))
            .FirstOrDefault();

        if (preferred is not null)
        {
            Log($"Using camera format: {preferred.VideoFormat.Width}x{preferred.VideoFormat.Height}, subtype={preferred.Subtype}, fps={FormatFrameRate(preferred)}");
            await source.SetFormatAsync(preferred);
        }
        else
        {
            Log("Using default camera format.");
        }
    }

    private static bool IsSupportedVideoSubtype(string subtype)
    {
        return string.Equals(subtype, MediaEncodingSubtypes.Bgra8, StringComparison.OrdinalIgnoreCase)
            || string.Equals(subtype, MediaEncodingSubtypes.Nv12, StringComparison.OrdinalIgnoreCase)
            || string.Equals(subtype, MediaEncodingSubtypes.Yuy2, StringComparison.OrdinalIgnoreCase)
            || string.Equals(subtype, MediaEncodingSubtypes.Rgb32, StringComparison.OrdinalIgnoreCase);
    }

    private void LogSupportedFormats(MediaFrameSource source)
    {
        foreach (var format in source.SupportedFormats
            .Where(format => format.VideoFormat.Width > 0 && format.VideoFormat.Height > 0)
            .OrderByDescending(format => format.VideoFormat.Width * format.VideoFormat.Height)
            .Take(12))
        {
            Log($"Camera format: {format.VideoFormat.Width}x{format.VideoFormat.Height}, subtype={format.Subtype}, fps={FormatFrameRate(format)}");
        }
    }

    private static string FormatFrameRate(MediaFrameFormat format)
    {
        return GetFrameRate(format).ToString("0.##");
    }

    private static double GetFrameRate(MediaFrameFormat format)
    {
        var numerator = format.FrameRate.Numerator;
        var denominator = format.FrameRate.Denominator;
        if (denominator == 0)
        {
            return 0;
        }

        return numerator / (double)denominator;
    }

    private void Log(string message)
    {
        DiagnosticLogged?.Invoke(message);
    }

    public async ValueTask DisposeAsync()
    {
        if (_reader is not null)
        {
            _reader.FrameArrived -= OnFrameArrived;
            await _reader.StopAsync();
            _reader.Dispose();
            _reader = null;
        }

        _mediaCapture?.Dispose();
        _mediaCapture = null;
    }
}

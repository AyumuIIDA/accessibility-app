using Windows.Devices.Enumeration;
using Windows.Graphics.Imaging;
using Windows.Media.Capture;
using Windows.Media.MediaProperties;
using Windows.Storage.Streams;

namespace RyoikiTenkai.Vision;

internal sealed class CameraFrameSource : IAsyncDisposable
{
    private MediaCapture? _mediaCapture;

    public async Task InitializeAsync()
    {
        var cameras = await DeviceInformation.FindAllAsync(DeviceClass.VideoCapture);
        if (cameras.Count == 0)
        {
            throw new InvalidOperationException("No camera device found.");
        }

        foreach (var camera in cameras)
        {
            Console.WriteLine($"Camera candidate: {camera.Name}");
        }

        var selectedCamera = cameras
            .FirstOrDefault(x => x.Name.Contains("front", StringComparison.OrdinalIgnoreCase))
            ?? cameras[0];

        _mediaCapture = new MediaCapture();
        await _mediaCapture.InitializeAsync(new MediaCaptureInitializationSettings
        {
            VideoDeviceId = selectedCamera.Id,
            StreamingCaptureMode = StreamingCaptureMode.Video
        });

        Console.WriteLine($"Camera initialized: {selectedCamera.Name}");
    }

    public async Task<CameraFrame> CaptureFrameAsync()
    {
        if (_mediaCapture is null)
        {
            throw new InvalidOperationException("Camera is not initialized.");
        }

        using var stream = new InMemoryRandomAccessStream();
        await _mediaCapture.CapturePhotoToStreamAsync(ImageEncodingProperties.CreateJpeg(), stream);
        stream.Seek(0);

        var decoder = await BitmapDecoder.CreateAsync(stream);
        var targetWidth = Math.Min(320u, decoder.PixelWidth);
        var targetHeight = Math.Max(1u, decoder.PixelHeight * targetWidth / decoder.PixelWidth);
        var transform = new BitmapTransform
        {
            ScaledWidth = targetWidth,
            ScaledHeight = targetHeight
        };

        var pixelData = await decoder.GetPixelDataAsync(
            BitmapPixelFormat.Bgra8,
            BitmapAlphaMode.Premultiplied,
            transform,
            ExifOrientationMode.IgnoreExifOrientation,
            ColorManagementMode.DoNotColorManage);

        return new CameraFrame((int)targetWidth, (int)targetHeight, pixelData.DetachPixelData());
    }

    public ValueTask DisposeAsync()
    {
        _mediaCapture?.Dispose();
        return ValueTask.CompletedTask;
    }
}

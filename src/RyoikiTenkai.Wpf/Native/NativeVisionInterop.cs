using System.Runtime.InteropServices;
using System.Text;

namespace RyoikiTenkai.Wpf.Native;

internal static partial class NativeVisionInterop
{
    public const string LibraryName = "RyoikiTenkai.Native";
    public const uint AbiVersion = 4;

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_get_abi_version")]
    public static partial uint GetAbiVersion();

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_create")]
    public static partial IntPtr Create(IntPtr parentHwnd);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_start")]
    public static partial int Start(IntPtr handle);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_stop")]
    public static partial void Stop(IntPtr handle);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_resize")]
    public static partial int Resize(IntPtr handle, int width, int height);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_destroy")]
    public static partial void Destroy(IntPtr handle);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_get_latest_metrics")]
    public static partial int GetLatestMetrics(IntPtr handle, out NativeVisionMetrics metrics);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_get_latest_palm")]
    public static partial int GetLatestPalm(IntPtr handle, out NativePalmResult result);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_get_latest_hand")]
    public static partial int GetLatestHand(IntPtr handle, out NativeHandResult result);

    [DllImport(LibraryName, EntryPoint = "ryoiki_get_last_error")]
    private static extern int GetLastError(IntPtr handle, byte[] buffer, int bufferLength);

    public static string GetLastErrorMessage(IntPtr handle)
    {
        var buffer = new byte[1024];
        if (GetLastError(handle, buffer, buffer.Length) == 0)
        {
            return string.Empty;
        }

        var length = Array.IndexOf(buffer, (byte)0);
        if (length < 0)
        {
            length = buffer.Length;
        }

        return length > 0
            ? Encoding.UTF8.GetString(buffer, 0, length)
            : string.Empty;
    }
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeVisionMetrics
{
    public uint AbiVersion;
    public uint StructSize;
    public ulong FrameId;
    public ulong CaptureTimestampUs;
    public double RuntimeSeconds;
    public double CameraFps;
    public double DisplayFps;
    public double PerceptionFps;
    public double CameraWaitMs;
    public double FrameCopyMs;
    public double PreprocessMs;
    public double PalmInferenceMs;
    public double PalmPostprocessMs;
    public double RoiCropWarpMs;
    public double HandInferenceMs;
    public double LandmarkPostprocessMs;
    public double TrackingUpdateMs;
    public double OverlayRenderMs;
    public double EndToEndLatencyMs;
    public double NativeOverheadMs;
    public ulong FramePoolDroppedFrames;
    public ulong PerceptionDroppedFrames;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativePalmResult
{
    public uint AbiVersion;
    public uint StructSize;
    public ulong FrameId;
    public int PalmCount;
    public float Confidence;
    public fixed float Bbox[4];
    public fixed float Keypoints[7 * 2];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeHandResult
{
    public uint AbiVersion;
    public uint StructSize;
    public ulong FrameId;
    public int HandCount;
    public float Confidence;
    public float Handedness;
    public fixed float Bbox[4];
    public fixed float Landmarks[21 * 3];
}

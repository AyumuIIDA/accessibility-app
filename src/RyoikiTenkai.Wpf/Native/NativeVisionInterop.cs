using System.Runtime.InteropServices;
using System.Text;

namespace RyoikiTenkai.Wpf.Native;

internal static partial class NativeVisionInterop
{
    public const string LibraryName = "RyoikiTenkai.Native";
    public const uint AbiVersion = 6;

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
internal unsafe struct NativeVisionMetrics
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
    public uint CaptureWidth;
    public uint CaptureHeight;
    public uint UprightWidth;
    public uint UprightHeight;
    public ulong FrameBytes;
    public ulong TensorInputBytes;
    public double GraphTotalMs;
    public double PerceptionFrameAgeMs;
    public double RenderFrameAgeMs;
    public fixed byte CameraSubtypeBytes[32];
    public fixed byte PalmProviderBytes[64];
    public fixed byte HandProviderBytes[64];
    public fixed byte ProviderFallbackReasonBytes[256];

    public string CameraSubtype
    {
        get
        {
            fixed (byte* buffer = CameraSubtypeBytes)
            {
                return DecodeUtf8(buffer, 32);
            }
        }
    }

    public string PalmProvider
    {
        get
        {
            fixed (byte* buffer = PalmProviderBytes)
            {
                return DecodeUtf8(buffer, 64);
            }
        }
    }

    public string HandProvider
    {
        get
        {
            fixed (byte* buffer = HandProviderBytes)
            {
                return DecodeUtf8(buffer, 64);
            }
        }
    }

    public string ProviderFallbackReason
    {
        get
        {
            fixed (byte* buffer = ProviderFallbackReasonBytes)
            {
                return DecodeUtf8(buffer, 256);
            }
        }
    }

    private static string DecodeUtf8(byte* buffer, int bufferLength)
    {
        var length = 0;
        while (length < bufferLength && buffer[length] != 0)
        {
            length++;
        }

        return length == 0
            ? string.Empty
            : Encoding.UTF8.GetString(buffer, length);
    }
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

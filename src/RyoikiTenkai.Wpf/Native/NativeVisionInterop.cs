using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;
using System.Text;

namespace RyoikiTenkai.Wpf.Native;

internal static partial class NativeVisionInterop
{
    public const string LibraryName = "RyoikiTenkai.Native";
    public const uint AbiVersion = 18;
    public const uint DomainExpansionStateId = 1;
    public const uint OpenPalmStateId = 2;
    public const uint SwipeLeftEventId = 1;
    public const uint SwipeRightEventId = 2;

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

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_cad_create")]
    public static partial IntPtr CreateCad(IntPtr parentHwnd);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_cad_resize")]
    public static partial int ResizeCad(IntPtr handle, int width, int height);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_cad_set_view")]
    public static partial int SetCadView(IntPtr handle, float yawRadians, float pitchRadians, float zoom);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_cad_get_view")]
    public static partial int GetCadView(IntPtr handle, out float yawRadians, out float pitchRadians, out float zoom);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_cad_set_view_ex")]
    public static partial int SetCadViewEx(
        IntPtr handle,
        float yawRadians,
        float pitchRadians,
        float zoom,
        float panX,
        float panY);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_cad_get_view_ex")]
    public static partial int GetCadViewEx(
        IntPtr handle,
        out float yawRadians,
        out float pitchRadians,
        out float zoom,
        out float panX,
        out float panY);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_cad_reset_view")]
    public static partial void ResetCadView(IntPtr handle);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_cad_destroy")]
    public static partial void DestroyCad(IntPtr handle);

    [DllImport(LibraryName, EntryPoint = "ryoiki_cad_get_last_error")]
    private static extern int GetCadLastError(IntPtr handle, byte[] buffer, int bufferLength);

    public static string GetCadLastErrorMessage(IntPtr handle)
    {
        var buffer = new byte[1024];
        if (GetCadLastError(handle, buffer, buffer.Length) == 0) return string.Empty;
        var length = Array.IndexOf(buffer, (byte)0);
        return Encoding.UTF8.GetString(buffer, 0, length < 0 ? buffer.Length : length);
    }

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_get_latest_metrics")]
    public static partial int GetLatestMetrics(IntPtr handle, out NativeVisionMetrics metrics);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_get_latest_palm")]
    public static partial int GetLatestPalm(IntPtr handle, out NativePalmResult result);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_get_latest_hand")]
    public static partial int GetLatestHand(IntPtr handle, out NativeHandResult result);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_get_latest_states")]
    public static partial int GetLatestStates(IntPtr handle, out NativeHandStateSnapshot snapshot);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_read_hand_events")]
    public static partial int ReadHandEvents(
        IntPtr handle,
        ulong afterSequence,
        out NativeHandEventBatch batch);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_capture_palm_rotation_reference")]
    public static partial int CapturePalmRotationReference(IntPtr handle);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_set_hand_presentation_mode")]
    public static partial int SetHandPresentationMode(
        IntPtr handle,
        int presentationMode);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_configure_cad_hand_interaction")]
    public static partial int ConfigureCadHandInteraction(
        IntPtr visionHandle,
        IntPtr cadHandle,
        int interactionMode,
        int presentationMode,
        float rotationSensitivity);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_get_cad_hand_interaction")]
    public static partial int GetCadHandInteraction(
        IntPtr visionHandle,
        out NativeCadHandInteractionResult result);

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
    public double CameraUploadMs;
    public double CameraDrawMs;
    public double OverlayDrawMs;
    public double Hand3dDrawMs;
    public double EndDrawMs;
    public double PresentWaitMs;
    public double OverlayRenderMs;
    public double EndToEndLatencyMs;
    public double NativeOverheadMs;
    public ulong FramePoolDroppedFrames;
    public ulong PerceptionDroppedFrames;
    public ulong GpuCameraFrames;
    public ulong GpuRenderedFrames;
    public uint GpuCameraDxgiFormat;
    public uint GpuCameraSubresource;
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
    public ulong CaptureTimestampUs;
    public int HandCount;
    public float Confidence;
    public float Handedness;
    public fixed float Bbox[4];
    public fixed float Landmarks[21 * 3];
    public fixed float WorldLandmarks[21 * 3];
    public fixed float PalmNormal[3];
    public fixed float PalmRotationBasis[3 * 3];
    public fixed float PalmCenter[3];
    public float PalmScale;
    public int PalmPoseValid;
    public fixed float ScreenPalmCenter[2];
    public float ScreenPalmScale;
    public int ScreenPalmValid;
    public fixed float PalmRelativeRotation[3 * 3];
    public float PalmRotationFitError;
    public int PalmRelativeRotationValid;
    public float DomainSignConfidence;
    public int DomainSignDetected;
    public fixed float DomainSignFeatures[6];
}

internal enum NativeCadHandInteractionMode
{
    None,
    Rotate,
    Pan,
    Zoom
}

internal enum NativeHandPresentationMode
{
    MirrorDirect,
    Physical
}

internal enum NativeCadHandInteractionState
{
    Inactive,
    Ready,
    Rotating,
    Panning,
    Zooming,
    Suspended,
    AwaitingRelease
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeCadHandInteractionResult
{
    public uint AbiVersion;
    public uint StructSize;
    public ulong FrameId;
    public NativeCadHandInteractionState State;
    public int ViewChanged;
    public float YawRadians;
    public float PitchRadians;
    public float Zoom;
    public float PanX;
    public float PanY;
    public float YawDeltaDegrees;
    public float PitchDeltaDegrees;
}

internal enum NativeHandStatePhase : uint
{
    Inactive = 0,
    Candidate = 1,
    Active = 2
}

internal enum NativeHandStateTransition : uint
{
    None = 0,
    Began = 1,
    Ended = 2,
    Cancelled = 3
}

[Flags]
internal enum NativeHandStateFlags : uint
{
    None = 0,
    Stale = 1 << 0,
    ExitPending = 1 << 1
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeHandState
{
    public uint Id;
    public NativeHandStatePhase Phase;
    public NativeHandStateTransition Transition;
    public NativeHandStateFlags Flags;
    public float Confidence;
    public float InputQuality;
    public fixed uint Reserved0[2];
    public ulong BeganFrameId;
    public ulong CurrentFrameId;
    public ulong TimestampUs;
    public fixed uint Reserved[4];
}

[InlineArray(16)]
internal struct NativeHandStateBuffer
{
    private NativeHandState _element0;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeHandStateSnapshot
{
    public uint AbiVersion;
    public uint StructSize;
    public ulong FrameId;
    public ulong TimestampUs;
    public uint Count;
    public uint Flags;
    public NativeHandStateBuffer States;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeHandEvent
{
    public ulong Sequence;
    public uint Id;
    public uint Flags;
    public float Confidence;
    public float InputQuality;
    public float DisplacementX;
    public float DisplacementY;
    public ulong BeganFrameId;
    public ulong EndedFrameId;
    public ulong BeganTimestampUs;
    public ulong EndedTimestampUs;
    public ulong DurationUs;
    public fixed uint Reserved[2];
}

[InlineArray(16)]
internal struct NativeHandEventBuffer
{
    private NativeHandEvent _element0;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeHandEventBatch
{
    public uint AbiVersion;
    public uint StructSize;
    public ulong NextSequence;
    public ulong DroppedCount;
    public uint Count;
    public uint Flags;
    public NativeHandEventBuffer Events;
}

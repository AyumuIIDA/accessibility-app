using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;
using System.Text;

namespace RyoikiTenkai.Wpf.Native;

internal static partial class NativeVisionInterop
{
    public const string LibraryName = "RyoikiTenkai.Native";
    public const uint AbiVersion = 29;
    public const int MaxGestureRecordings = 3;
    public const int MaxGestureDefinitions = 8;
    public const int MaxHands = 2;
    public const uint DomainExpansionStateId = 1;

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

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_dtw_debug_create")]
    public static partial IntPtr CreateDtwDebug(IntPtr parentHwnd, IntPtr sourceHandle);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_dtw_debug_resize")]
    public static partial int ResizeDtwDebug(IntPtr handle, int width, int height);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_dtw_debug_destroy")]
    public static partial void DestroyDtwDebug(IntPtr handle);

    [DllImport(LibraryName, EntryPoint = "ryoiki_dtw_debug_get_last_error")]
    private static extern int GetDtwDebugLastError(IntPtr handle, byte[] buffer, int bufferLength);

    public static string GetDtwDebugLastErrorMessage(IntPtr handle)
    {
        var buffer = new byte[1024];
        if (GetDtwDebugLastError(handle, buffer, buffer.Length) == 0) return string.Empty;
        var length = Array.IndexOf(buffer, (byte)0);
        return Encoding.UTF8.GetString(buffer, 0, length < 0 ? buffer.Length : length);
    }

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

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_get_latest_hands")]
    public static partial int GetLatestHands(IntPtr handle, out NativeHandsResult result);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_get_latest_hand_topology")]
    public static partial int GetLatestHandTopology(IntPtr handle, out NativeHandTopologySnapshot snapshot);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_get_latest_gesture_recognition")]
    public static partial int GetLatestGestureRecognition(IntPtr handle, out NativeGestureRecognitionSnapshot snapshot);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_get_latest_gesture_dtw_debug")]
    public static partial int GetLatestGestureDtwDebug(IntPtr handle, out NativeGestureDtwDebugSnapshot snapshot);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_request_gesture_template_registration")]
    public static partial int RequestGestureTemplateRegistration(IntPtr handle, uint trackId, uint templateId);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_begin_gesture_recording")]
    public static partial int BeginGestureRecording(IntPtr handle, uint templateId);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_finish_gesture_recording")]
    public static partial int FinishGestureRecording(IntPtr handle);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_cancel_gesture_recording")]
    public static partial int CancelGestureRecording(IntPtr handle);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_get_gesture_recording_status")]
    public static partial int GetGestureRecordingStatus(IntPtr handle, out NativeGestureRecordingStatus status);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_list_gesture_definitions")]
    public static partial int ListGestureDefinitions(
        IntPtr handle,
        out NativeGestureDefinitionList definitions);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_set_gesture_definition_metadata",
        StringMarshalling = StringMarshalling.Utf8)]
    public static partial int SetGestureDefinitionMetadata(
        IntPtr handle,
        uint definitionId,
        string name,
        uint enabled);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_delete_gesture_definition")]
    public static partial int DeleteGestureDefinition(IntPtr handle, uint definitionId);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_reload_gesture_definitions")]
    public static partial int ReloadGestureDefinitions(IntPtr handle);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_list_gesture_recordings")]
    public static partial int ListGestureRecordings(
        IntPtr handle, uint definitionId, out NativeGestureRecordingList recordings);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_export_gesture_recording",
        StringMarshalling = StringMarshalling.Utf8)]
    public static partial int ExportGestureRecording(
        IntPtr handle, uint definitionId, uint takeIndex, string path);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_recording_playback_create")]
    public static partial IntPtr CreateRecordingPlayback(IntPtr parentHwnd, IntPtr sourceHandle);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_recording_playback_resize")]
    public static partial int ResizeRecordingPlayback(IntPtr handle, int width, int height);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_recording_playback_select")]
    public static partial int SelectRecordingPlayback(IntPtr handle, uint definitionId, uint takeIndex);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_recording_playback_set_playing")]
    public static partial int SetRecordingPlaybackPlaying(IntPtr handle, uint playing);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_recording_playback_seek")]
    public static partial int SeekRecordingPlayback(IntPtr handle, float normalizedProgress);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_recording_playback_get_status")]
    public static partial int GetRecordingPlaybackStatus(
        IntPtr handle, out NativeGesturePlaybackStatus status);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_recording_playback_destroy")]
    public static partial void DestroyRecordingPlayback(IntPtr handle);

    [DllImport(LibraryName, EntryPoint = "ryoiki_recording_playback_get_last_error")]
    private static extern int GetRecordingPlaybackLastError(IntPtr handle, byte[] buffer, int bufferLength);

    public static string GetRecordingPlaybackLastErrorMessage(IntPtr handle)
    {
        var buffer = new byte[1024];
        if (GetRecordingPlaybackLastError(handle, buffer, buffer.Length) == 0) return string.Empty;
        var length = Array.IndexOf(buffer, (byte)0);
        return Encoding.UTF8.GetString(buffer, 0, length < 0 ? buffer.Length : length);
    }

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_list_gesture_bindings")]
    public static partial int ListGestureBindings(IntPtr handle, out NativeGestureBindingList bindings);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_upsert_gesture_binding",
        StringMarshalling = StringMarshalling.Utf8)]
    public static partial int UpsertGestureBinding(IntPtr handle, uint definitionId,
        string actionType, string actionParameter, uint enabled);

    [LibraryImport(LibraryName, EntryPoint = "ryoiki_delete_gesture_binding")]
    public static partial int DeleteGestureBinding(IntPtr handle, uint definitionId);

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

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeObservedHand
{
    public uint TrackId;
    public uint Flags;
    public float Confidence;
    public float Handedness;
    public float FilteredHandedness;
    public fixed float Bbox[4];
    public fixed float Landmarks[21 * 3];
    public fixed float WorldLandmarks[21 * 3];
}

[InlineArray(NativeVisionInterop.MaxHands)]
internal struct NativeObservedHandBuffer
{
    private NativeObservedHand _element0;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeHandsResult
{
    public uint AbiVersion;
    public uint StructSize;
    public ulong FrameId;
    public ulong CaptureTimestampUs;
    public uint HandCount;
    public uint Flags;
    public uint CrossingOwnerTrackId;
    public uint Reserved;
    public NativeObservedHandBuffer Hands;
}

internal enum NativeTwoHandOrdering : uint
{
    None = 0,
    Handedness = 1,
    ScreenPosition = 2
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeHandTopologySummary
{
    public uint TrackId;
    public uint Valid;
    public uint SampleCount;
    public uint FingerStateTransitionCount;
    public uint StartFingerStateMask;
    public uint EndFingerStateMask;
    public uint SignedPalmAreaSignChanges;
    public uint Reserved;
    public ulong FirstFrameId;
    public ulong LastFrameId;
    public ulong DurationUs;
    public float PalmTravel;
    public float PalmOrientationRangeRadians;
    public float HandednessRange;
    public float HandednessMean;
    public float FingerStraightnessRangeMax;
    public float SignedPalmAreaRange;
    public float PalmCompressionMin;
    public float PalmCompressionMax;
    public float PalmCompressionDrop;
    public float PalmDepthRangeMax;
    public float PalmTurnScore;
    public float HandScaleRatioRange;
    public float HandScaleRatioDelta;
    public float BoundingBoxAreaRatioRange;
    public float BoundingBoxAreaRatioDelta;
    public float TranslationDeltaX;
    public float TranslationDeltaY;
    public float TranslationDistance;
    public float TopologyChangeScore;
    public float ReservedFloat;
}

[InlineArray(NativeVisionInterop.MaxHands)]
internal struct NativeHandTopologySummaryBuffer
{
    private NativeHandTopologySummary _element0;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeTwoHandRelation
{
    public uint Valid;
    public uint FirstTrackId;
    public uint SecondTrackId;
    public NativeTwoHandOrdering Ordering;
    public float DeltaX;
    public float DeltaY;
    public float Distance;
    public float AngleRadians;
    public float ScaleRatio;
    public float Quality;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeHandTopologySnapshot
{
    public uint AbiVersion;
    public uint StructSize;
    public ulong FrameId;
    public ulong CaptureTimestampUs;
    public uint SummaryCount;
    public uint Reserved;
    public NativeHandTopologySummaryBuffer Summaries;
    public NativeTwoHandRelation Relation;
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

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeGestureRecognitionSnapshot
{
    public uint AbiVersion, StructSize;
    public ulong FrameId, TimestampUs, BestCandidateStartTimestampUs, BestCandidateEndTimestampUs;
    public uint TrackId, HasResult, TemplateCount, CandidateCount, BestTemplateId, BestCandidateDurationMs;
    public uint BestEligible, BestUsableFrameCount, BestSourceFrameCount, BestActiveSegmentValid;
    public float BestScore, BestConfidence, BestWarpRatio;
    public uint BestHasWarpRatio;
    public float BestReverseScore;
    public uint BestHasReverseScore;
    public float BestActiveSegmentPathVelocity;
    public uint LastRegistrationStatus, LastRegistrationTemplateId;
    public fixed byte BestRejectionReason[128];
    public fixed byte LastRegistrationReason[128];
    public fixed uint Reserved[4];
}

internal enum NativeGestureRecordingState : uint
{
    Idle = 0,
    AwaitingHand = 1,
    Recording = 2,
    Completed = 3,
    Rejected = 4,
    Cancelled = 5,
    TrackLost = 6,
    AwaitingNextTake = 7
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeGestureRecordingStatus
{
    public uint AbiVersion, StructSize;
    public NativeGestureRecordingState State;
    public uint TrackId;
    public ulong BeganFrameId, BeganTimestampUs, LastFrameId, LastTimestampUs;
    public uint SampleCount, UsableSampleCount, LastResultTemplateId, CurrentTake;
    public fixed byte RejectionReason[128];
    public uint AcceptedTakeCount, RequiredTakeCount, AttemptCount, Reserved0;

    public string GetReason()
    {
        fixed (byte* reason = RejectionReason)
            return Marshal.PtrToStringUTF8((IntPtr)reason) ?? string.Empty;
    }
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeGestureDefinitionMetadata
{
    public uint DefinitionId, Enabled, TakeCount;
    public fixed byte Name[64];

    public string GetName()
    {
        fixed (byte* name = Name)
            return Marshal.PtrToStringUTF8((IntPtr)name) ?? string.Empty;
    }
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeGestureDefinitionList
{
    public uint AbiVersion, StructSize, Count;
    public fixed byte Items[NativeVisionInterop.MaxGestureDefinitions * 76];
    public fixed byte Error[128];

    public NativeGestureDefinitionMetadata GetItem(int index)
    {
        if ((uint)index >= Math.Min(Count, (uint)NativeVisionInterop.MaxGestureDefinitions))
            throw new ArgumentOutOfRangeException(nameof(index));
        fixed (byte* items = Items)
            return ((NativeGestureDefinitionMetadata*)items)[index];
    }

    public string GetError()
    {
        fixed (byte* error = Error)
            return Marshal.PtrToStringUTF8((IntPtr)error) ?? string.Empty;
    }
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeGestureRecordingMetadata
{
    public uint TakeIndex, Accepted;
    public ulong CapturedAtUs;
    public double DurationMs;
    public float AverageConfidence;
    public uint SourceFrameCount, ValidLandmarkFrameCount, HighConfidenceFrameCount, FrameCount;
    public float EffectiveFps, UsableEffectiveFps;
    public fixed byte SourceId[64];
    public fixed byte QualityReason[128];

    public string GetSourceId()
    {
        fixed (byte* value = SourceId)
            return Marshal.PtrToStringUTF8((IntPtr)value) ?? string.Empty;
    }

    public string GetQualityReason()
    {
        fixed (byte* value = QualityReason)
            return Marshal.PtrToStringUTF8((IntPtr)value) ?? string.Empty;
    }
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeGestureRecordingList
{
    public uint AbiVersion, StructSize, Count;
    public uint Reserved0;
    public fixed byte Items[NativeVisionInterop.MaxGestureRecordings * 248];
    public fixed byte Error[128];

    public NativeGestureRecordingMetadata GetItem(int index)
    {
        if ((uint)index >= Math.Min(Count, (uint)NativeVisionInterop.MaxGestureRecordings))
            throw new ArgumentOutOfRangeException(nameof(index));
        fixed (byte* items = Items)
            return ((NativeGestureRecordingMetadata*)items)[index];
    }

    public string GetError()
    {
        fixed (byte* error = Error)
            return Marshal.PtrToStringUTF8((IntPtr)error) ?? string.Empty;
    }
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeGesturePlaybackStatus
{
    public uint AbiVersion, StructSize, DefinitionId, TakeIndex, Playing, FrameIndex, FrameCount;
    public float NormalizedProgress;
    public double DurationMs, PositionMs;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeGestureBindingMetadata
{
    public uint DefinitionId, Enabled;
    public fixed byte ActionType[32];
    public fixed byte ActionParameter[256];
    public string GetActionType() { fixed (byte* value = ActionType) return Marshal.PtrToStringUTF8((IntPtr)value) ?? string.Empty; }
    public string GetActionParameter() { fixed (byte* value = ActionParameter) return Marshal.PtrToStringUTF8((IntPtr)value) ?? string.Empty; }
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeGestureBindingList
{
    public uint AbiVersion, StructSize, Count;
    public fixed byte Items[NativeVisionInterop.MaxGestureDefinitions * 296];
    public fixed byte Error[128];
    public NativeGestureBindingMetadata GetItem(int index)
    {
        if ((uint)index >= Math.Min(Count, (uint)NativeVisionInterop.MaxGestureDefinitions))
            throw new ArgumentOutOfRangeException(nameof(index));
        fixed (byte* items = Items) return ((NativeGestureBindingMetadata*)items)[index];
    }
    public string GetError() { fixed (byte* value = Error) return Marshal.PtrToStringUTF8((IntPtr)value) ?? string.Empty; }
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeGestureDtwDebugSnapshot
{
    public uint AbiVersion, StructSize;
    public ulong FrameId, TimestampUs;
    public uint TemplateId, CandidateDurationMs, Eligible, PathCount;
    public float Score, Confidence, ThresholdScore, WarpRatio, ReverseScore;
    public uint HasWarpRatio, HasReverseScore, Reserved0;
    public fixed float ScoreBreakdown[12];
    public fixed byte CandidateIndices[64];
    public fixed byte TemplateIndices[64];
    public fixed byte RejectionReason[128];
    public fixed uint Reserved[4];

    public string GetReason()
    {
        fixed (byte* reason = RejectionReason)
            return Marshal.PtrToStringUTF8((IntPtr)reason) ?? string.Empty;
    }
}

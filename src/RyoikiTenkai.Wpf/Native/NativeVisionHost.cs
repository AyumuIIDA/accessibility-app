using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace RyoikiTenkai.Wpf.Native;

internal sealed partial class NativeVisionHost : HwndHost
{
    private const int WsChild = 0x40000000;
    private const int WsVisible = 0x10000000;

    private IntPtr _hostHwnd;
    private IntPtr _nativeHandle;
    private bool _nativeAvailable;

    public bool NativeAvailable => _nativeAvailable;

    public bool IsStarted { get; private set; }

    public string LastError { get; private set; } = string.Empty;

    public event Action<string>? DiagnosticLogged;

    internal IntPtr NativeHandle => _nativeHandle;

    public bool StartNativeRuntime()
    {
        if (!_nativeAvailable || _nativeHandle == IntPtr.Zero)
        {
            return false;
        }

        if (IsStarted)
        {
            return true;
        }

        try
        {
            if (NativeVisionInterop.Start(_nativeHandle) == 0)
            {
                LastError = NativeVisionInterop.GetLastErrorMessage(_nativeHandle);
                DiagnosticLogged?.Invoke("Native runtime failed to start: " + LastError);
                return false;
            }

            IsStarted = true;
            DiagnosticLogged?.Invoke("Native runtime started.");
            return true;
        }
        catch (Exception ex) when (IsNativeLoadException(ex))
        {
            _nativeAvailable = false;
            LastError = ex.Message;
            DiagnosticLogged?.Invoke("Native runtime unavailable: " + ex.Message);
            return false;
        }
    }

    public void StopNativeRuntime()
    {
        if (!_nativeAvailable || _nativeHandle == IntPtr.Zero || !IsStarted)
        {
            return;
        }

        try
        {
            NativeVisionInterop.Stop(_nativeHandle);
            IsStarted = false;
            DiagnosticLogged?.Invoke("Native runtime stopped.");
        }
        catch (Exception ex) when (IsNativeLoadException(ex))
        {
            _nativeAvailable = false;
            LastError = ex.Message;
            DiagnosticLogged?.Invoke("Native runtime stop failed: " + ex.Message);
        }
    }

    public bool TryGetMetrics(out NativeVisionMetrics metrics)
    {
        metrics = default;
        var success = _nativeAvailable
            && _nativeHandle != IntPtr.Zero
            && NativeVisionInterop.GetLatestMetrics(_nativeHandle, out metrics) != 0;
        return success
            && metrics.AbiVersion == NativeVisionInterop.AbiVersion
            && metrics.StructSize == Marshal.SizeOf<NativeVisionMetrics>();
    }

    public bool TryGetHand(out NativeHandResult result)
    {
        result = default;
        var success = _nativeAvailable
            && _nativeHandle != IntPtr.Zero
            && NativeVisionInterop.GetLatestHand(_nativeHandle, out result) != 0;
        return success
            && result.AbiVersion == NativeVisionInterop.AbiVersion
            && result.StructSize == Marshal.SizeOf<NativeHandResult>();
    }

    public bool TryGetHands(out NativeHandsResult result)
    {
        result = default;
        var success = _nativeAvailable
            && _nativeHandle != IntPtr.Zero
            && NativeVisionInterop.GetLatestHands(_nativeHandle, out result) != 0;
        return success
            && result.AbiVersion == NativeVisionInterop.AbiVersion
            && result.StructSize == Marshal.SizeOf<NativeHandsResult>();
    }

    public bool TryGetHandTopology(out NativeHandTopologySnapshot snapshot)
    {
        snapshot = default;
        var success = _nativeAvailable
            && _nativeHandle != IntPtr.Zero
            && NativeVisionInterop.GetLatestHandTopology(_nativeHandle, out snapshot) != 0;
        return success
            && snapshot.AbiVersion == NativeVisionInterop.AbiVersion
            && snapshot.StructSize == Marshal.SizeOf<NativeHandTopologySnapshot>();
    }

    public bool TryGetGestureRecognition(out NativeGestureRecognitionSnapshot snapshot)
    {
        snapshot = default;
        var success = _nativeAvailable && _nativeHandle != IntPtr.Zero
            && NativeVisionInterop.GetLatestGestureRecognition(_nativeHandle, out snapshot) != 0;
        return success && snapshot.AbiVersion == NativeVisionInterop.AbiVersion
            && snapshot.StructSize == Marshal.SizeOf<NativeGestureRecognitionSnapshot>();
    }

    public bool TryGetGestureDtwDebug(out NativeGestureDtwDebugSnapshot snapshot)
    {
        snapshot = default;
        var success = _nativeAvailable && _nativeHandle != IntPtr.Zero
            && NativeVisionInterop.GetLatestGestureDtwDebug(_nativeHandle, out snapshot) != 0;
        return success && snapshot.AbiVersion == NativeVisionInterop.AbiVersion
            && snapshot.StructSize == Marshal.SizeOf<NativeGestureDtwDebugSnapshot>();
    }

    public bool RequestGestureTemplateRegistration(uint trackId, uint templateId) =>
        _nativeAvailable && _nativeHandle != IntPtr.Zero
        && NativeVisionInterop.RequestGestureTemplateRegistration(_nativeHandle, trackId, templateId) != 0;

    public bool BeginGestureRecording(uint templateId) =>
        _nativeAvailable && _nativeHandle != IntPtr.Zero
        && NativeVisionInterop.BeginGestureRecording(_nativeHandle, templateId) != 0;

    public bool FinishGestureRecording() =>
        _nativeAvailable && _nativeHandle != IntPtr.Zero
        && NativeVisionInterop.FinishGestureRecording(_nativeHandle) != 0;

    public bool CancelGestureRecording() =>
        _nativeAvailable && _nativeHandle != IntPtr.Zero
        && NativeVisionInterop.CancelGestureRecording(_nativeHandle) != 0;

    public bool TryGetGestureRecordingStatus(out NativeGestureRecordingStatus status)
    {
        status = default;
        var success = _nativeAvailable && _nativeHandle != IntPtr.Zero
            && NativeVisionInterop.GetGestureRecordingStatus(_nativeHandle, out status) != 0;
        return success && status.AbiVersion == NativeVisionInterop.AbiVersion
            && status.StructSize == Marshal.SizeOf<NativeGestureRecordingStatus>();
    }

    public bool TryListGestureDefinitions(out NativeGestureDefinitionList definitions)
    {
        definitions = default;
        if (!_nativeAvailable || _nativeHandle == IntPtr.Zero) return false;
        _ = NativeVisionInterop.ListGestureDefinitions(_nativeHandle, out definitions);
        // The native call initializes the ABI header and error text even when
        // repository loading fails, so preserve that diagnostic snapshot.
        return definitions.AbiVersion == NativeVisionInterop.AbiVersion
            && definitions.StructSize == Marshal.SizeOf<NativeGestureDefinitionList>();
    }

    public bool SetGestureDefinitionMetadata(uint definitionId, string name, bool enabled) =>
        _nativeAvailable && _nativeHandle != IntPtr.Zero
        && NativeVisionInterop.SetGestureDefinitionMetadata(
            _nativeHandle, definitionId, name, enabled ? 1U : 0U) != 0;

    public bool DeleteGestureDefinition(uint definitionId) =>
        _nativeAvailable && _nativeHandle != IntPtr.Zero
        && NativeVisionInterop.DeleteGestureDefinition(_nativeHandle, definitionId) != 0;

    public bool ReloadGestureDefinitions() =>
        _nativeAvailable && _nativeHandle != IntPtr.Zero
        && NativeVisionInterop.ReloadGestureDefinitions(_nativeHandle) != 0;

    public bool TryListGestureRecordings(uint definitionId, out NativeGestureRecordingList recordings)
    {
        recordings = default;
        if (!_nativeAvailable || _nativeHandle == IntPtr.Zero) return false;
        _ = NativeVisionInterop.ListGestureRecordings(_nativeHandle, definitionId, out recordings);
        return recordings.AbiVersion == NativeVisionInterop.AbiVersion
            && recordings.StructSize == Marshal.SizeOf<NativeGestureRecordingList>();
    }

    public bool ExportGestureRecording(uint definitionId, uint takeIndex, string path) =>
        _nativeAvailable && _nativeHandle != IntPtr.Zero
        && NativeVisionInterop.ExportGestureRecording(_nativeHandle, definitionId, takeIndex, path) != 0;

    public bool TryListGestureBindings(out NativeGestureBindingList bindings)
    {
        bindings = default;
        if (!_nativeAvailable || _nativeHandle == IntPtr.Zero) return false;
        _ = NativeVisionInterop.ListGestureBindings(_nativeHandle, out bindings);
        return bindings.AbiVersion == NativeVisionInterop.AbiVersion
            && bindings.StructSize == Marshal.SizeOf<NativeGestureBindingList>();
    }

    public bool UpsertGestureBinding(uint definitionId, string actionType,
        string actionParameter, bool enabled) =>
        _nativeAvailable && _nativeHandle != IntPtr.Zero
        && NativeVisionInterop.UpsertGestureBinding(_nativeHandle, definitionId,
            actionType, actionParameter, enabled ? 1U : 0U) != 0;

    public bool DeleteGestureBinding(uint definitionId) =>
        _nativeAvailable && _nativeHandle != IntPtr.Zero
        && NativeVisionInterop.DeleteGestureBinding(_nativeHandle, definitionId) != 0;

    public bool TryGetStates(out NativeHandStateSnapshot snapshot)
    {
        snapshot = default;
        var success = _nativeAvailable
            && _nativeHandle != IntPtr.Zero
            && NativeVisionInterop.GetLatestStates(_nativeHandle, out snapshot) != 0;
        return success
            && snapshot.AbiVersion == NativeVisionInterop.AbiVersion
            && snapshot.StructSize == Marshal.SizeOf<NativeHandStateSnapshot>();
    }

    public bool TryReadHandEvents(
        ulong afterSequence,
        out NativeHandEventBatch batch)
    {
        batch = default;
        var success = _nativeAvailable
            && _nativeHandle != IntPtr.Zero
            && NativeVisionInterop.ReadHandEvents(
                _nativeHandle,
                afterSequence,
                out batch) != 0;
        return success
            && batch.AbiVersion == NativeVisionInterop.AbiVersion
            && batch.StructSize == Marshal.SizeOf<NativeHandEventBatch>();
    }

    public bool TryGetPalm(out NativePalmResult result)
    {
        result = default;
        var success = _nativeAvailable
            && _nativeHandle != IntPtr.Zero
            && NativeVisionInterop.GetLatestPalm(_nativeHandle, out result) != 0;
        return success
            && result.AbiVersion == NativeVisionInterop.AbiVersion
            && result.StructSize == Marshal.SizeOf<NativePalmResult>();
    }

    public bool ConfigureCadHandInteraction(
        CadViewportHost cadViewport,
        NativeCadHandInteractionMode mode,
        NativeHandPresentationMode presentation,
        float rotationSensitivity)
    {
        return _nativeAvailable
            && _nativeHandle != IntPtr.Zero
            && cadViewport.NativeHandle != IntPtr.Zero
            && NativeVisionInterop.ConfigureCadHandInteraction(
                _nativeHandle,
                cadViewport.NativeHandle,
                (int)mode,
                (int)presentation,
                rotationSensitivity) != 0;
    }

    public bool SetHandPresentationMode(
        NativeHandPresentationMode presentation)
    {
        return _nativeAvailable
            && _nativeHandle != IntPtr.Zero
            && NativeVisionInterop.SetHandPresentationMode(
                _nativeHandle,
                (int)presentation) != 0;
    }

    public bool TryGetCadHandInteraction(out NativeCadHandInteractionResult result)
    {
        result = default;
        var success = _nativeAvailable
            && _nativeHandle != IntPtr.Zero
            && NativeVisionInterop.GetCadHandInteraction(
                _nativeHandle,
                out result) != 0;
        return success
            && result.AbiVersion == NativeVisionInterop.AbiVersion
            && result.StructSize == Marshal.SizeOf<NativeCadHandInteractionResult>();
    }

    public string GetLastErrorMessage()
    {
        if (!_nativeAvailable || _nativeHandle == IntPtr.Zero)
        {
            return LastError;
        }

        return NativeVisionInterop.GetLastErrorMessage(_nativeHandle);
    }

    protected override HandleRef BuildWindowCore(HandleRef hwndParent)
    {
        _hostHwnd = NativeMethods.CreateWindowEx(
            0,
            "static",
            "",
            WsChild | WsVisible,
            0,
            0,
            Math.Max(1, (int)ActualWidth),
            Math.Max(1, (int)ActualHeight),
            hwndParent.Handle,
            IntPtr.Zero,
            IntPtr.Zero,
            IntPtr.Zero);

        if (_hostHwnd == IntPtr.Zero)
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Failed to create native host window.");
        }

        TryCreateNativeRuntime();
        return new HandleRef(this, _hostHwnd);
    }

    protected override void DestroyWindowCore(HandleRef hwnd)
    {
        StopNativeRuntime();

        if (_nativeHandle != IntPtr.Zero)
        {
            try
            {
                NativeVisionInterop.Destroy(_nativeHandle);
            }
            catch (Exception ex) when (IsNativeLoadException(ex))
            {
                DiagnosticLogged?.Invoke("Native runtime destroy failed: " + ex.Message);
            }

            _nativeHandle = IntPtr.Zero;
        }

        if (hwnd.Handle != IntPtr.Zero)
        {
            NativeMethods.DestroyWindow(hwnd.Handle);
        }

        _hostHwnd = IntPtr.Zero;
    }

    protected override void OnWindowPositionChanged(Rect rcBoundingBox)
    {
        base.OnWindowPositionChanged(rcBoundingBox);
        if (_hostHwnd != IntPtr.Zero)
        {
            var width = Math.Max(1, (int)rcBoundingBox.Width);
            var height = Math.Max(1, (int)rcBoundingBox.Height);
            NativeMethods.MoveWindow(
                _hostHwnd,
                0,
                0,
                width,
                height,
                true);

            if (_nativeHandle != IntPtr.Zero)
            {
                NativeVisionInterop.Resize(_nativeHandle, width, height);
            }
        }
    }

    private void TryCreateNativeRuntime()
    {
        try
        {
            var abiVersion = NativeVisionInterop.GetAbiVersion();
            if (abiVersion != NativeVisionInterop.AbiVersion)
            {
                LastError = $"Native ABI mismatch. Expected {NativeVisionInterop.AbiVersion}, got {abiVersion}.";
                DiagnosticLogged?.Invoke(LastError);
                return;
            }

            _nativeHandle = NativeVisionInterop.Create(_hostHwnd);
            _nativeAvailable = _nativeHandle != IntPtr.Zero;
            DiagnosticLogged?.Invoke(_nativeAvailable
                ? "Native runtime loaded."
                : "Native runtime returned null handle.");
        }
        catch (Exception ex) when (IsNativeLoadException(ex))
        {
            _nativeAvailable = false;
            LastError = ex.Message;
            DiagnosticLogged?.Invoke("Native runtime unavailable: " + ex.Message);
        }
    }

    private static bool IsNativeLoadException(Exception ex)
    {
        return ex is DllNotFoundException
            or EntryPointNotFoundException
            or BadImageFormatException
            or MarshalDirectiveException;
    }

    private static partial class NativeMethods
    {
        [LibraryImport("user32.dll", EntryPoint = "CreateWindowExW", SetLastError = true, StringMarshalling = StringMarshalling.Utf16)]
        public static partial IntPtr CreateWindowEx(
            int dwExStyle,
            string lpClassName,
            string lpWindowName,
            int dwStyle,
            int x,
            int y,
            int nWidth,
            int nHeight,
            IntPtr hWndParent,
            IntPtr hMenu,
            IntPtr hInstance,
            IntPtr lpParam);

        [LibraryImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static partial bool DestroyWindow(IntPtr hWnd);

        [LibraryImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static partial bool MoveWindow(
            IntPtr hWnd,
            int x,
            int y,
            int nWidth,
            int nHeight,
            [MarshalAs(UnmanagedType.Bool)] bool repaint);
    }
}

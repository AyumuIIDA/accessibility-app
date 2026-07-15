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

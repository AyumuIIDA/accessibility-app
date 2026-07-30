using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace RyoikiTenkai.Wpf.Native;

internal sealed partial class CadViewportHost : HwndHost
{
    private const int WsChild = 0x40000000;
    private const int WsVisible = 0x10000000;
    private IntPtr _hostHwnd;
    private IntPtr _cadHandle;

    public bool IsAvailable => _cadHandle != IntPtr.Zero;
    public string InitializationError { get; private set; } = string.Empty;
    public event Action<string>? DiagnosticLogged;
    internal IntPtr NativeHandle => _cadHandle;

    public bool SetView(float yawRadians, float pitchRadians, float zoom)
    {
        return _cadHandle != IntPtr.Zero
            && NativeVisionInterop.SetCadView(_cadHandle, yawRadians, pitchRadians, zoom) != 0;
    }

    public bool SetView(
        float yawRadians,
        float pitchRadians,
        float zoom,
        float panX,
        float panY)
    {
        return _cadHandle != IntPtr.Zero
            && NativeVisionInterop.SetCadViewEx(
                _cadHandle, yawRadians, pitchRadians, zoom, panX, panY) != 0;
    }

    public bool TryGetView(out float yawRadians, out float pitchRadians, out float zoom)
    {
        yawRadians = 0;
        pitchRadians = 0;
        zoom = 1;
        return _cadHandle != IntPtr.Zero
            && NativeVisionInterop.GetCadView(_cadHandle, out yawRadians, out pitchRadians, out zoom) != 0;
    }

    public bool TryGetView(
        out float yawRadians,
        out float pitchRadians,
        out float zoom,
        out float panX,
        out float panY)
    {
        yawRadians = 0;
        pitchRadians = 0;
        zoom = 1;
        panX = 0;
        panY = 0;
        return _cadHandle != IntPtr.Zero
            && NativeVisionInterop.GetCadViewEx(
                _cadHandle,
                out yawRadians,
                out pitchRadians,
                out zoom,
                out panX,
                out panY) != 0;
    }

    public void ResetView()
    {
        if (_cadHandle != IntPtr.Zero) NativeVisionInterop.ResetCadView(_cadHandle);
    }

    protected override HandleRef BuildWindowCore(HandleRef hwndParent)
    {
        _hostHwnd = NativeMethods.CreateWindowEx(
            0, "static", "", WsChild | WsVisible, 0, 0,
            Math.Max(1, (int)ActualWidth), Math.Max(1, (int)ActualHeight),
            hwndParent.Handle, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        if (_hostHwnd == IntPtr.Zero)
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Failed to create CAD host window.");
        }

        try
        {
            if (NativeVisionInterop.GetAbiVersion() != NativeVisionInterop.AbiVersion)
            {
                DiagnosticLogged?.Invoke("CAD viewer native ABI mismatch.");
            }
            else
            {
                _cadHandle = NativeVisionInterop.CreateCad(_hostHwnd);
                if (_cadHandle == IntPtr.Zero)
                {
                    DiagnosticLogged?.Invoke("Native CAD renderer failed to initialize.");
                }
                else
                {
                    var error = NativeVisionInterop.GetCadLastErrorMessage(_cadHandle);
                    if (!string.IsNullOrWhiteSpace(error))
                    {
                        InitializationError = error;
                        DiagnosticLogged?.Invoke(error);
                    }
                }
            }
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
        {
            DiagnosticLogged?.Invoke("Native CAD renderer unavailable: " + ex.Message);
        }
        return new HandleRef(this, _hostHwnd);
    }

    protected override void DestroyWindowCore(HandleRef hwnd)
    {
        if (_cadHandle != IntPtr.Zero)
        {
            NativeVisionInterop.DestroyCad(_cadHandle);
            _cadHandle = IntPtr.Zero;
        }
        if (hwnd.Handle != IntPtr.Zero) NativeMethods.DestroyWindow(hwnd.Handle);
        _hostHwnd = IntPtr.Zero;
    }

    protected override void OnWindowPositionChanged(Rect rcBoundingBox)
    {
        base.OnWindowPositionChanged(rcBoundingBox);
        if (_hostHwnd == IntPtr.Zero) return;
        var width = Math.Max(1, (int)rcBoundingBox.Width);
        var height = Math.Max(1, (int)rcBoundingBox.Height);
        NativeMethods.MoveWindow(_hostHwnd, 0, 0, width, height, true);
        if (_cadHandle != IntPtr.Zero) NativeVisionInterop.ResizeCad(_cadHandle, width, height);
    }

    private static partial class NativeMethods
    {
        [LibraryImport("user32.dll", EntryPoint = "CreateWindowExW", SetLastError = true, StringMarshalling = StringMarshalling.Utf16)]
        public static partial IntPtr CreateWindowEx(int exStyle, string className, string windowName,
            int style, int x, int y, int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr parameter);

        [LibraryImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static partial bool DestroyWindow(IntPtr hwnd);

        [LibraryImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static partial bool MoveWindow(IntPtr hwnd, int x, int y, int width, int height,
            [MarshalAs(UnmanagedType.Bool)] bool repaint);
    }
}

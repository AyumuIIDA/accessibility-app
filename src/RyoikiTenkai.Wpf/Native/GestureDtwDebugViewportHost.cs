using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace RyoikiTenkai.Wpf.Native;

internal sealed partial class GestureDtwDebugViewportHost : HwndHost
{
    private const int WsChild = 0x40000000;
    private const int WsVisible = 0x10000000;

    private IntPtr _hostHwnd;
    private IntPtr _debugHandle;
    private NativeVisionHost? _source;

    public string InitializationError { get; private set; } = string.Empty;
    public event Action<string>? DiagnosticLogged;

    public void Attach(NativeVisionHost source)
    {
        ArgumentNullException.ThrowIfNull(source);
        if (_source is not null && !ReferenceEquals(_source, source))
            throw new InvalidOperationException("The DTW viewport is already attached to another runtime.");
        _source = source;
        TryCreateRenderer();
    }

    protected override HandleRef BuildWindowCore(HandleRef hwndParent)
    {
        _hostHwnd = NativeMethods.CreateWindowEx(
            0, "static", "", WsChild | WsVisible, 0, 0,
            Math.Max(1, (int)ActualWidth), Math.Max(1, (int)ActualHeight),
            hwndParent.Handle, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        if (_hostHwnd == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Failed to create DTW debug host window.");
        TryCreateRenderer();
        return new HandleRef(this, _hostHwnd);
    }

    private void TryCreateRenderer()
    {
        if (_debugHandle != IntPtr.Zero || _hostHwnd == IntPtr.Zero
            || _source is null || _source.NativeHandle == IntPtr.Zero) return;
        try
        {
            if (NativeVisionInterop.GetAbiVersion() != NativeVisionInterop.AbiVersion)
            {
                InitializationError = "DTW debugger native ABI mismatch.";
            }
            else
            {
                _debugHandle = NativeVisionInterop.CreateDtwDebug(_hostHwnd, _source.NativeHandle);
                if (_debugHandle == IntPtr.Zero)
                    InitializationError = "Native DTW debug renderer failed to initialize.";
                else
                    InitializationError = NativeVisionInterop.GetDtwDebugLastErrorMessage(_debugHandle);
            }
        }
        catch (Exception exception) when (exception is DllNotFoundException
            or EntryPointNotFoundException or BadImageFormatException)
        {
            InitializationError = "Native DTW debug renderer unavailable: " + exception.Message;
        }
        if (!string.IsNullOrWhiteSpace(InitializationError))
            DiagnosticLogged?.Invoke(InitializationError);
    }

    protected override void DestroyWindowCore(HandleRef hwnd)
    {
        if (_debugHandle != IntPtr.Zero)
        {
            NativeVisionInterop.DestroyDtwDebug(_debugHandle);
            _debugHandle = IntPtr.Zero;
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
        if (_debugHandle != IntPtr.Zero)
            NativeVisionInterop.ResizeDtwDebug(_debugHandle, width, height);
    }

    private static partial class NativeMethods
    {
        [LibraryImport("user32.dll", EntryPoint = "CreateWindowExW", SetLastError = true,
            StringMarshalling = StringMarshalling.Utf16)]
        public static partial IntPtr CreateWindowEx(int exStyle, string className, string windowName,
            int style, int x, int y, int width, int height, IntPtr parent, IntPtr menu,
            IntPtr instance, IntPtr parameter);

        [LibraryImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static partial bool DestroyWindow(IntPtr hwnd);

        [LibraryImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static partial bool MoveWindow(IntPtr hwnd, int x, int y, int width, int height,
            [MarshalAs(UnmanagedType.Bool)] bool repaint);
    }
}

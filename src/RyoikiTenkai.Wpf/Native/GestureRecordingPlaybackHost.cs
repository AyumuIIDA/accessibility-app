using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace RyoikiTenkai.Wpf.Native;

internal sealed partial class GestureRecordingPlaybackHost : HwndHost
{
    private const int WsChild = 0x40000000;
    private const int WsVisible = 0x10000000;
    private IntPtr _hostHwnd;
    private IntPtr _playbackHandle;
    private NativeVisionHost? _source;

    public string InitializationError { get; private set; } = string.Empty;
    public event Action<string>? DiagnosticLogged;

    public void Attach(NativeVisionHost source)
    {
        ArgumentNullException.ThrowIfNull(source);
        _source = source;
        TryCreateRenderer();
    }

    public bool Select(uint definitionId, uint takeIndex) => _playbackHandle != IntPtr.Zero
        && NativeVisionInterop.SelectRecordingPlayback(_playbackHandle, definitionId, takeIndex) != 0;
    public bool SetPlaying(bool playing) => _playbackHandle != IntPtr.Zero
        && NativeVisionInterop.SetRecordingPlaybackPlaying(_playbackHandle, playing ? 1U : 0U) != 0;
    public bool Seek(float progress) => _playbackHandle != IntPtr.Zero
        && NativeVisionInterop.SeekRecordingPlayback(_playbackHandle, Math.Clamp(progress, 0.0F, 1.0F)) != 0;
    public bool TryGetStatus(out NativeGesturePlaybackStatus status)
    {
        status = default;
        return _playbackHandle != IntPtr.Zero
            && NativeVisionInterop.GetRecordingPlaybackStatus(_playbackHandle, out status) != 0
            && status.AbiVersion == NativeVisionInterop.AbiVersion
            && status.StructSize == Marshal.SizeOf<NativeGesturePlaybackStatus>();
    }

    protected override HandleRef BuildWindowCore(HandleRef hwndParent)
    {
        _hostHwnd = NativeMethods.CreateWindowEx(0, "static", "", WsChild | WsVisible, 0, 0,
            Math.Max(1, (int)ActualWidth), Math.Max(1, (int)ActualHeight), hwndParent.Handle,
            IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        if (_hostHwnd == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Failed to create recording playback host window.");
        TryCreateRenderer();
        return new HandleRef(this, _hostHwnd);
    }

    private void TryCreateRenderer()
    {
        if (_playbackHandle != IntPtr.Zero || _hostHwnd == IntPtr.Zero
            || _source is null || _source.NativeHandle == IntPtr.Zero) return;
        try
        {
            _playbackHandle = NativeVisionInterop.CreateRecordingPlayback(_hostHwnd, _source.NativeHandle);
            if (_playbackHandle == IntPtr.Zero)
                InitializationError = "Native recording playback renderer failed to initialize.";
            else
                InitializationError = NativeVisionInterop.GetRecordingPlaybackLastErrorMessage(_playbackHandle);
        }
        catch (Exception exception) when (exception is DllNotFoundException
            or EntryPointNotFoundException or BadImageFormatException)
        {
            InitializationError = "Native recording playback is unavailable: " + exception.Message;
        }
        if (!string.IsNullOrWhiteSpace(InitializationError)) DiagnosticLogged?.Invoke(InitializationError);
    }

    protected override void DestroyWindowCore(HandleRef hwnd)
    {
        if (_playbackHandle != IntPtr.Zero)
        {
            NativeVisionInterop.DestroyRecordingPlayback(_playbackHandle);
            _playbackHandle = IntPtr.Zero;
        }
        if (hwnd.Handle != IntPtr.Zero) NativeMethods.DestroyWindow(hwnd.Handle);
        _hostHwnd = IntPtr.Zero;
    }

    protected override void OnWindowPositionChanged(Rect bounds)
    {
        base.OnWindowPositionChanged(bounds);
        if (_hostHwnd == IntPtr.Zero) return;
        var width = Math.Max(1, (int)bounds.Width);
        var height = Math.Max(1, (int)bounds.Height);
        NativeMethods.MoveWindow(_hostHwnd, 0, 0, width, height, true);
        if (_playbackHandle != IntPtr.Zero)
            NativeVisionInterop.ResizeRecordingPlayback(_playbackHandle, width, height);
    }

    private static partial class NativeMethods
    {
        [LibraryImport("user32.dll", EntryPoint = "CreateWindowExW", SetLastError = true,
            StringMarshalling = StringMarshalling.Utf16)]
        public static partial IntPtr CreateWindowEx(int exStyle, string className, string windowName,
            int style, int x, int y, int width, int height, IntPtr parent, IntPtr menu,
            IntPtr instance, IntPtr parameter);
        [LibraryImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static partial bool DestroyWindow(IntPtr hwnd);
        [LibraryImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static partial bool MoveWindow(IntPtr hwnd, int x, int y, int width, int height,
            [MarshalAs(UnmanagedType.Bool)] bool repaint);
    }
}

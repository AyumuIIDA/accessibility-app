using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using RyoikiTenkai.Core;
using RyoikiTenkai.Storage;
using RyoikiTenkai.Wpf.Native;

namespace RyoikiTenkai.Wpf;

public partial class MainWindow : Window
{
    private readonly BindingStore _store;
    private readonly string _logPath;
    private readonly string? _nativeMetricsPath;
    private readonly DispatcherTimer _nativePollTimer;
    private string? _lastNativeRuntimeError;
    private ulong _lastSampledNativeFrameId;
    private ulong _lastHandEventSequence;
    private CadViewerWindow? _cadViewerWindow;

    public MainWindow()
    {
        InitializeComponent();
        _store = new BindingStore(System.IO.Path.Combine(AppContext.BaseDirectory, "bindings.json"));
        _logPath = System.IO.Path.Combine(AppContext.BaseDirectory, "ryoikitenkai.log");
        var metricsPath = Environment.GetEnvironmentVariable("RYOIKI_NATIVE_METRICS_CSV");
        _nativeMetricsPath = string.IsNullOrWhiteSpace(metricsPath)
            ? null
            : System.IO.Path.GetFullPath(metricsPath);
        NativeVisionHostControl.DiagnosticLogged += Log;
        _nativePollTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(250) };
        _nativePollTimer.Tick += NativePollTimer_Tick;
        EnsureSeedData();
        RefreshBindings();
        Log($"Ready. Native runtime is required. Log file: {_logPath}");
    }

    private void StartButton_Click(object sender, RoutedEventArgs e)
    {
        StartButton.IsEnabled = false;
        StopButton.IsEnabled = true;
        StateText.Text = "Starting native camera and models...";
        OverlayStatusText.Text = "Starting native runtime";
        NativeVisionHostControl.Visibility = Visibility.Visible;
        if (!NativeVisionHostControl.StartNativeRuntime())
        {
            NativeVisionHostControl.Visibility = Visibility.Collapsed;
            StateText.Text = "Native runtime unavailable";
            OverlayStatusText.Text = "Native startup failed";
            StartButton.IsEnabled = true;
            StopButton.IsEnabled = false;
            Log("Native runtime is required; no managed inference fallback exists.");
            return;
        }
        _lastNativeRuntimeError = null;
        _lastSampledNativeFrameId = 0;
        _lastHandEventSequence = 0;
        InitializeNativeMetricsFile();
        _nativePollTimer.Start();
        StateText.Text = "Running native runtime";
        OverlayStatusText.Text = "Native runtime";
        Log("Native-only camera, inference, landmark processing, and rendering started.");
    }

    private void StopButton_Click(object sender, RoutedEventArgs e)
    {
        StopNativeRuntime();
        StateText.Text = "Stopped";
        OverlayStatusText.Text = "Stopped";
        StartButton.IsEnabled = true;
        StopButton.IsEnabled = false;
    }

    private void OpenCadViewerButton_Click(object sender, RoutedEventArgs e)
    {
        if (_cadViewerWindow is { IsVisible: true })
        {
            _cadViewerWindow.Activate();
            return;
        }
        try
        {
            _cadViewerWindow = new CadViewerWindow(NativeVisionHostControl) { Owner = this };
            _cadViewerWindow.Closed += (_, _) => _cadViewerWindow = null;
            _cadViewerWindow.Show();
            Log("3D viewer opened.");
        }
        catch (Exception exception)
        {
            _cadViewerWindow = null;
            Log("3D viewer failed to open: " + exception);
        }
    }

    private void StopNativeRuntime()
    {
        _nativePollTimer.Stop();
        NativeVisionHostControl.StopNativeRuntime();
        NativeVisionHostControl.Visibility = Visibility.Collapsed;
    }

    private void NativePollTimer_Tick(object? sender, EventArgs e)
    {
        if (!NativeVisionHostControl.IsStarted
            || !NativeVisionHostControl.TryGetMetrics(out var metrics))
        {
            return;
        }
        var nativeError = NativeVisionHostControl.GetLastErrorMessage();
        if (!string.IsNullOrWhiteSpace(nativeError)
            && !StringComparer.Ordinal.Equals(nativeError, _lastNativeRuntimeError))
        {
            _lastNativeRuntimeError = nativeError;
            Log("Native runtime error: " + nativeError);
        }
        if (metrics.FrameId == 0)
        {
            if (!string.IsNullOrWhiteSpace(nativeError))
            {
                StateText.Text = "Native camera error";
                OverlayStatusText.Text = "Native camera unavailable";
            }
            return;
        }

        GestureText.Text = "none";
        ConfidenceText.Text = "-";
        if (NativeVisionHostControl.TryGetPalm(out var palm) && palm.FrameId > 0)
        {
            GestureText.Text = palm.PalmCount > 0 ? "palm" : "none";
            ConfidenceText.Text = palm.PalmCount > 0 ? palm.Confidence.ToString("0.000") : "-";
        }
        if (NativeVisionHostControl.TryGetHand(out var hand)
            && hand.FrameId > 0 && hand.HandCount > 0)
        {
            GestureText.Text = "hand";
            ConfidenceText.Text = hand.Confidence.ToString("0.000");
        }
        PollHandEvents();
        LatencyText.Text = $"{metrics.EndToEndLatencyMs:0.0} ms";
        StateText.Text = $"Native frame {metrics.FrameId}";
        OverlayStatusText.Text =
            $"Native  camera {metrics.CameraFps:0.0}  display {metrics.DisplayFps:0.0} fps  "
            + $"copy {metrics.FrameCopyMs:0.0}  upload {metrics.CameraUploadMs:0.0}  "
            + $"present {metrics.PresentWaitMs:0.0} ms  "
            + $"gpu {metrics.GpuRenderedFrames}/{metrics.GpuCameraFrames}  "
            + $"drops {metrics.FramePoolDroppedFrames}/{metrics.PerceptionDroppedFrames}";
        AppendNativeMetrics(metrics);
    }

    private void PollHandEvents()
    {
        if (!NativeVisionHostControl.TryReadHandEvents(
                _lastHandEventSequence,
                out var batch))
        {
            return;
        }
        if (batch.DroppedCount > 0)
        {
            Log($"Hand event overflow: dropped {batch.DroppedCount} event(s).");
        }
        var count = Math.Min(batch.Count, 16U);
        for (var index = 0; index < count; ++index)
        {
            var handEvent = batch.Events[index];
            var label = handEvent.Id switch
            {
                NativeVisionInterop.SwipeLeftEventId => "swipe_left",
                NativeVisionInterop.SwipeRightEventId => "swipe_right",
                _ => $"event_{handEvent.Id}"
            };
            GestureText.Text = label;
            ConfidenceText.Text = handEvent.Confidence.ToString("0.000");
            Log(
                $"{label} event #{handEvent.Sequence}: "
                + $"dx={handEvent.DisplacementX:0.000}, "
                + $"dy={handEvent.DisplacementY:0.000}, "
                + $"duration={handEvent.DurationUs / 1000.0:0} ms");
            _lastHandEventSequence = handEvent.Sequence;
        }
        if (count == 0 && batch.NextSequence > _lastHandEventSequence)
        {
            _lastHandEventSequence = batch.NextSequence;
        }
    }

    private void InitializeNativeMetricsFile()
    {
        if (_nativeMetricsPath is null) return;
        try
        {
            var directory = System.IO.Path.GetDirectoryName(_nativeMetricsPath);
            if (!string.IsNullOrEmpty(directory)) System.IO.Directory.CreateDirectory(directory);
            System.IO.File.WriteAllText(
                _nativeMetricsPath,
                "sample_time,frame_id,camera_fps,display_fps,perception_fps,camera_wait_ms,"
                + "frame_copy_ms,preprocess_ms,palm_inference_ms,palm_postprocess_ms,"
                + "roi_crop_warp_ms,hand_inference_ms,landmark_postprocess_ms,tracking_update_ms,"
                + "camera_upload_ms,camera_draw_ms,overlay_draw_ms,hand_3d_draw_ms,end_draw_ms,"
                + "present_wait_ms,render_total_ms,end_to_end_ms,frame_pool_drops,"
                + "perception_drops,gpu_camera_frames,gpu_rendered_frames,gpu_dxgi_format,"
                + "gpu_subresource" + Environment.NewLine);
        }
        catch (Exception exception)
        {
            Log("Native metrics file initialization failed: " + exception.Message);
        }
    }

    private void AppendNativeMetrics(NativeVisionMetrics metrics)
    {
        if (_nativeMetricsPath is null || metrics.FrameId == _lastSampledNativeFrameId) return;
        _lastSampledNativeFrameId = metrics.FrameId;
        object[] values =
        [
            DateTimeOffset.Now.ToString("O", CultureInfo.InvariantCulture),
            metrics.FrameId, metrics.CameraFps, metrics.DisplayFps, metrics.PerceptionFps,
            metrics.CameraWaitMs, metrics.FrameCopyMs, metrics.PreprocessMs,
            metrics.PalmInferenceMs, metrics.PalmPostprocessMs, metrics.RoiCropWarpMs,
            metrics.HandInferenceMs, metrics.LandmarkPostprocessMs, metrics.TrackingUpdateMs,
            metrics.CameraUploadMs, metrics.CameraDrawMs, metrics.OverlayDrawMs,
            metrics.Hand3dDrawMs, metrics.EndDrawMs, metrics.PresentWaitMs,
            metrics.OverlayRenderMs, metrics.EndToEndLatencyMs,
            metrics.FramePoolDroppedFrames, metrics.PerceptionDroppedFrames,
            metrics.GpuCameraFrames, metrics.GpuRenderedFrames,
            metrics.GpuCameraDxgiFormat, metrics.GpuCameraSubresource
        ];
        try
        {
            System.IO.File.AppendAllText(
                _nativeMetricsPath,
                string.Join(',', values.Select(static value =>
                    Convert.ToString(value, CultureInfo.InvariantCulture))) + Environment.NewLine);
        }
        catch (Exception exception)
        {
            Log("Native metrics sample failed: " + exception.Message);
        }
    }

    private void SaveBindingButton_Click(object sender, RoutedEventArgs e)
    {
        var gestureId = ((ComboBoxItem)GestureCombo.SelectedItem).Content.ToString()!;
        var actionItem = (ComboBoxItem)ActionTypeCombo.SelectedItem;
        var actionType = actionItem.Tag.ToString()!;
        var value = ActionValueText.Text.Trim();
        if (string.IsNullOrWhiteSpace(value))
        {
            Log("Action value is empty.");
            return;
        }
        var parameterName = actionType switch
        {
            "app.launch" => "path",
            "keyboard.hotkey" => "hotkey",
            "keyboard.typeText" => "text",
            _ => "value"
        };
        var binding = new GestureBinding(
            gestureId,
            $"{gestureId} -> {actionItem.Content}",
            new ActionSpec(actionType, new Dictionary<string, string> { [parameterName] = value }));
        var bindings = _store.Load();
        bindings.RemoveAll(item =>
            StringComparer.OrdinalIgnoreCase.Equals(item.GestureId, gestureId));
        bindings.Add(binding);
        _store.Save(bindings);
        RefreshBindings();
        Log($"Saved binding: {binding.DisplayName}");
    }

    private void ResetSampleButton_Click(object sender, RoutedEventArgs e)
    {
        _store.Save([]);
        EnsureSeedData();
        RefreshBindings();
        Log("Sample binding restored.");
    }

    private void ActionTypeCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (ActionValueLabel is null || ActionValueText is null
            || ActionTypeCombo.SelectedItem is not ComboBoxItem item)
        {
            return;
        }
        var actionType = item.Tag?.ToString();
        ActionValueLabel.Text = actionType switch
        {
            "app.launch" => "Path",
            "keyboard.hotkey" => "Hotkey",
            "keyboard.typeText" => "Text",
            _ => "Value"
        };
        ActionValueText.Text = actionType switch
        {
            "app.launch" => "notepad.exe",
            "keyboard.hotkey" => "ctrl+s",
            "keyboard.typeText" => "Hello from RyoikiTenkai",
            _ => ""
        };
    }

    private void EnsureSeedData()
    {
        if (_store.Load().Count > 0) return;
        _store.Save([
            new GestureBinding(
                "open_palm",
                "Open palm -> launch Notepad",
                new ActionSpec(
                    "app.launch",
                    new Dictionary<string, string> { ["path"] = "notepad.exe" }))
        ]);
    }

    private void RefreshBindings()
    {
        BindingsList.Items.Clear();
        foreach (var binding in _store.Load())
        {
            var value = string.Join(", ", binding.Action.Params.Select(
                item => $"{item.Key}={item.Value}"));
            BindingsList.Items.Add(
                $"{binding.GestureId}: {binding.Action.Type} ({value})");
        }
    }

    private void Log(string message)
    {
        var line = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}  {message}";
        try { System.IO.File.AppendAllText(_logPath, line + Environment.NewLine); }
        catch { }
        if (!Dispatcher.CheckAccess())
        {
            Dispatcher.BeginInvoke(() => LogToUi(line));
            return;
        }
        LogToUi(line);
    }

    private void LogToUi(string line)
    {
        ActionLogList.Items.Insert(0, line);
        while (ActionLogList.Items.Count > 80)
        {
            ActionLogList.Items.RemoveAt(ActionLogList.Items.Count - 1);
        }
    }

    protected override void OnClosed(EventArgs e)
    {
        StopNativeRuntime();
        base.OnClosed(e);
    }
}

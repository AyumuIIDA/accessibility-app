using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using System.Windows.Threading;
using RyoikiTenkai.Actions;
using RyoikiTenkai.Core;
using RyoikiTenkai.Storage;
using RyoikiTenkai.Vision;
using RyoikiTenkai.Wpf.Native;

namespace RyoikiTenkai.Wpf;

public partial class MainWindow : Window
{
    private const int RequiredTemplateCount = 3;
    private static readonly TimeSpan RecordingLeadInDuration = TimeSpan.FromMilliseconds(900);
    private static readonly TimeSpan RecordingDuration = TimeSpan.FromMilliseconds(1500);
    private static readonly string[] BuiltInGestureIds = ["open_palm", "fist", "pinch", "peace"];
    private static readonly OverlayStyle TrackingOverlay = new(
        Line: new SolidColorBrush(Color.FromRgb(150, 168, 190)),
        PointFill: Brushes.White,
        Box: new SolidColorBrush(Color.FromArgb(160, 150, 168, 190)));
    private static readonly OverlayStyle RecordingOverlay = new(
        Line: Brushes.DeepSkyBlue,
        PointFill: Brushes.White,
        Box: new SolidColorBrush(Color.FromArgb(190, 0, 191, 255)));
    private static readonly OverlayStyle GestureOnlyOverlay = new(
        Line: Brushes.Gold,
        PointFill: Brushes.White,
        Box: new SolidColorBrush(Color.FromArgb(190, 255, 215, 0)));
    private static readonly OverlayStyle ActionReadyOverlay = new(
        Line: Brushes.Lime,
        PointFill: Brushes.White,
        Box: new SolidColorBrush(Color.FromArgb(190, 0, 255, 90)));
    private static readonly (int Start, int End)[] HandConnections =
    [
        (0, 1), (1, 2), (2, 3), (3, 4),
        (0, 5), (5, 6), (6, 7), (7, 8),
        (5, 9), (9, 10), (10, 11), (11, 12),
        (9, 13), (13, 14), (14, 15), (15, 16),
        (13, 17), (17, 18), (18, 19), (19, 20),
        (0, 17)
    ];

    private readonly BindingStore _store;
    private readonly GestureDefinitionStore _gestureStore;
    private readonly ActionExecutor _executor = new();
    private readonly WindowGestureRecognizer _recognizer = new();
    private readonly string _modelDirectory;
    private readonly string _logPath;

    private List<GestureDefinition> _gestureDefinitions = [];
    private CancellationTokenSource? _cameraLoopCts;
    private RealtimeCameraFrameSource? _camera;
    private MediaPipeHandsModel? _model;
    private WriteableBitmap? _bitmap;
    private bool _isExecuting;
    private bool _isInferenceRunning;
    private readonly DispatcherTimer _nativePollTimer;
    private string? _lastNativeRuntimeError;
    private string? _lastNativeProviderSummary;
    private string? _lastNativeProviderFallbackReason;
    private DateTimeOffset _lastNativePerfLogAt;
    private bool _isRecordingGesture;
    private bool _isCapturingGesture;
    private DateTimeOffset _recordingStartedAt;
    private DateTimeOffset _recordingCaptureStartedAt;
    private string? _recordingGestureName;
    private readonly List<GestureFrameSample> _recordingSamples = [];

    public MainWindow()
    {
        InitializeComponent();

        _store = new BindingStore(System.IO.Path.Combine(AppContext.BaseDirectory, "bindings.json"));
        _gestureStore = new GestureDefinitionStore(System.IO.Path.Combine(AppContext.BaseDirectory, "gestures.json"));
        _modelDirectory = System.IO.Path.Combine(AppContext.BaseDirectory, "models");
        _logPath = System.IO.Path.Combine(AppContext.BaseDirectory, "ryoikitenkai.log");
        NativeVisionHostControl.DiagnosticLogged += Log;
        _nativePollTimer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(250)
        };
        _nativePollTimer.Tick += NativePollTimer_Tick;
        _recognizer.SetBuiltInGesturesEnabled(false);
        RemoveLegacySampleBinding();
        RefreshGestures();
        RefreshBindings();
        Log($"Ready. Log file: {_logPath}");
    }

    private async void StartButton_Click(object sender, RoutedEventArgs e)
    {
        StartButton.IsEnabled = false;
        StopButton.IsEnabled = true;
        StateText.Text = "Starting camera and model...";
        OverlayStatusText.Text = "Starting";

        if (UseNativeRuntimeCheckBox.IsChecked == true)
        {
            if (StartNativeRuntime())
            {
                return;
            }

            Log("Native runtime is required for NPU mode; managed CPU fallback was not started.");
            StateText.Text = "Native runtime unavailable";
            OverlayStatusText.Text = "NPU unavailable";
            RuntimeProviderText.Text = "Native blocked";
            StartButton.IsEnabled = true;
            StopButton.IsEnabled = false;
            return;
        }

        Log("Managed CPU camera/inference path selected intentionally; this path is slower and does not use the NPU.");
        RuntimeProviderText.Text = "Managed CPU";

        _cameraLoopCts = new CancellationTokenSource();
        try
        {
            _model = new MediaPipeHandsModel(new MediaPipeHandsModelOptions(
                PalmDetectorPath: System.IO.Path.Combine(_modelDirectory, "palm_detection.onnx"),
                HandLandmarkPath: System.IO.Path.Combine(_modelDirectory, "hand_landmark.onnx")));

            _camera = new RealtimeCameraFrameSource();
            _camera.DiagnosticLogged += Log;
            await _camera.InitializeAsync();
            StateText.Text = "Running";
            OverlayStatusText.Text = "Running";
            await RunCameraLoopAsync(_cameraLoopCts.Token);
        }
        catch (OperationCanceledException)
        {
            StateText.Text = "Stopped";
            OverlayStatusText.Text = "Stopped";
        }
        catch (Exception ex)
        {
            Log("Error: " + ex);
            StateText.Text = "Error";
            OverlayStatusText.Text = "Error";
        }
        finally
        {
            await DisposeRuntimeAsync();
            StartButton.IsEnabled = true;
            StopButton.IsEnabled = false;
        }
    }

    private void StopButton_Click(object sender, RoutedEventArgs e)
    {
        var wasNativeStarted = NativeVisionHostControl.IsStarted;
        StopNativeRuntime();
        if (wasNativeStarted)
        {
            StateText.Text = "Stopped";
            OverlayStatusText.Text = "Stopped";
            StartButton.IsEnabled = true;
            StopButton.IsEnabled = false;
        }

        _cameraLoopCts?.Cancel();
    }

    private bool StartNativeRuntime()
    {
        CameraImage.Visibility = Visibility.Collapsed;
        OverlayCanvas.Visibility = Visibility.Collapsed;
        NativeVisionHostControl.Visibility = Visibility.Visible;

        if (!NativeVisionHostControl.StartNativeRuntime())
        {
            NativeVisionHostControl.Visibility = Visibility.Collapsed;
            CameraImage.Visibility = Visibility.Visible;
            OverlayCanvas.Visibility = Visibility.Visible;
            return false;
        }

        StateText.Text = "Running native runtime";
        OverlayStatusText.Text = "Native runtime";
        RuntimeProviderText.Text = "Native starting";
        _lastNativeRuntimeError = null;
        _lastNativeProviderSummary = null;
        _lastNativeProviderFallbackReason = null;
        _lastNativePerfLogAt = DateTimeOffset.MinValue;
        _nativePollTimer.Start();
        Log("Using native runtime path.");
        return true;
    }

    private void StopNativeRuntime()
    {
        _nativePollTimer.Stop();
        NativeVisionHostControl.StopNativeRuntime();
        NativeVisionHostControl.Visibility = Visibility.Collapsed;
        CameraImage.Visibility = Visibility.Visible;
        OverlayCanvas.Visibility = Visibility.Visible;
    }

    private void NativePollTimer_Tick(object? sender, EventArgs e)
    {
        if (!NativeVisionHostControl.NativeAvailable || !NativeVisionHostControl.IsStarted)
        {
            return;
        }

        if (NativeVisionHostControl.TryGetMetrics(out var metrics))
        {
            var nativeError = NativeVisionHostControl.GetLastErrorMessage();
            if (!string.IsNullOrWhiteSpace(nativeError)
                && !StringComparer.Ordinal.Equals(nativeError, _lastNativeRuntimeError))
            {
                _lastNativeRuntimeError = nativeError;
                Log("Native runtime error: " + nativeError);
            }
            var providerSummary = FormatNativeProviderSummary(metrics);
            if (!string.IsNullOrWhiteSpace(providerSummary)
                && !StringComparer.Ordinal.Equals(providerSummary, _lastNativeProviderSummary))
            {
                _lastNativeProviderSummary = providerSummary;
                Log("Native inference providers: " + providerSummary);
            }

            var fallbackReason = metrics.ProviderFallbackReason;
            if (!string.IsNullOrWhiteSpace(fallbackReason)
                && !StringComparer.Ordinal.Equals(fallbackReason, _lastNativeProviderFallbackReason))
            {
                _lastNativeProviderFallbackReason = fallbackReason;
                Log("Native provider fallback: " + fallbackReason);
            }
            if (!string.IsNullOrWhiteSpace(fallbackReason))
            {
                StateText.Text = "NPU unavailable";
                OverlayStatusText.Text = "NPU required";
                RuntimeProviderText.Text = "NPU blocked";
                return;
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

            GestureText.Text = "native";
            ConfidenceText.Text = "-";
            if (NativeVisionHostControl.TryGetPalm(out var palm)
                && palm.FrameId > 0)
            {
                GestureText.Text = palm.PalmCount > 0 ? "palm" : "none";
                ConfidenceText.Text = palm.PalmCount > 0
                    ? palm.Confidence.ToString("0.000")
                    : "-";
            }
            if (NativeVisionHostControl.TryGetHand(out var hand)
                && hand.FrameId > 0
                && hand.HandCount > 0)
            {
                GestureText.Text = "hand";
                ConfidenceText.Text = hand.Confidence.ToString("0.000");
            }
            LatencyText.Text = $"{metrics.EndToEndLatencyMs:0.0} ms";
            StateText.Text = $"Native frame {metrics.FrameId}";
            RuntimeProviderText.Text = metrics.PalmProvider.Contains("QNN", StringComparison.OrdinalIgnoreCase)
                || metrics.HandProvider.Contains("QNN", StringComparison.OrdinalIgnoreCase)
                ? "QNN/HTP NPU"
                : "Native";
            var workingSetMb = Process.GetCurrentProcess().WorkingSet64 / (1024.0 * 1024.0);
            var frameMb = metrics.FrameBytes / (1024.0 * 1024.0);
            var tensorMb = metrics.TensorInputBytes / (1024.0 * 1024.0);
            OverlayStatusText.Text =
                $"Native {metrics.CaptureWidth}x{metrics.CaptureHeight} {metrics.CameraSubtype}  " +
                $"cam {metrics.CameraFps:0.0} disp {metrics.DisplayFps:0.0} perc {metrics.PerceptionFps:0.0} fps  " +
                $"copy {metrics.FrameCopyMs:0.0} graph {metrics.GraphTotalMs:0.0} age {metrics.PerceptionFrameAgeMs:0.0} ms  " +
                $"drops {metrics.FramePoolDroppedFrames}/{metrics.PerceptionDroppedFrames}  frame {frameMb:0.0} MB tensor {tensorMb:0.0} MB  " +
                $"mem {workingSetMb:0} MB";
            LogNativePerformanceSample(metrics, providerSummary, workingSetMb);
        }
    }

    private void LogNativePerformanceSample(
        NativeVisionMetrics metrics,
        string providerSummary,
        double workingSetMb)
    {
        var now = DateTimeOffset.UtcNow;
        if (now - _lastNativePerfLogAt < TimeSpan.FromSeconds(5))
        {
            return;
        }

        _lastNativePerfLogAt = now;
        Log(
            $"Native perf: frame={metrics.FrameId} {metrics.CaptureWidth}x{metrics.CaptureHeight} {metrics.CameraSubtype} " +
            $"providers=[{providerSummary}] fps cam={metrics.CameraFps:0.0} perc={metrics.PerceptionFps:0.0} display={metrics.DisplayFps:0.0} " +
            $"ms wait={metrics.CameraWaitMs:0.0} copy={metrics.FrameCopyMs:0.0} palm-pre={metrics.PreprocessMs:0.0} " +
            $"palm={metrics.PalmInferenceMs:0.0} roi={metrics.RoiCropWarpMs:0.0} hand={metrics.HandInferenceMs:0.0} " +
            $"graph={metrics.GraphTotalMs:0.0} age={metrics.PerceptionFrameAgeMs:0.0} " +
            $"drops={metrics.FramePoolDroppedFrames}/{metrics.PerceptionDroppedFrames} mem={workingSetMb:0}MB");
    }

    private static string FormatNativeProviderSummary(NativeVisionMetrics metrics)
    {
        var palmProvider = string.IsNullOrWhiteSpace(metrics.PalmProvider)
            ? "palm=?"
            : $"palm={metrics.PalmProvider}";
        var handProvider = string.IsNullOrWhiteSpace(metrics.HandProvider)
            ? "hand=?"
            : $"hand={metrics.HandProvider}";
        return $"{palmProvider}, {handProvider}";
    }

    private async Task RunCameraLoopAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            if (_camera is null || _model is null)
            {
                return;
            }

            var displayFrame = await _camera.WaitForFrameAsync(cancellationToken);
            ShowFrame(displayFrame);

            if (!_isInferenceRunning)
            {
                var modelFrame = CameraFrameResizer.ResizeToWidth(displayFrame, 640);
                _isInferenceRunning = true;
                _ = RunInferenceAsync(modelFrame, cancellationToken);
            }
        }
    }

    private async Task RunInferenceAsync(CameraFrame modelFrame, CancellationToken cancellationToken)
    {
        if (_model is null)
        {
            _isInferenceRunning = false;
            return;
        }

        var stopwatch = Stopwatch.StartNew();
        try
        {
            var result = await Task.Run(() => _model.Detect(modelFrame), cancellationToken);
            stopwatch.Stop();
            await Dispatcher.InvokeAsync(async () =>
            {
                if (cancellationToken.IsCancellationRequested)
                {
                    return;
                }

                await ApplyInferenceResultAsync(modelFrame, result, stopwatch.ElapsedMilliseconds);
            });
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception ex)
        {
            Log("Inference error: " + ex);
        }
        finally
        {
            _isInferenceRunning = false;
        }
    }

    private async Task ApplyInferenceResultAsync(CameraFrame modelFrame, HandLandmarkResult? result, long elapsedMs)
    {
        if (result is null)
        {
            if (_isRecordingGesture)
            {
                UpdateRecordingClock(DateTimeOffset.UtcNow);
                GestureText.Text = "recording";
                ConfidenceText.Text = "-";
                LatencyText.Text = $"No hand / {elapsedMs} ms";
                return;
            }

            ResetGestureUi($"No hand / {elapsedMs} ms");
            _recognizer.Reset();
            UpdateDebugLab(_recognizer.GetDebugSnapshot());
            return;
        }

        var now = DateTimeOffset.UtcNow;
        var sample = new GestureFrameSample(now, result.Landmarks, result.Confidence, result.BoundingBox, result.Handedness);

        if (_isRecordingGesture)
        {
            CaptureRecordingSample(sample);
        }

        var recognition = _isRecordingGesture ? null : _recognizer.Recognize(sample);
        UpdateDebugLab(_recognizer.GetDebugSnapshot());
        var hasBoundAction = recognition is not null && HasBindingForGesture(recognition.GestureId);
        DrawOverlay(modelFrame, result, GetOverlayStyle(recognition, hasBoundAction));

        var displayGesture = recognition?.DisplayName
            ?? (EnableBuiltInsCheckBox.IsChecked == true ? MediaPipeLandmarkGestureModel.Classify(result.Landmarks) : null);
        if (displayGesture is null)
        {
            GestureText.Text = _isRecordingGesture ? "recording" : "tracking";
            ConfidenceText.Text = result.Confidence.ToString("0.00");
            LatencyText.Text = $"{elapsedMs} ms";
            OverlayStatusText.Text = _isRecordingGesture ? "RECORDING" : "TRACKING";
            StateText.Text = _isRecordingGesture ? StateText.Text : "Tracking hand";
            return;
        }

        GestureText.Text = displayGesture;
        ConfidenceText.Text = (recognition?.Confidence ?? result.Confidence).ToString("0.00");
        LatencyText.Text = $"{elapsedMs} ms";
        OverlayStatusText.Text = $"{displayGesture}  {(recognition?.Confidence ?? result.Confidence):0.00}";

        if (recognition is not null)
        {
            await HandleRecognizedGestureAsync(recognition);
        }
    }

    private async Task HandleRecognizedGestureAsync(GestureRecognitionResult recognition)
    {
        StateText.Text = $"Matched: {recognition.DisplayName}";
        if (_isExecuting)
        {
            return;
        }

        var binding = _store.Load()
            .FirstOrDefault(x => StringComparer.OrdinalIgnoreCase.Equals(x.GestureId, recognition.GestureId));
        if (binding is null)
        {
            Log($"No binding for {recognition.DisplayName}.");
            return;
        }

        _isExecuting = true;
        try
        {
            Log($"Execute: {binding.DisplayName}");
            await Task.Run(() => _executor.Execute(binding.Action));
            StateText.Text = $"Executed: {recognition.DisplayName}";
        }
        finally
        {
            _isExecuting = false;
        }
    }

    private void ShowFrame(CameraFrame frame)
    {
        if (_bitmap is null || _bitmap.PixelWidth != frame.Width || _bitmap.PixelHeight != frame.Height)
        {
            _bitmap = new WriteableBitmap(frame.Width, frame.Height, 96, 96, PixelFormats.Bgra32, null);
            CameraImage.Source = _bitmap;
        }

        _bitmap.WritePixels(
            new Int32Rect(0, 0, frame.Width, frame.Height),
            frame.Bgra,
            frame.Width * 4,
            0);
    }

    private bool HasBindingForGesture(string gestureId)
    {
        return _store.Load()
            .Any(x => StringComparer.OrdinalIgnoreCase.Equals(x.GestureId, gestureId));
    }

    private OverlayStyle GetOverlayStyle(GestureRecognitionResult? recognition, bool hasBoundAction)
    {
        if (_isRecordingGesture)
        {
            return RecordingOverlay;
        }

        if (recognition is null)
        {
            return TrackingOverlay;
        }

        return hasBoundAction ? ActionReadyOverlay : GestureOnlyOverlay;
    }

    private void DrawOverlay(CameraFrame frame, HandLandmarkResult result, OverlayStyle style)
    {
        OverlayCanvas.Children.Clear();
        var bounds = GetImageBounds(frame);
        if (bounds.Width <= 1 || bounds.Height <= 1)
        {
            return;
        }

        if (result.BoundingBox is not null)
        {
            var box = result.BoundingBox;
            var rect = new Rectangle
            {
                Width = Math.Max(1, (box.X2 - box.X1) * bounds.ScaleX),
                Height = Math.Max(1, (box.Y2 - box.Y1) * bounds.ScaleY),
                Stroke = style.Box,
                StrokeThickness = 2
            };
            Canvas.SetLeft(rect, bounds.X + box.X1 * bounds.ScaleX);
            Canvas.SetTop(rect, bounds.Y + box.Y1 * bounds.ScaleY);
            OverlayCanvas.Children.Add(rect);
        }

        foreach (var (start, end) in HandConnections)
        {
            if (result.Landmarks.Count <= Math.Max(start, end))
            {
                continue;
            }

            var a = result.Landmarks[start];
            var b = result.Landmarks[end];
            OverlayCanvas.Children.Add(new Line
            {
                X1 = bounds.X + a.X * bounds.ScaleX,
                Y1 = bounds.Y + a.Y * bounds.ScaleY,
                X2 = bounds.X + b.X * bounds.ScaleX,
                Y2 = bounds.Y + b.Y * bounds.ScaleY,
                Stroke = style.Line,
                StrokeThickness = 5,
                StrokeStartLineCap = PenLineCap.Round,
                StrokeEndLineCap = PenLineCap.Round
            });
        }

        foreach (var landmark in result.Landmarks)
        {
            var point = new Ellipse
            {
                Width = 7,
                Height = 7,
                Fill = style.PointFill,
                Stroke = Brushes.Black,
                StrokeThickness = 1
            };
            Canvas.SetLeft(point, bounds.X + landmark.X * bounds.ScaleX - 3.5);
            Canvas.SetTop(point, bounds.Y + landmark.Y * bounds.ScaleY - 3.5);
            OverlayCanvas.Children.Add(point);
        }
    }

    private ImageBounds GetImageBounds(CameraFrame frame)
    {
        var hostWidth = PreviewHost.ActualWidth;
        var hostHeight = PreviewHost.ActualHeight;
        if (hostWidth <= 0 || hostHeight <= 0)
        {
            return new ImageBounds(0, 0, 0, 0, 0, 0);
        }

        var scale = Math.Min(hostWidth / frame.Width, hostHeight / frame.Height);
        var width = frame.Width * scale;
        var height = frame.Height * scale;
        return new ImageBounds(
            (hostWidth - width) / 2,
            (hostHeight - height) / 2,
            width,
            height,
            scale,
            scale);
    }

    private void ResetGestureUi(string status)
    {
        OverlayCanvas.Children.Clear();
        GestureText.Text = "-";
        ConfidenceText.Text = "-";
        LatencyText.Text = status;
        OverlayStatusText.Text = status;
        StateText.Text = "Running";
    }

    private void SaveBindingButton_Click(object sender, RoutedEventArgs e)
    {
        if (GestureCombo.SelectedItem is not GestureChoice selectedGesture)
        {
            Log("Select a gesture before saving a binding.");
            return;
        }

        var gestureId = selectedGesture.Id;
        var actionItem = (ComboBoxItem)ActionTypeCombo.SelectedItem;
        var actionType = actionItem.Tag.ToString()!;
        var value = ActionValueText.Text.Trim();
        if (string.IsNullOrWhiteSpace(value))
        {
            Log("Action value is empty.");
            return;
        }

        var paramName = actionType switch
        {
            "app.launch" => "path",
            "keyboard.hotkey" => "hotkey",
            "keyboard.typeText" => "text",
            _ => "value"
        };
        var binding = new GestureBinding(
            GestureId: gestureId,
            DisplayName: $"{selectedGesture.DisplayName} -> {actionItem.Content}",
            Action: new ActionSpec(actionType, new Dictionary<string, string> { [paramName] = value }));

        var bindings = _store.Load();
        bindings.RemoveAll(x => StringComparer.OrdinalIgnoreCase.Equals(x.GestureId, gestureId));
        bindings.Add(binding);
        _store.Save(bindings);
        RefreshBindings();
        Log($"Saved binding: {binding.DisplayName}");
    }

    private void ResetSampleButton_Click(object sender, RoutedEventArgs e)
    {
        _store.Save([]);
        RefreshBindings();
        Log("Bindings cleared.");
    }

    private void ActionTypeCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (ActionValueLabel is null || ActionValueText is null || ActionTypeCombo.SelectedItem is not ComboBoxItem item)
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

    private void RemoveLegacySampleBinding()
    {
        var bindings = _store.Load();
        var removed = bindings.RemoveAll(binding =>
            StringComparer.OrdinalIgnoreCase.Equals(binding.GestureId, "open_palm")
            && StringComparer.OrdinalIgnoreCase.Equals(binding.DisplayName, "Open palm -> launch Notepad")
            && StringComparer.OrdinalIgnoreCase.Equals(binding.Action.Type, "app.launch")
            && binding.Action.Params.TryGetValue("path", out var path)
            && StringComparer.OrdinalIgnoreCase.Equals(path, "notepad.exe"));
        if (removed == 0)
        {
            return;
        }

        _store.Save(bindings);
        Log("Removed legacy sample binding: open_palm -> Notepad.");
    }

    private void RefreshGestures()
    {
        _gestureDefinitions = _gestureStore.Load();
        _recognizer.SetDefinitions(_gestureDefinitions);
        var builtInsEnabled = EnableBuiltInsCheckBox?.IsChecked == true;
        _recognizer.SetBuiltInGesturesEnabled(builtInsEnabled);
        RuntimeModeText.Text = builtInsEnabled
            ? "Custom + built-in gestures / Cooldown: 1500 ms"
            : "Custom gestures only / Built-ins off";

        GesturesList.Items.Clear();
        if (builtInsEnabled)
        {
            foreach (var id in BuiltInGestureIds)
            {
                GesturesList.Items.Add($"{id}  built-in");
            }
        }

        foreach (var gesture in _gestureDefinitions.OrderBy(x => x.DisplayName))
        {
            var state = gesture.Templates.Count >= RequiredTemplateCount ? "active" : "needs examples";
            GesturesList.Items.Add($"{gesture.DisplayName}  {gesture.Templates.Count}/{RequiredTemplateCount} {state}");
        }

        GestureCombo.Items.Clear();
        if (builtInsEnabled)
        {
            foreach (var id in BuiltInGestureIds)
            {
                GestureCombo.Items.Add(new GestureChoice(id, id));
            }
        }

        foreach (var gesture in _gestureDefinitions.OrderBy(x => x.DisplayName))
        {
            GestureCombo.Items.Add(new GestureChoice(gesture.Id, gesture.DisplayName));
        }

        if (GestureCombo.Items.Count > 0)
        {
            GestureCombo.SelectedIndex = 0;
        }
    }

    private void EnableBuiltInsCheckBox_Changed(object sender, RoutedEventArgs e)
    {
        RefreshGestures();
        _recognizer.Reset();
        UpdateDebugLab(_recognizer.GetDebugSnapshot());
        Log(EnableBuiltInsCheckBox.IsChecked == true
            ? "Built-in gestures enabled."
            : "Built-in gestures disabled.");
    }

    private void RefreshBindings()
    {
        BindingsList.Items.Clear();
        foreach (var binding in _store.Load())
        {
            var value = string.Join(", ", binding.Action.Params.Select(x => $"{x.Key}={x.Value}"));
            BindingsList.Items.Add($"{binding.GestureId}: {binding.Action.Type} ({value})");
        }
    }

    private void ClearDebugButton_Click(object sender, RoutedEventArgs e)
    {
        DebugScoresList.Items.Clear();
        DebugVectorList.Items.Clear();
        DebugPathCanvas.Children.Clear();
        DebugStateText.Text = "Debug cleared";
        DebugCandidateText.Text = "No candidate vector";
    }

    private void RecordGestureButton_Click(object sender, RoutedEventArgs e)
    {
        if (_camera is null || _model is null)
        {
            Log("Start the camera before recording a gesture.");
            return;
        }

        var name = GestureNameText.Text.Trim();
        if (string.IsNullOrWhiteSpace(name))
        {
            Log("Gesture name is empty.");
            return;
        }

        _recordingSamples.Clear();
        _recordingGestureName = name;
        _recordingStartedAt = DateTimeOffset.UtcNow;
        _recordingCaptureStartedAt = _recordingStartedAt + RecordingLeadInDuration;
        _isRecordingGesture = true;
        _isCapturingGesture = false;
        RecordGestureButton.IsEnabled = false;
        RecordingProgressBar.Value = 0;
        RecordingWindowText.Text = "GET READY";
        RecordingStatusText.Text = $"{name}: capture starts in {RecordingLeadInDuration.TotalMilliseconds:0} ms";
        StateText.Text = $"Get ready: {name}";
        OverlayStatusText.Text = "GET READY";
    }

    private void DeleteGestureButton_Click(object sender, RoutedEventArgs e)
    {
        var name = GestureNameText.Text.Trim();
        if (string.IsNullOrWhiteSpace(name))
        {
            Log("Gesture name is empty.");
            return;
        }

        var definitions = _gestureStore.Load();
        var removed = definitions.RemoveAll(x =>
            StringComparer.OrdinalIgnoreCase.Equals(x.DisplayName, name)
            || StringComparer.OrdinalIgnoreCase.Equals(x.Id, name));
        if (removed == 0)
        {
            Log($"No custom gesture found for {name}.");
            return;
        }

        _gestureStore.Save(definitions);
        RefreshGestures();
        RefreshBindings();
        Log($"Deleted custom gesture: {name}");
    }

    private void CaptureRecordingSample(GestureFrameSample sample)
    {
        UpdateRecordingClock(sample.Timestamp);
        if (!_isRecordingGesture)
        {
            return;
        }

        if (sample.Timestamp < _recordingCaptureStartedAt)
        {
            return;
        }

        if (!_isCapturingGesture)
        {
            _isCapturingGesture = true;
            _recordingSamples.Clear();
            RecordingProgressBar.Value = 0;
            RecordingWindowText.Text = "CAPTURING";
            StateText.Text = $"Capturing: {_recordingGestureName}";
            OverlayStatusText.Text = "CAPTURING";
        }

        _recordingSamples.Add(sample);
        var elapsed = sample.Timestamp - _recordingCaptureStartedAt;
        RecordingProgressBar.Value = 100 * Math.Clamp(
            elapsed.TotalMilliseconds / Math.Max(1, RecordingDuration.TotalMilliseconds),
            0,
            1);
        RecordingWindowText.Text = "CAPTURING";
        RecordingStatusText.Text = $"{_recordingGestureName}: {elapsed.TotalMilliseconds:0}/{RecordingDuration.TotalMilliseconds:0} ms";
        OverlayStatusText.Text = $"CAPTURING  {elapsed.TotalMilliseconds:0} ms";
        if (elapsed < RecordingDuration)
        {
            return;
        }

        _isRecordingGesture = false;
        _isCapturingGesture = false;
        RecordGestureButton.IsEnabled = true;
        RecordingProgressBar.Value = 100;
        RecordingWindowText.Text = "DONE";
        SaveRecordedGestureTemplate();
    }

    private void UpdateRecordingClock(DateTimeOffset now)
    {
        if (!_isRecordingGesture)
        {
            return;
        }

        if (now < _recordingCaptureStartedAt)
        {
            var leadInElapsed = now - _recordingStartedAt;
            RecordingProgressBar.Value = 100 * Math.Clamp(
                leadInElapsed.TotalMilliseconds / Math.Max(1, RecordingLeadInDuration.TotalMilliseconds),
                0,
                1);
            var remaining = Math.Max(0, (_recordingCaptureStartedAt - now).TotalMilliseconds);
            RecordingWindowText.Text = "GET READY";
            RecordingStatusText.Text = $"{_recordingGestureName}: capture starts in {remaining:0} ms";
            OverlayStatusText.Text = "GET READY";
            return;
        }

        var elapsed = now - _recordingCaptureStartedAt;
        RecordingProgressBar.Value = 100 * Math.Clamp(
            elapsed.TotalMilliseconds / Math.Max(1, RecordingDuration.TotalMilliseconds),
            0,
            1);
        if (!_isCapturingGesture)
        {
            RecordingWindowText.Text = "CAPTURING";
            StateText.Text = $"Capturing: {_recordingGestureName}";
        }

        if (elapsed < RecordingDuration)
        {
            return;
        }

        _isRecordingGesture = false;
        _isCapturingGesture = false;
        RecordGestureButton.IsEnabled = true;
        RecordingProgressBar.Value = 100;
        RecordingWindowText.Text = "DONE";
        SaveRecordedGestureTemplate();
    }

    private void SaveRecordedGestureTemplate()
    {
        var template = GestureTemplateFactory.Create(_recordingSamples);
        if (template is null || _recordingGestureName is null)
        {
            RecordingStatusText.Text = "Recording failed: not enough tracked hand frames.";
            RecordingWindowText.Text = "FAILED";
            RecordGestureButton.IsEnabled = true;
            Log("Gesture recording failed: not enough tracked hand frames.");
            _recordingSamples.Clear();
            return;
        }

        var definitions = _gestureStore.Load();
        var definition = definitions.FirstOrDefault(x =>
            StringComparer.OrdinalIgnoreCase.Equals(x.DisplayName, _recordingGestureName));
        if (definition is null)
        {
            definition = new GestureDefinition(
                Id: CreateGestureId(_recordingGestureName, definitions),
                DisplayName: _recordingGestureName,
                Type: "template",
                Templates: [],
                CreatedAt: DateTimeOffset.UtcNow);
            definitions.Add(definition);
        }

        definition.Templates.Add(template);
        _gestureStore.Save(definitions);
        _recordingSamples.Clear();
        RefreshGestures();

        var remaining = Math.Max(0, RequiredTemplateCount - definition.Templates.Count);
        RecordingStatusText.Text = remaining == 0
            ? $"{definition.DisplayName} active with {definition.Templates.Count} examples."
            : $"{definition.DisplayName} saved. Record {remaining} more example(s).";
        Log($"Saved gesture example: {definition.DisplayName} ({definition.Templates.Count}/{RequiredTemplateCount})");
    }

    private static string CreateGestureId(string displayName, IReadOnlyList<GestureDefinition> existing)
    {
        var baseId = new string(displayName
            .Trim()
            .ToLowerInvariant()
            .Select(ch => char.IsLetterOrDigit(ch) ? ch : '_')
            .ToArray());
        baseId = string.Join("_", baseId.Split('_', StringSplitOptions.RemoveEmptyEntries));
        if (string.IsNullOrWhiteSpace(baseId))
        {
            baseId = "custom_gesture";
        }

        var id = baseId;
        var index = 2;
        while (existing.Any(x => StringComparer.OrdinalIgnoreCase.Equals(x.Id, id))
            || BuiltInGestureIds.Any(x => StringComparer.OrdinalIgnoreCase.Equals(x, id)))
        {
            id = $"{baseId}_{index}";
            index++;
        }

        return id;
    }

    private void Log(string message)
    {
        var line = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}  {message}";
        try
        {
            System.IO.File.AppendAllText(_logPath, line + Environment.NewLine);
        }
        catch
        {
        }

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

    private async Task DisposeRuntimeAsync()
    {
        _model?.Dispose();
        _model = null;

        if (_camera is not null)
        {
            _camera.DiagnosticLogged -= Log;
            await _camera.DisposeAsync();
            _camera = null;
        }

        _cameraLoopCts?.Dispose();
        _cameraLoopCts = null;
    }

    protected override async void OnClosed(EventArgs e)
    {
        _cameraLoopCts?.Cancel();
        await DisposeRuntimeAsync();
        base.OnClosed(e);
    }

    private readonly record struct ImageBounds(double X, double Y, double Width, double Height, double ScaleX, double ScaleY);
    private sealed record OverlayStyle(Brush Line, Brush PointFill, Brush Box);

    private sealed record GestureChoice(string Id, string DisplayName)
    {
        public override string ToString() => DisplayName;
    }

    private void UpdateDebugLab(GestureRecognitionDebugSnapshot snapshot)
    {
        if (PauseDebugCheckBox.IsChecked == true)
        {
            return;
        }

        var best = snapshot.BestDisplayName is null
            ? "best: -"
            : $"best: {snapshot.BestDisplayName} {snapshot.BestConfidence:0.00}";
        DebugStateText.Text =
            $"{snapshot.TriggerState} | {best} | static: {snapshot.StaticGestureId ?? "-"} | frames: {snapshot.BufferFrameCount} / {snapshot.BufferDurationMilliseconds:0} ms";

        DebugScoresList.Items.Clear();
        if (snapshot.Scores.Count == 0)
        {
            DebugScoresList.Items.Add("No custom templates scored.");
        }
        else
        {
            foreach (var score in snapshot.Scores
                .OrderByDescending(x => x.IsBest)
                .ThenByDescending(x => x.Confidence)
                .Take(24))
            {
                var marker = score.IsBest ? "* " : "  ";
                var template = score.TemplateIndex < 0 ? "-" : score.TemplateIndex.ToString();
                var rawScore = float.IsFinite(score.Score) ? score.Score.ToString("0.000") : "-";
                DebugScoresList.Items.Add(
                    $"{marker}{score.DisplayName} t{template} conf={score.Confidence:0.00} score={rawScore} threshold={snapshot.MatchThreshold:0.00} {score.Reason}");
            }
        }

        DrawDebugPaths(snapshot);
        UpdateDebugVectors(snapshot);
    }

    private void DrawDebugPaths(GestureRecognitionDebugSnapshot snapshot)
    {
        DebugPathCanvas.Children.Clear();
        var paths = new List<(IReadOnlyList<GestureTemplateSample> Samples, Brush Stroke, double Thickness)>
        {
            (snapshot.CandidateTemplate?.Samples ?? [], Brushes.Gold, 3)
        };

        var bestScore = snapshot.Scores.FirstOrDefault(x => x.IsBest);
        if (bestScore is not null)
        {
            var definition = _gestureDefinitions.FirstOrDefault(x =>
                StringComparer.OrdinalIgnoreCase.Equals(x.Id, bestScore.GestureId));
            if (definition is not null)
            {
                var brushes = new[] { Brushes.DeepSkyBlue, Brushes.LimeGreen, Brushes.OrangeRed, Brushes.HotPink };
                for (var i = 0; i < definition.Templates.Count; i++)
                {
                    paths.Add((definition.Templates[i].Samples, brushes[i % brushes.Length], 1.4));
                }
            }
        }

        var allPoints = paths.SelectMany(x => x.Samples).ToList();
        if (allPoints.Count == 0)
        {
            DebugPathCanvas.Children.Add(new TextBlock
            {
                Text = "Waiting for candidate vector",
                Foreground = Brushes.Gray,
                Margin = new Thickness(10)
            });
            return;
        }

        var minX = allPoints.Min(x => x.CenterX);
        var maxX = allPoints.Max(x => x.CenterX);
        var minY = allPoints.Min(x => x.CenterY);
        var maxY = allPoints.Max(x => x.CenterY);
        var width = Math.Max(1, DebugPathCanvas.ActualWidth > 1 ? DebugPathCanvas.ActualWidth : 400);
        var height = Math.Max(1, DebugPathCanvas.ActualHeight > 1 ? DebugPathCanvas.ActualHeight : 116);
        var pad = 14d;

        foreach (var path in paths.Where(x => x.Samples.Count > 0))
        {
            var line = new Polyline
            {
                Stroke = path.Stroke,
                StrokeThickness = path.Thickness,
                StrokeStartLineCap = PenLineCap.Round,
                StrokeEndLineCap = PenLineCap.Round
            };

            foreach (var point in path.Samples)
            {
                line.Points.Add(MapDebugPoint(point, minX, maxX, minY, maxY, width, height, pad));
            }

            DebugPathCanvas.Children.Add(line);
        }
    }

    private void UpdateDebugVectors(GestureRecognitionDebugSnapshot snapshot)
    {
        DebugVectorList.Items.Clear();
        var candidate = snapshot.CandidateTemplate?.Samples;
        if (candidate is null || candidate.Count == 0)
        {
            DebugCandidateText.Text = "No candidate vector";
            return;
        }

        DebugCandidateText.Text =
            $"candidate: {candidate.Count} samples, {snapshot.CandidateTemplate!.SourceFrameCount} source frames, avg conf {snapshot.CandidateTemplate.AverageConfidence:0.00}";
        if (DebugRawVectorsCheckBox.IsChecked != true)
        {
            return;
        }

        var bestTemplate = FindBestTemplate(snapshot);
        for (var i = 0; i < candidate.Count; i++)
        {
            var c = candidate[i];
            var t = bestTemplate is not null && bestTemplate.Count > i ? bestTemplate[i] : null;
            var distance = t is null ? "-" : SampleDistance(c, t).ToString("0.000");
            var templateCenter = t is null ? "(-,-)" : $"({t.CenterX:0.00},{t.CenterY:0.00})";
            DebugVectorList.Items.Add(
                $"{i + 1:00} cand=({c.CenterX:0.00},{c.CenterY:0.00}) tmpl={templateCenter} d={distance} v0={c.Values.ElementAtOrDefault(0):0.00},{c.Values.ElementAtOrDefault(1):0.00}");
        }
    }

    private IReadOnlyList<GestureTemplateSample>? FindBestTemplate(GestureRecognitionDebugSnapshot snapshot)
    {
        var bestScore = snapshot.Scores.FirstOrDefault(x => x.IsBest);
        if (bestScore is null || bestScore.TemplateIndex < 1)
        {
            return null;
        }

        var definition = _gestureDefinitions.FirstOrDefault(x =>
            StringComparer.OrdinalIgnoreCase.Equals(x.Id, bestScore.GestureId));
        return definition is null || definition.Templates.Count < bestScore.TemplateIndex
            ? null
            : definition.Templates[bestScore.TemplateIndex - 1].Samples;
    }

    private static Point MapDebugPoint(
        GestureTemplateSample point,
        float minX,
        float maxX,
        float minY,
        float maxY,
        double width,
        double height,
        double pad)
    {
        var xSpan = Math.Max(0.001f, maxX - minX);
        var ySpan = Math.Max(0.001f, maxY - minY);
        var x = pad + ((point.CenterX - minX) / xSpan * Math.Max(1, width - (pad * 2)));
        var y = pad + ((maxY - point.CenterY) / ySpan * Math.Max(1, height - (pad * 2)));
        return new Point(x, y);
    }

    private static float SampleDistance(GestureTemplateSample a, GestureTemplateSample b)
    {
        var centerDx = a.CenterX - b.CenterX;
        var centerDy = a.CenterY - b.CenterY;
        var sum = MathF.Sqrt((centerDx * centerDx) + (centerDy * centerDy));
        var count = 1;
        var valueCount = Math.Min(a.Values.Length, b.Values.Length);
        for (var i = 0; i < valueCount; i += 2)
        {
            var dx = a.Values[i] - b.Values[i];
            var dy = a.Values[i + 1] - b.Values[i + 1];
            sum += MathF.Sqrt((dx * dx) + (dy * dy));
            count++;
        }

        return sum / count;
    }
}

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
    private const int StableFrameThreshold = 3;
    private static readonly TimeSpan Cooldown = TimeSpan.FromMilliseconds(1500);
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
    private readonly ActionExecutor _executor = new();
    private readonly string _modelDirectory;
    private readonly string _logPath;

    private CancellationTokenSource? _cameraLoopCts;
    private RealtimeCameraFrameSource? _camera;
    private MediaPipeHandsModel? _model;
    private WriteableBitmap? _bitmap;
    private string? _candidateGesture;
    private int _candidateFrames;
    private DateTimeOffset _lastExecution = DateTimeOffset.MinValue;
    private bool _isExecuting;
    private bool _isInferenceRunning;
    private readonly DispatcherTimer _nativePollTimer;
    private string? _lastNativeRuntimeError;

    public MainWindow()
    {
        InitializeComponent();

        _store = new BindingStore(System.IO.Path.Combine(AppContext.BaseDirectory, "bindings.json"));
        _modelDirectory = System.IO.Path.Combine(AppContext.BaseDirectory, "models");
        _logPath = System.IO.Path.Combine(AppContext.BaseDirectory, "ryoikitenkai.log");
        NativeVisionHostControl.DiagnosticLogged += Log;
        _nativePollTimer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(250)
        };
        _nativePollTimer.Tick += NativePollTimer_Tick;
        EnsureSeedData();
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

            Log("Falling back to managed WPF camera pipeline.");
        }

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
        _lastNativeRuntimeError = null;
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
            OverlayStatusText.Text =
                $"Native  camera {metrics.CameraFps:0.0}  display {metrics.DisplayFps:0.0} fps  " +
                $"preprocess {metrics.PreprocessMs:0.0} ms  drops {metrics.FramePoolDroppedFrames}/{metrics.PerceptionDroppedFrames}";
        }
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
            ResetGestureUi($"No hand / {elapsedMs} ms");
            return;
        }

        DrawOverlay(modelFrame, result);
        var gestureId = MediaPipeLandmarkGestureModel.Classify(result.Landmarks);
        if (gestureId is null)
        {
            ResetGestureUi($"Hand detected / {elapsedMs} ms");
            return;
        }

        GestureText.Text = gestureId;
        ConfidenceText.Text = result.Confidence.ToString("0.00");
        LatencyText.Text = $"{elapsedMs} ms";
        OverlayStatusText.Text = $"{gestureId}  {result.Confidence:0.00}";

        await HandleStableGestureAsync(gestureId, result.Confidence);
    }

    private async Task HandleStableGestureAsync(string gestureId, float confidence)
    {
        if (StringComparer.OrdinalIgnoreCase.Equals(_candidateGesture, gestureId))
        {
            _candidateFrames++;
        }
        else
        {
            _candidateGesture = gestureId;
            _candidateFrames = 1;
        }

        var elapsedSinceExecution = DateTimeOffset.UtcNow - _lastExecution;
        StateText.Text = _candidateFrames < StableFrameThreshold
            ? $"Candidate: {gestureId} ({_candidateFrames}/{StableFrameThreshold})"
            : elapsedSinceExecution < Cooldown
                ? $"Cooldown: {gestureId}"
                : $"Stable: {gestureId}";

        if (_candidateFrames < StableFrameThreshold || elapsedSinceExecution < Cooldown || _isExecuting)
        {
            return;
        }

        var binding = _store.Load()
            .FirstOrDefault(x => StringComparer.OrdinalIgnoreCase.Equals(x.GestureId, gestureId));
        if (binding is null)
        {
            Log($"No binding for {gestureId}.");
            _lastExecution = DateTimeOffset.UtcNow;
            return;
        }

        _isExecuting = true;
        try
        {
            Log($"Execute: {binding.DisplayName}");
            await Task.Run(() => _executor.Execute(binding.Action));
            _lastExecution = DateTimeOffset.UtcNow;
            StateText.Text = $"Executed: {gestureId}";
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

    private void DrawOverlay(CameraFrame frame, HandLandmarkResult result)
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
                Stroke = new SolidColorBrush(Color.FromArgb(180, 0, 255, 180)),
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
                Stroke = Brushes.Lime,
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
                Fill = Brushes.White,
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
        _candidateGesture = null;
        _candidateFrames = 0;
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

        var paramName = actionType switch
        {
            "app.launch" => "path",
            "keyboard.hotkey" => "hotkey",
            "keyboard.typeText" => "text",
            _ => "value"
        };
        var binding = new GestureBinding(
            GestureId: gestureId,
            DisplayName: $"{gestureId} -> {actionItem.Content}",
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
        EnsureSeedData();
        RefreshBindings();
        Log("Sample binding restored.");
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

    private void EnsureSeedData()
    {
        if (_store.Load().Count > 0)
        {
            return;
        }

        _store.Save([
            new GestureBinding(
                GestureId: "open_palm",
                DisplayName: "Open palm -> launch Notepad",
                Action: new ActionSpec(
                    Type: "app.launch",
                    Params: new Dictionary<string, string>
                    {
                        ["path"] = "notepad.exe"
                    }))
        ]);
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
}

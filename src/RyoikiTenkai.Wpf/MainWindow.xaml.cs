using System.Diagnostics;
using System.Text.Json;
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
    private const int NativeMetricsPollMilliseconds = 250;
    private const int NativeGesturePollMilliseconds = 16;
    private const int MaxUiLogLines = 1000;
    private static readonly bool GestureRecognitionEnabled = true;
    private static readonly TimeSpan NoHandResetGrace = TimeSpan.FromMilliseconds(500);
    private static readonly TimeSpan RecordingLeadInDuration = TimeSpan.FromMilliseconds(900);
    private static readonly TimeSpan RecordingDuration = TimeSpan.FromSeconds(2);
    private static readonly TimeSpan DebugTimelineHistoryDuration = TimeSpan.FromSeconds(12);
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
    private readonly WindowGestureRecognizer _lowHandednessRecognizer = new();
    private readonly WindowGestureRecognizer _highHandednessRecognizer = new();
    private readonly MultiHandWindowGestureRecognizer _multiHandRecognizer = new();
    private readonly string _gestureStorePath;
    private readonly string _modelDirectory;
    private readonly string _logPath;
    private readonly string _classifierFrameLogPath;
    private readonly string _debugExportPath;
    private readonly string _debugTimelineLogPath;
    private readonly GestureDebugSession _debugSession = new(TimeSpan.FromMinutes(5), 20_000);

    private List<GestureDefinition> _gestureDefinitions = [];
    private CancellationTokenSource? _cameraLoopCts;
    private RealtimeCameraFrameSource? _camera;
    private MediaPipeHandsModel? _model;
    private WriteableBitmap? _bitmap;
    private bool _isExecuting;
    private bool _isInferenceRunning;
    private readonly DispatcherTimer _nativePollTimer;
    private readonly DispatcherTimer _nativeGesturePollTimer;
    private readonly DispatcherTimer _gesturePlaybackTimer;
    private readonly DispatcherTimer _debugPlaybackTimer;
    private readonly DispatcherTimer _gestureFoundToastTimer;
    private string? _lastNativeRuntimeError;
    private string? _lastNativeProviderSummary;
    private string? _lastNativeProviderFallbackReason;
    private DateTimeOffset _lastNativePerfLogAt;
    private string _lastNativeLatencyText = "-";
    private bool _isRecordingGesture;
    private bool _isCapturingGesture;
    private bool _recordingTakeFailed;
    private int _recordingTakeIndex;
    private int _recordingAttemptCount;
    private DateTimeOffset _recordingStartedAt;
    private DateTimeOffset _recordingCaptureStartedAt;
    private string? _recordingGestureName;
    private GestureKind? _recordingGestureKind;
    private readonly List<GestureFrameSample> _recordingSamples = [];
    private readonly List<GestureFrameSetSample> _recordingFrameSetSamples = [];
    private readonly List<GestureTemplate> _recordingCompletedTemplates = [];
    private readonly List<GestureRecording> _recordingCompletedRecordings = [];
    private readonly List<DebugTimelinePoint> _debugTimelineHistory = [];
    private bool _debugLiveFollow = true;
    private bool _isDebugPlaybackRunning;
    private bool _isUpdatingDebugSlider;
    private SkeletonVideoDebugWindow? _skeletonDebugWindow;
    private DateTimeOffset? _lastValidHandSampleAt;
    private bool _recognizerResetForNoHand;
    private ulong _lastProcessedNativeHandFrameId;
    private IReadOnlyList<GestureSkeletonFrame> _playbackFrames = [];
    private int _playbackFrameIndex;
    private string _playbackGestureName = string.Empty;

    public MainWindow()
    {
        InitializeComponent();

        _store = new BindingStore(System.IO.Path.Combine(AppContext.BaseDirectory, "bindings.json"));
        _gestureStorePath = System.IO.Path.Combine(AppContext.BaseDirectory, "gestures.json");
        _gestureStore = new GestureDefinitionStore(_gestureStorePath);
        _modelDirectory = System.IO.Path.Combine(AppContext.BaseDirectory, "models");
        _logPath = System.IO.Path.Combine(AppContext.BaseDirectory, "ryoikitenkai.log");
        _classifierFrameLogPath = System.IO.Path.Combine(AppContext.BaseDirectory, "classifier-frames.jsonl");
        _debugExportPath = System.IO.Path.Combine(AppContext.BaseDirectory, "gesture-debug-session.jsonl");
        _debugTimelineLogPath = System.IO.Path.Combine(
            AppContext.BaseDirectory,
            $"gesture-debug-{DateTimeOffset.Now:yyyyMMdd-HHmmss}.jsonl");
        NativeVisionHostControl.DiagnosticLogged += Log;
        _nativePollTimer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(NativeMetricsPollMilliseconds)
        };
        _nativePollTimer.Tick += NativePollTimer_Tick;
        _nativeGesturePollTimer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(NativeGesturePollMilliseconds)
        };
        _nativeGesturePollTimer.Tick += NativeGesturePollTimer_Tick;
        _gesturePlaybackTimer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(66)
        };
        _gesturePlaybackTimer.Tick += GesturePlaybackTimer_Tick;
        _debugPlaybackTimer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(33)
        };
        _debugPlaybackTimer.Tick += DebugPlaybackTimer_Tick;
        _gestureFoundToastTimer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(1800)
        };
        _gestureFoundToastTimer.Tick += GestureFoundToastTimer_Tick;
        RefreshGestures();
        RefreshBindings();
        ApplyGestureRecognitionMode();
        StopGesturePlayback("Select a recorded custom gesture to play its hand skeleton.");
        Log($"Ready. Log file: {_logPath}");
        Log($"Classifier frame log file: {_classifierFrameLogPath}");
        Log($"Gesture debug timeline file: {_debugTimelineLogPath}");
        Log($"Gesture store file: {_gestureStorePath}");
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
        _lastNativeLatencyText = "-";
        _lastProcessedNativeHandFrameId = 0;
        _nativePollTimer.Start();
        _nativeGesturePollTimer.Start();
        Log($"Using native runtime path. Gesture samples poll every {NativeGesturePollMilliseconds} ms (~{1000.0 / NativeGesturePollMilliseconds:0} Hz).");
        return true;
    }

    private void StopNativeRuntime()
    {
        _nativePollTimer.Stop();
        _nativeGesturePollTimer.Stop();
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

            if (NativeVisionHostControl.TryGetPalm(out var palm)
                && palm.FrameId > 0)
            {
                GestureText.Text = palm.PalmCount > 0 ? "palm" : "none";
                ConfidenceText.Text = palm.PalmCount > 0
                    ? palm.Confidence.ToString("0.000")
                    : "-";
            }
            _lastNativeLatencyText = $"{metrics.EndToEndLatencyMs:0.0} ms";
            LatencyText.Text = _lastNativeLatencyText;
            StateText.Text = $"Native frame {metrics.FrameId}";
            RuntimeProviderText.Text = metrics.PalmProvider.Contains("QNN", StringComparison.OrdinalIgnoreCase)
                || metrics.HandProvider.Contains("QNN", StringComparison.OrdinalIgnoreCase)
                ? "QNN/HTP NPU"
                : "Native";
            var workingSetMb = Process.GetCurrentProcess().WorkingSet64 / (1024.0 * 1024.0);
            var frameMb = metrics.FrameBytes / (1024.0 * 1024.0);
            var tensorMb = metrics.TensorInputBytes / (1024.0 * 1024.0);
            OverlayStatusText.Text =
                $"Native frame {metrics.FrameId}  perc {metrics.PerceptionFps:0.0} fps  graph {metrics.GraphTotalMs:0.0} ms";
            LogNativePerformanceSample(metrics, providerSummary, workingSetMb);
        }
    }

    private async void NativeGesturePollTimer_Tick(object? sender, EventArgs e)
    {
        if (!NativeVisionHostControl.NativeAvailable || !NativeVisionHostControl.IsStarted)
        {
            return;
        }

        if (!NativeVisionHostControl.TryGetHand(out var hand)
            || hand.FrameId == 0
            || hand.HandCount <= 0)
        {
            if (_isRecordingGesture)
            {
                UpdateRecordingClock(DateTimeOffset.UtcNow);
                GestureText.Text = "recording";
                ConfidenceText.Text = "-";
            }
            else
            {
                TrackNoHandForRecognition(DateTimeOffset.UtcNow, "native hand unavailable");
            }

            return;
        }

        if (hand.FrameId == _lastProcessedNativeHandFrameId)
        {
            return;
        }

        _lastProcessedNativeHandFrameId = hand.FrameId;
        var handCount = Math.Min(hand.HandCount, NativeVisionInterop.MaxHands);
        var bestConfidence = 0f;
        var bestHandIndex = 0;
        for (var index = 0; index < handCount; ++index)
        {
            var confidence = hand.GetConfidence(index);
            if (confidence > bestConfidence)
            {
                bestConfidence = confidence;
                bestHandIndex = index;
            }
        }

        GestureText.Text = handCount == 1 ? "hand" : $"{handCount} hands";
        ConfidenceText.Text = bestConfidence.ToString("0.000");
        var timestamp = DateTimeOffset.UtcNow;
        var frameSet = handCount >= 2 ? CreateNativeGestureFrameSet(hand, timestamp) : null;
        if (_isRecordingGesture)
        {
            var sample = CreateNativeGestureSample(hand, bestHandIndex, timestamp);
            if (frameSet is not null)
            {
                CaptureRecordingFrameSet(frameSet);
            }

            TrackValidHandSample(sample.Timestamp);
            await ProcessNativeGestureSampleAsync(
                sample,
                elapsedText: _lastNativeLatencyText,
                updateTrackingState: false,
                frameSet);
            return;
        }

        if (frameSet is not null)
        {
            TrackValidHandSample(frameSet.Timestamp);
            await ProcessNativeGestureFrameSetAsync(frameSet, _lastNativeLatencyText);
        }

        var singleHandSample = CreateNativeGestureSample(hand, bestHandIndex, timestamp);
        TrackValidHandSample(singleHandSample.Timestamp);
        await ProcessNativeGestureSampleAsync(
            singleHandSample,
            elapsedText: handCount == 1 ? _lastNativeLatencyText : $"{_lastNativeLatencyText} best hand {bestHandIndex + 1}/{handCount}",
            updateTrackingState: false,
            frameSet);
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

    private async Task ProcessNativeGestureSampleAsync(
        GestureFrameSample sample,
        string elapsedText,
        bool updateTrackingState,
        GestureFrameSetSample? frameSet = null)
    {
        try
        {
            await ProcessGestureSampleAsync(
                sample,
                elapsedText,
                drawOverlay: null,
                updateTrackingState,
                frameSet);
        }
        catch (Exception ex)
        {
            Log("Native gesture classification error: " + ex);
        }
    }

    private async Task ProcessNativeGestureFrameSetAsync(
        GestureFrameSetSample frameSet,
        string elapsedText)
    {
        try
        {
            var recognition = GestureRecognitionEnabled
                ? _multiHandRecognizer.Recognize(frameSet)
                : null;
            if (recognition is null)
            {
                return;
            }

            var hasBoundAction = HasBindingForGesture(recognition.GestureId);
            var sample = frameSet.Hands
                .OrderByDescending(x => x.Confidence)
                .FirstOrDefault();
            if (sample is not null)
            {
                var debugFrame = _debugSession.AddHandFrame(
                    sample,
                    _multiHandRecognizer.GetDebugSnapshot(),
                    recognition,
                    hasBoundAction,
                    "native-two-hand",
                    elapsedText,
                    frameSet);
                SaveDebugFrame(debugFrame);
                UpdateDebugLab(debugFrame);
            }

            ShowGestureFoundNotification(recognition, hasBoundAction, "two-hand");
            if (hasBoundAction)
            {
                await HandleRecognizedGestureAsync(recognition);
            }
        }
        catch (Exception ex)
        {
            Log("Native two-hand gesture classification error: " + ex);
        }
    }

    private void TrackValidHandSample(DateTimeOffset timestamp)
    {
        _lastValidHandSampleAt = timestamp;
        _recognizerResetForNoHand = false;
    }

    private void TrackNoHandForRecognition(DateTimeOffset now, string reason)
    {
        var missingDuration = _lastValidHandSampleAt is null
            ? NoHandResetGrace
            : now - _lastValidHandSampleAt.Value;
        if (missingDuration < NoHandResetGrace || _recognizerResetForNoHand)
        {
            return;
        }

        ResetRecognizers($"No hand for {missingDuration.TotalMilliseconds:0} ms");
        _recognizerResetForNoHand = true;
        Log($"Classifier reset: no hand for {missingDuration.TotalMilliseconds:0} ms ({reason}).");
        var debugFrame = _debugSession.AddNoHandFrame(now, reason, _recognizer.GetDebugSnapshot());
        SaveDebugFrame(debugFrame);
        UpdateDebugLab(debugFrame);
    }

    private void SetRecognizerDefinitions(IEnumerable<GestureDefinition> definitions)
    {
        var materialized = definitions.ToList();
        _recognizer.SetDefinitions(materialized);
        _lowHandednessRecognizer.SetDefinitions(materialized);
        _highHandednessRecognizer.SetDefinitions(materialized);
        _multiHandRecognizer.SetDefinitions(materialized);
    }

    private void ResetRecognizers(string triggerState)
    {
        _recognizer.Reset(triggerState);
        _lowHandednessRecognizer.Reset(triggerState);
        _highHandednessRecognizer.Reset(triggerState);
        _multiHandRecognizer.Reset(triggerState);
    }

    private WindowGestureRecognizer RecognizerForSample(GestureFrameSample sample)
    {
        return _recognizer;
    }

    private static unsafe GestureFrameSample CreateNativeGestureSample(
        NativeHandResult hand,
        int handIndex,
        DateTimeOffset timestamp)
    {
        var landmarks = new List<HandLandmark>(21);
        var landmarkOffset = handIndex * 21 * 3;
        for (var landmarkIndex = 0; landmarkIndex < 21; landmarkIndex++)
        {
            var offset = landmarkOffset + (landmarkIndex * 3);
            landmarks.Add(new HandLandmark(
                hand.Landmarks[offset],
                hand.Landmarks[offset + 1],
                hand.Landmarks[offset + 2]));
        }

        var confidence = hand.GetConfidence(handIndex);
        var handedness = hand.Handedness[handIndex];
        var bboxOffset = handIndex * 4;
        var boundingBox = new HandBox(
            hand.Bbox[bboxOffset],
            hand.Bbox[bboxOffset + 1],
            hand.Bbox[bboxOffset + 2],
            hand.Bbox[bboxOffset + 3]);

        return new GestureFrameSample(timestamp, landmarks, confidence, boundingBox, handedness);
    }

    private static GestureFrameSetSample CreateNativeGestureFrameSet(
        NativeHandResult hand,
        DateTimeOffset timestamp)
    {
        var handCount = Math.Min(hand.HandCount, NativeVisionInterop.MaxHands);
        var hands = new List<GestureFrameSample>(handCount);
        for (var index = 0; index < handCount; index++)
        {
            hands.Add(CreateNativeGestureSample(hand, index, timestamp));
        }

        return new GestureFrameSetSample(timestamp, hands);
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
            var results = await Task.Run(() => _model.DetectHands(modelFrame), cancellationToken);
            stopwatch.Stop();
            await Dispatcher.InvokeAsync(async () =>
            {
                if (cancellationToken.IsCancellationRequested)
                {
                    return;
                }

                await ApplyInferenceResultsAsync(modelFrame, results, stopwatch.ElapsedMilliseconds);
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

    private async Task ApplyInferenceResultsAsync(CameraFrame modelFrame, IReadOnlyList<HandLandmarkResult> results, long elapsedMs)
    {
        var result = results.OrderByDescending(x => x.Confidence).FirstOrDefault();
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
            TrackNoHandForRecognition(DateTimeOffset.UtcNow, $"managed no hand / {elapsedMs} ms");
            return;
        }

        var timestamp = DateTimeOffset.UtcNow;
        var frameSet = new GestureFrameSetSample(
            timestamp,
            results
                .OrderByDescending(x => x.Confidence)
                .Take(2)
                .Select(x => new GestureFrameSample(timestamp, x.Landmarks, x.Confidence, x.BoundingBox, x.Handedness))
                .ToList());
        if (_isRecordingGesture && frameSet.Hands.Count >= 2)
        {
            CaptureRecordingFrameSet(frameSet);
        }
        else if (!_isRecordingGesture && frameSet.Hands.Count >= 2)
        {
            await ProcessNativeGestureFrameSetAsync(frameSet, $"{elapsedMs} ms");
        }

        var sample = new GestureFrameSample(
            timestamp,
            result.Landmarks,
            result.Confidence,
            result.BoundingBox,
            result.Handedness);
        TrackValidHandSample(sample.Timestamp);
        await ProcessGestureSampleAsync(
            sample,
            elapsedText: $"{elapsedMs} ms",
            drawOverlay: (modelFrame, result),
            updateTrackingState: true,
            frameSet.Hands.Count >= 2 ? frameSet : null);
    }

    private async Task ProcessGestureSampleAsync(
        GestureFrameSample sample,
        string elapsedText,
        (CameraFrame Frame, HandLandmarkResult Result)? drawOverlay,
        bool updateTrackingState,
        GestureFrameSetSample? frameSet = null)
    {
        if (_isRecordingGesture)
        {
            CaptureRecordingSample(sample);
        }

        var recognizer = RecognizerForSample(sample);
        var recognition = !_isRecordingGesture && GestureRecognitionEnabled
            ? recognizer.Recognize(sample)
            : null;
        var snapshot = _isRecordingGesture
            ? CreateTrackingOnlyDebugSnapshot(sample, "Recording gesture")
            : !GestureRecognitionEnabled
                ? CreateTrackingOnlyDebugSnapshot(sample, "Gesture recognition disabled")
            : recognizer.GetDebugSnapshot();
        var hasBoundAction = recognition is not null && HasBindingForGesture(recognition.GestureId);
        var debugFrame = _debugSession.AddHandFrame(
            sample,
            snapshot,
            recognition,
            hasBoundAction,
            drawOverlay is null ? "native" : "managed",
            elapsedText,
            frameSet);
        SaveDebugFrame(debugFrame);
        UpdateDebugLab(debugFrame);
        if (drawOverlay is { } overlay)
        {
            DrawOverlay(overlay.Frame, overlay.Result, GetOverlayStyle(recognition, hasBoundAction));
        }

        var displayGesture = recognition?.DisplayName;
        if (displayGesture is null)
        {
            GestureText.Text = _isRecordingGesture ? "recording" : "tracking";
            ConfidenceText.Text = sample.Confidence.ToString("0.00");
            LatencyText.Text = elapsedText;
            OverlayStatusText.Text = _isRecordingGesture ? "RECORDING" : "TRACKING";
            if (updateTrackingState)
            {
                StateText.Text = _isRecordingGesture ? StateText.Text : "Tracking hand";
            }
            return;
        }

        GestureText.Text = displayGesture;
        ConfidenceText.Text = (recognition?.Confidence ?? sample.Confidence).ToString("0.00");
        LatencyText.Text = elapsedText;
        OverlayStatusText.Text = $"{displayGesture}  {(recognition?.Confidence ?? sample.Confidence):0.00}";

        if (recognition is not null)
        {
            ShowGestureFoundNotification(recognition, hasBoundAction, recognition.Source);
            await HandleRecognizedGestureAsync(recognition);
        }
    }

    private void ShowGestureFoundNotification(GestureRecognitionResult recognition, bool hasBoundAction, string source)
    {
        GestureFoundNameText.Text = recognition.DisplayName;
        GestureFoundMetaText.Text =
            $"{recognition.Confidence:0.00} confidence  |  {source}  |  {(hasBoundAction ? "binding ready" : "no binding")}";
        GestureFoundToast.Visibility = Visibility.Visible;
        _gestureFoundToastTimer.Stop();
        _gestureFoundToastTimer.Start();
    }

    private void GestureFoundToastTimer_Tick(object? sender, EventArgs e)
    {
        _gestureFoundToastTimer.Stop();
        GestureFoundToast.Visibility = Visibility.Collapsed;
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
        if (!GestureRecognitionEnabled)
        {
            Log("Gesture recognition is disabled for redesign; bindings are not active.");
            return;
        }

        if (GestureCombo.SelectedItem is not GestureChoice selectedGesture)
        {
            Log("Select a gesture before saving a binding.");
            return;
        }

        var gestureId = selectedGesture.Id;
        if (ActionTypeCombo.SelectedItem is not ListBoxItem actionItem)
        {
            Log("Select an action type before saving a binding.");
            return;
        }

        var actionType = actionItem.Tag?.ToString() ?? string.Empty;
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

    private void DeleteBindingButton_Click(object sender, RoutedEventArgs e)
    {
        if (BindingsList.SelectedItem is not BindingListItem selected)
        {
            Log("Select a binding to delete.");
            return;
        }

        var bindings = _store.Load();
        var removed = bindings.RemoveAll(x =>
            StringComparer.OrdinalIgnoreCase.Equals(x.GestureId, selected.GestureId));
        if (removed == 0)
        {
            Log($"Binding not found: {selected.GestureId}");
            return;
        }

        _store.Save(bindings);
        RefreshBindings();
        Log($"Deleted binding: {selected.GestureId}");
    }

    private async void TestBindingButton_Click(object sender, RoutedEventArgs e)
    {
        if (BindingsList.SelectedItem is not BindingListItem selected)
        {
            Log("Select a binding to test.");
            return;
        }

        var binding = _store.Load()
            .FirstOrDefault(x => StringComparer.OrdinalIgnoreCase.Equals(x.GestureId, selected.GestureId));
        if (binding is null)
        {
            Log($"Binding not found: {selected.GestureId}");
            return;
        }

        try
        {
            Log($"Test binding: {binding.DisplayName}");
            await Task.Run(() => _executor.Execute(binding.Action));
        }
        catch (Exception ex)
        {
            Log("Test binding failed: " + ex.Message);
        }
    }

    private void ResetSampleButton_Click(object sender, RoutedEventArgs e)
    {
        if (!GestureRecognitionEnabled)
        {
            Log("Gesture recognition is disabled for redesign; bindings are not active.");
            return;
        }

        _store.Save([]);
        RefreshBindings();
        Log("Bindings cleared.");
    }

    private void ActionTypeCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (ActionValueLabel is null || ActionValueText is null || ActionTypeCombo.SelectedItem is not ListBoxItem item)
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

    private void ApplyGestureRecognitionMode()
    {
        if (GestureRecognitionEnabled)
        {
            return;
        }

        RuntimeModeText.Text = "Gesture recognition disabled";
        GestureText.Text = "tracking";
        ConfidenceText.Text = "-";
        RecordingCounterText.Text = "Recognizer removed";
        RecordingWindowText.Text = "Disabled";
        RecordingStatusText.Text = "Skeleton video recording/debugging only.";
        GestureNameText.IsEnabled = false;
        GesturesList.IsEnabled = false;
        GestureCombo.IsEnabled = false;
        ActionTypeCombo.IsEnabled = false;
        ActionValueText.IsEnabled = false;
        RecordGestureButton.IsEnabled = false;
        DeleteGestureButton.IsEnabled = false;
        FlushGesturesButton.IsEnabled = false;
        SaveBindingButton.IsEnabled = false;
        DeleteBindingButton.IsEnabled = false;
        TestBindingButton.IsEnabled = false;
        ClearBindingsButton.IsEnabled = false;
        BindingsList.Items.Clear();
        BindingsList.Items.Add("Disabled while gesture recognition is redesigned.");
    }

    private void RefreshGestures()
    {
        if (!GestureRecognitionEnabled)
        {
            _gestureDefinitions = [];
            SetRecognizerDefinitions([]);
            RuntimeModeText.Text = "Gesture recognition disabled";
            GesturesList.Items.Clear();
            GestureCombo.Items.Clear();
            if (!_isRecordingGesture)
            {
                RecordingCounterText.Text = "Recognizer removed";
                RecordingStatusText.Text = "Skeleton video recording/debugging only.";
                RecordingWindowText.Text = "Disabled";
            }
            return;
        }

        _gestureDefinitions = _gestureStore.Load();
        SetRecognizerDefinitions(_gestureDefinitions);
        RuntimeModeText.Text = "Recorded custom gestures only";

        GesturesList.Items.Clear();
        foreach (var gesture in _gestureDefinitions.OrderBy(x => x.DisplayName))
        {
            var activeTemplateCount = CountActiveTemplates(gesture);
            var state = activeTemplateCount >= RequiredTemplateCount ? "active" : "needs examples";
            var avgMotion = gesture.Templates.Count == 0
                ? 0
                : gesture.Templates.Average(x => x.MotionSummary?.MotionScore ?? 0);
            var recordingCount = gesture.Recordings?.Count ?? gesture.Templates.Count;
            GesturesList.Items.Add(new GestureListItem(
                gesture.Id,
                gesture.DisplayName,
                $"{activeTemplateCount}/{RequiredTemplateCount} {state}  recordings {recordingCount}  motion {avgMotion:0.00}"));
        }

        GestureCombo.Items.Clear();
        foreach (var gesture in _gestureDefinitions.OrderBy(x => x.DisplayName))
        {
            var activeTemplateCount = CountActiveTemplates(gesture);
            var status = activeTemplateCount >= RequiredTemplateCount
                ? "active"
                : $"{activeTemplateCount}/{RequiredTemplateCount}";
            GestureCombo.Items.Add(new GestureChoice(gesture.Id, gesture.DisplayName, status));
        }

        if (GestureCombo.Items.Count > 0)
        {
            GestureCombo.SelectedIndex = 0;
        }

        if (!_isRecordingGesture)
        {
            var activeCount = _gestureDefinitions.Count(x => CountActiveTemplates(x) >= RequiredTemplateCount);
            RecordingCounterText.Text = "Accepted 0/3  Idle";
            RecordingStatusText.Text = $"{activeCount}/{_gestureDefinitions.Count} custom gestures active";
        }

        LogLoadedGestureState("Gesture store loaded");
    }

    private void LogLoadedGestureState(string prefix)
    {
        var ids = _gestureDefinitions.Count == 0
            ? "-"
            : string.Join(", ", _gestureDefinitions.Select(x => $"{x.Id}:{CountActiveTemplates(x)}/{x.Templates.Count}"));
        Log($"{prefix}: count={_gestureDefinitions.Count}; ids={ids}; path={_gestureStorePath}");
    }

    private static int CountActiveTemplates(GestureDefinition gesture)
    {
        return gesture.Templates.Count(template =>
            GestureTemplateFactory.IsRecognizableOneHandTemplate(template)
            || MultiHandGestureFeatureExtractor.IsDistinctTwoHandTemplate(template));
    }

    private void GesturesList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (GesturesList.SelectedItem is not GestureListItem item)
        {
            StopGesturePlayback("Select a recorded custom gesture to play its hand skeleton.");
            return;
        }

        GestureNameText.Text = item.DisplayName;
        var definition = _gestureDefinitions.FirstOrDefault(x =>
            StringComparer.OrdinalIgnoreCase.Equals(x.Id, item.Id));
        var template = definition?.Templates.LastOrDefault(x => x.SkeletonFrames?.Count > 0);
        if (definition is null || template?.SkeletonFrames is not { Count: > 0 } frames)
        {
            StopGesturePlayback($"{item.DisplayName}: no skeleton frames stored yet. Record another example.");
            return;
        }

        StartGesturePlayback(item.DisplayName, frames);
    }

    private void RefreshBindings()
    {
        BindingsList.Items.Clear();
        foreach (var binding in _store.Load())
        {
            var value = string.Join(", ", binding.Action.Params.Select(x => $"{x.Key}={x.Value}"));
            BindingsList.Items.Add(new BindingListItem(
                binding.GestureId,
                binding.DisplayName,
                binding.Action.Type,
                value));
        }
    }

    private void ClearDebugButton_Click(object sender, RoutedEventArgs e)
    {
        StopDebugPlayback();
        _debugSession.Clear();
        SkeletonVideoDebugView.Clear();
        _skeletonDebugWindow?.Clear();
        ClassifierDebugRecordButton.Content = "Live";
        DebugRecordText.Text = "No debug frames captured yet";
        DebugScoresList.Items.Clear();
        DebugVectorList.Items.Clear();
        _debugTimelineHistory.Clear();
        DebugTimelineCanvas.Children.Clear();
        DebugSkeletonCanvas.Children.Clear();
        DebugMatchCanvas.Children.Clear();
        DebugPathCanvas.Children.Clear();
        DebugStateText.Text = "Live follow";
        DebugCandidateText.Text = "No candidate vector";
        DebugSelectionText.Text = "-";
        UpdateDebugSlider(null);
    }

    private void OpenSkeletonDebuggerButton_Click(object sender, RoutedEventArgs e)
    {
        EnsureSkeletonDebuggerWindow();
        _skeletonDebugWindow!.Show();
        _skeletonDebugWindow.Activate();
        if (_debugSession.Latest is { } latest)
        {
            UpdateSkeletonDebuggers(latest);
        }
    }

    private void EnsureSkeletonDebuggerWindow()
    {
        if (_skeletonDebugWindow is not null)
        {
            return;
        }

        _skeletonDebugWindow = new SkeletonVideoDebugWindow
        {
            Owner = this
        };
        _skeletonDebugWindow.Closed += (_, _) => _skeletonDebugWindow = null;
    }

    private void ClassifierDebugRecordButton_Click(object sender, RoutedEventArgs e)
    {
        StopDebugPlayback();
        _debugLiveFollow = true;
        PauseDebugCheckBox.IsChecked = false;
        ClassifierDebugRecordButton.Content = "Live";
        DebugStateText.Text = "Live follow";
        if (_debugSession.Latest is { } latest)
        {
            RenderDebugFrame(latest);
        }
    }

    private void DebugStepBackButton_Click(object sender, RoutedEventArgs e)
    {
        StopDebugPlayback();
        StepDebugFrame(-1);
    }

    private void DebugStepForwardButton_Click(object sender, RoutedEventArgs e)
    {
        StopDebugPlayback();
        StepDebugFrame(1);
    }

    private void DebugPlayPauseButton_Click(object sender, RoutedEventArgs e)
    {
        if (_isDebugPlaybackRunning)
        {
            StopDebugPlayback();
            return;
        }

        if (_debugSession.Frames.Count == 0)
        {
            return;
        }

        _debugLiveFollow = false;
        PauseDebugCheckBox.IsChecked = true;
        _isDebugPlaybackRunning = true;
        DebugPlayPauseButton.Content = "Pause";
        DebugStateText.Text = "Playing timeline";
        _debugPlaybackTimer.Start();
    }

    private void DebugPlaybackTimer_Tick(object? sender, EventArgs e)
    {
        if (_debugSession.Frames.Count == 0)
        {
            StopDebugPlayback();
            return;
        }

        var current = (int)Math.Round(DebugFrameSlider.Value);
        if (current >= _debugSession.Frames.Count - 1)
        {
            StopDebugPlayback();
            return;
        }

        SelectDebugFrame(current + 1);
    }

    private void StepDebugFrame(int delta)
    {
        if (_debugSession.Frames.Count == 0)
        {
            return;
        }

        var current = (int)Math.Round(DebugFrameSlider.Value);
        SelectDebugFrame(current + delta);
    }

    private void SelectDebugFrame(int index)
    {
        var frames = _debugSession.Frames;
        if (frames.Count == 0)
        {
            return;
        }

        index = Math.Clamp(index, 0, frames.Count - 1);
        _debugLiveFollow = false;
        PauseDebugCheckBox.IsChecked = true;
        DebugStateText.Text = "Scrubbed frame";
        RenderDebugFrame(frames[index]);
    }

    private void StopDebugPlayback()
    {
        _debugPlaybackTimer.Stop();
        _isDebugPlaybackRunning = false;
        DebugPlayPauseButton.Content = "Play";
    }

    private void ExportDebugButton_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            _debugSession.ExportJsonLines(_debugExportPath);
            DebugRecordText.Text = $"Exported {_debugSession.Frames.Count} frames";
            Log($"Gesture debug session exported: {_debugExportPath}");
        }
        catch (Exception ex)
        {
            DebugRecordText.Text = "Export failed";
            Log("Gesture debug export failed: " + ex);
        }
    }

    private void DebugFrameSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_isUpdatingDebugSlider)
        {
            return;
        }

        var frame = _debugSession.FrameAtSliderValue(e.NewValue);
        if (frame is null)
        {
            return;
        }

        _debugLiveFollow = false;
        PauseDebugCheckBox.IsChecked = true;
        DebugStateText.Text = "Scrubbed frame";
        RenderDebugFrame(frame);
    }

    private void RecordGestureButton_Click(object sender, RoutedEventArgs e)
    {
        if (!GestureRecognitionEnabled)
        {
            Log("Gesture recording is disabled while recognition is redesigned. Skeleton debug recording still runs.");
            return;
        }

        if (_isRecordingGesture)
        {
            if (_recordingTakeFailed)
            {
                _recordingTakeFailed = false;
                BeginRecordingTake(_recordingTakeIndex, DateTimeOffset.UtcNow, "Retrying failed take.");
                return;
            }

            CancelGestureRecording();
            return;
        }

        if ((_camera is null || _model is null) && !NativeVisionHostControl.IsStarted)
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

        _recordingGestureName = name;
        _recordingCompletedTemplates.Clear();
        _recordingCompletedRecordings.Clear();
        _recordingAttemptCount = 0;
        _recordingTakeFailed = false;
        _recordingGestureKind = null;
        BeginRecordingTake(1, DateTimeOffset.UtcNow, statusPrefix: null);
        Log($"Gesture recording session armed: {name}. Capturing {RequiredTemplateCount} takes of {RecordingDuration.TotalMilliseconds:0} ms each.");
    }

    private void BeginRecordingTake(int takeIndex, DateTimeOffset now, string? statusPrefix)
    {
        _recordingSamples.Clear();
        _recordingFrameSetSamples.Clear();
        _recordingTakeIndex = takeIndex;
        _recordingAttemptCount++;
        _recordingStartedAt = now;
        _recordingCaptureStartedAt = now + RecordingLeadInDuration;
        _isRecordingGesture = true;
        _isCapturingGesture = false;
        RecordGestureButton.Content = "Cancel";
        RecordGestureButton.IsEnabled = true;
        RecordingProgressBar.Value = 0;
        RecordingWindowText.Text = "GET READY";
        UpdateRecordingCounterText();
        var prefix = string.IsNullOrWhiteSpace(statusPrefix) ? string.Empty : $"{statusPrefix} ";
        RecordingStatusText.Text = $"{_recordingGestureName}: get ready";
        Log($"{prefix}{_recordingGestureName}: take {_recordingTakeIndex}/{RequiredTemplateCount} starts in {RecordingLeadInDuration.TotalMilliseconds:0} ms. Accepted {_recordingCompletedTemplates.Count}/{RequiredTemplateCount}. Saving to {_gestureStorePath}");
        StateText.Text = $"Get ready: {_recordingGestureName} take {_recordingTakeIndex}/{RequiredTemplateCount}";
        OverlayStatusText.Text = "GET READY";
    }

    private void CancelGestureRecording()
    {
        var name = _recordingGestureName ?? "gesture";
        _isRecordingGesture = false;
        _isCapturingGesture = false;
        _recordingSamples.Clear();
        _recordingFrameSetSamples.Clear();
        _recordingCompletedTemplates.Clear();
        _recordingCompletedRecordings.Clear();
        _recordingGestureName = null;
        _recordingGestureKind = null;
        _recordingTakeIndex = 0;
        _recordingAttemptCount = 0;
        _recordingTakeFailed = false;
        RecordGestureButton.Content = "Record 3 Examples";
        RecordGestureButton.IsEnabled = true;
        RecordingWindowText.Text = "CANCELLED";
        UpdateRecordingCounterText();
        RecordingStatusText.Text = $"{name}: cancelled";
        OverlayStatusText.Text = "TRACKING";
        Log($"Gesture recording cancelled: {name}");
    }

    private void UpdateRecordingCounterText()
    {
        var accepted = _recordingCompletedTemplates.Count;
        var phase = _recordingTakeFailed
            ? "failed"
            : _isCapturingGesture
                ? "capturing"
                : _isRecordingGesture
                    ? "ready"
                    : "idle";
        RecordingCounterText.Text =
            $"Accepted {accepted}/{RequiredTemplateCount}  Take {_recordingTakeIndex}/{RequiredTemplateCount}  Attempts {_recordingAttemptCount}  {_recordingGestureKind?.ToString() ?? "-"}  {phase}";
    }

    private void DeleteGestureButton_Click(object sender, RoutedEventArgs e)
    {
        if (!GestureRecognitionEnabled)
        {
            Log("Gesture recognition is disabled for redesign; stored gesture editing is inactive.");
            return;
        }

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

    private void FlushGesturesButton_Click(object sender, RoutedEventArgs e)
    {
        if (!GestureRecognitionEnabled)
        {
            Log("Gesture recognition is disabled for redesign; stored gesture editing is inactive.");
            return;
        }

        FlushAllGestures();
    }

    private void FlushAllGestures()
    {
        if (_isRecordingGesture)
        {
            CancelGestureRecording();
        }

        _gestureStore.Save([]);
        _gestureDefinitions.Clear();
        SetRecognizerDefinitions([]);
        ResetRecognizers("Gestures flushed");
        _recordingCompletedTemplates.Clear();
        _recordingCompletedRecordings.Clear();
        _recordingSamples.Clear();
        _recordingFrameSetSamples.Clear();
        _recordingGestureKind = null;
        StopGesturePlayback("Recorded custom gestures flushed.");
        RefreshGestures();
        RefreshBindings();
        SelectGestureInList(string.Empty);
        RecordingCounterText.Text = "Accepted 0/3  Idle";
        RecordingWindowText.Text = "Idle";
        RecordingStatusText.Text = "All recorded gestures flushed";
        AppendDebugCascadeLine($"{DateTimeOffset.Now:HH:mm:ss.fff} Gestures flushed; recognizer reset.");
        Log($"Flushed all recorded custom gestures from memory and {_gestureStorePath}.");
    }

    private void CaptureRecordingSample(GestureFrameSample sample)
    {
        UpdateRecordingClock(sample.Timestamp);
        if (!_isRecordingGesture || _recordingTakeFailed)
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
            _recordingFrameSetSamples.Clear();
            RecordingProgressBar.Value = 0;
            RecordingWindowText.Text = "CAPTURING";
            StateText.Text = $"Capturing: {_recordingGestureName} take {_recordingTakeIndex}/{RequiredTemplateCount}";
            OverlayStatusText.Text = "CAPTURING";
            UpdateRecordingCounterText();
        }

        _recordingSamples.Add(sample);
        AddSingleHandFallbackFrameSet(sample);
        var elapsed = sample.Timestamp - _recordingCaptureStartedAt;
        RecordingProgressBar.Value = 100 * Math.Clamp(
            elapsed.TotalMilliseconds / Math.Max(1, RecordingDuration.TotalMilliseconds),
            0,
            1);
        RecordingWindowText.Text = "CAPTURING";
        RecordingStatusText.Text =
            $"{_recordingGestureName}: {elapsed.TotalMilliseconds:0}/{RecordingDuration.TotalMilliseconds:0} ms";
        OverlayStatusText.Text = $"CAPTURING  {elapsed.TotalMilliseconds:0} ms";
        if (elapsed < RecordingDuration)
        {
            return;
        }

        RecordingProgressBar.Value = 100;
        RecordingWindowText.Text = "DONE";
        FinishRecordingTake();
    }

    private void UpdateRecordingClock(DateTimeOffset now)
    {
        if (!_isRecordingGesture || _recordingTakeFailed)
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
            UpdateRecordingCounterText();
            RecordingStatusText.Text =
                $"{_recordingGestureName}: starts in {remaining:0} ms";
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
            StateText.Text = $"Capturing: {_recordingGestureName} take {_recordingTakeIndex}/{RequiredTemplateCount}";
            UpdateRecordingCounterText();
        }

        if (elapsed < RecordingDuration)
        {
            return;
        }

        RecordingProgressBar.Value = 100;
        RecordingWindowText.Text = "DONE";
        FinishRecordingTake();
    }

    private void CaptureRecordingFrameSet(GestureFrameSetSample frameSet)
    {
        UpdateRecordingClock(frameSet.Timestamp);
        if (!_isRecordingGesture || _recordingTakeFailed || frameSet.Timestamp < _recordingCaptureStartedAt)
        {
            return;
        }

        if (!_isCapturingGesture)
        {
            _isCapturingGesture = true;
            _recordingSamples.Clear();
            _recordingFrameSetSamples.Clear();
            RecordingProgressBar.Value = 0;
            RecordingWindowText.Text = "CAPTURING";
            StateText.Text = $"Capturing: {_recordingGestureName} take {_recordingTakeIndex}/{RequiredTemplateCount}";
            OverlayStatusText.Text = "CAPTURING";
            UpdateRecordingCounterText();
        }

        _recordingFrameSetSamples.Add(frameSet);
    }

    private void AddSingleHandFallbackFrameSet(GestureFrameSample sample)
    {
        if (_recordingFrameSetSamples.Count > 0)
        {
            return;
        }

        _recordingFrameSetSamples.Add(new GestureFrameSetSample(sample.Timestamp, [sample]));
    }

    private void FinishRecordingTake()
    {
        if (!_isRecordingGesture || _recordingTakeFailed)
        {
            return;
        }

        _isCapturingGesture = false;
        var twoHandFrameCount = MultiHandGestureFeatureExtractor.CountUsableTwoHandFrames(_recordingFrameSetSamples);
        var useTwoHandRecording = MultiHandGestureFeatureExtractor.HasPredominantTwoHandCoverage(
            _recordingFrameSetSamples,
            Math.Max(_recordingSamples.Count, _recordingFrameSetSamples.Count));
        var creation = useTwoHandRecording
            ? MultiHandGestureFeatureExtractor.TryCreate(_recordingFrameSetSamples)
            : GestureTemplateFactory.TryCreate(_recordingSamples);
        var template = creation.Template;
        if (template is null || _recordingGestureName is null)
        {
            var reason = string.IsNullOrWhiteSpace(creation.FailureReason)
                ? "Template creation returned no result."
                : creation.FailureReason;
            var failure = $"Take {_recordingTakeIndex}/{RequiredTemplateCount} failed: {reason} {FormatTemplateCreationCounts(creation)}";
            _recordingTakeFailed = true;
            _isCapturingGesture = false;
            RecordingStatusText.Text = TrimDebugLine(failure, 180);
            RecordingWindowText.Text = "RETRY - SEE REASON";
            RecordGestureButton.Content = "Retry Take";
            UpdateRecordingCounterText();
            Log($"Gesture recording take failed: {_recordingGestureName} take {_recordingTakeIndex}/{RequiredTemplateCount}. {reason} {FormatTemplateCreationCounts(creation)}");
            _recordingSamples.Clear();
            _recordingFrameSetSamples.Clear();
            return;
        }

        _recordingGestureKind ??= template.Kind;
        var recordingId = $"{DateTimeOffset.UtcNow:yyyyMMddHHmmssfff}-{_recordingTakeIndex}";
        var recording = CreateGestureRecording(
            recordingId,
            _recordingTakeIndex,
            _recordingSamples,
            _recordingFrameSetSamples,
            creation,
            accepted: true,
            reason: "Accepted");
        _recordingCompletedTemplates.Add(template with { SourceRecordingId = recordingId });
        _recordingCompletedRecordings.Add(recording);
        UpdateRecordingCounterText();
        Log($"Captured gesture take: {_recordingGestureName} ({_recordingCompletedTemplates.Count}/{RequiredTemplateCount}) unified motion={template.MotionSummary?.MotionScore:0.000} rawFrames={recording.Frames.Count} twoHandFrames={twoHandFrameCount} twoHandMode={useTwoHandRecording} {FormatTemplateCreationCounts(creation)}");
        _recordingSamples.Clear();
        _recordingFrameSetSamples.Clear();

        if (_recordingCompletedTemplates.Count < RequiredTemplateCount)
        {
            RecordingWindowText.Text = "TAKE SAVED";
            var saved = $"{_recordingGestureName}: saved take {_recordingCompletedTemplates.Count}/{RequiredTemplateCount}.";
            RecordingStatusText.Text = "Take saved";
            BeginRecordingTake(_recordingCompletedTemplates.Count + 1, DateTimeOffset.UtcNow, saved);
            return;
        }

        SaveRecordedGestureSession();
    }

    private void SaveRecordedGestureSession()
    {
        if (_recordingGestureName is null || _recordingCompletedTemplates.Count == 0)
        {
            CancelGestureRecording();
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
                CreatedAt: DateTimeOffset.UtcNow,
                Recordings: []);
            definitions.Add(definition);
        }

        definition.Templates.AddRange(_recordingCompletedTemplates);
        var recordings = definition.Recordings ?? [];
        recordings.AddRange(_recordingCompletedRecordings);
        definition = definition with
        {
            Recordings = recordings,
            UpdatedAt = DateTimeOffset.UtcNow,
            SchemaVersion = 4
        };
        var definitionIndex = definitions.FindIndex(x => StringComparer.OrdinalIgnoreCase.Equals(x.Id, definition.Id));
        if (definitionIndex >= 0)
        {
            definitions[definitionIndex] = definition;
        }

        _gestureStore.Save(definitions);
        var savedCount = _recordingCompletedTemplates.Count;
        _recordingCompletedTemplates.Clear();
        _recordingCompletedRecordings.Clear();
        _recordingSamples.Clear();
        _recordingFrameSetSamples.Clear();
        _recordingGestureKind = null;
        _isRecordingGesture = false;
        _isCapturingGesture = false;
        _recordingTakeIndex = 0;
        _recordingAttemptCount = 0;
        _recordingTakeFailed = false;
        RecordGestureButton.Content = "Record 3 Examples";
        RecordGestureButton.IsEnabled = true;
        RefreshGestures();
        SelectGestureInList(definition.Id);
        RecordingCounterText.Text = $"Accepted {savedCount}/{RequiredTemplateCount}  Complete  unified";

        var remaining = Math.Max(0, RequiredTemplateCount - definition.Templates.Count);
        RecordingStatusText.Text = remaining == 0
            ? $"{definition.DisplayName} active"
            : $"{definition.DisplayName}: need {remaining} more";
        Log($"Saved gesture session: {definition.DisplayName} (+{savedCount}, total {definition.Templates.Count}/{RequiredTemplateCount}) to {_gestureStorePath}");
    }

    private static string FormatRecordingSampleCounts(IReadOnlyList<GestureFrameSample> samples)
    {
        var landmarkFrames = samples.Count(x => x.Landmarks.Count >= 21);
        var usableFrames = samples.Count(x => x.Landmarks.Count >= 21 && x.Confidence >= 0.35f);
        var durationMilliseconds = samples.Count < 2
            ? 0
            : Math.Max(0, (samples[^1].Timestamp - samples[0].Timestamp).TotalMilliseconds);
        var fps = durationMilliseconds <= 0
            ? 0
            : samples.Count * 1000.0 / durationMilliseconds;
        var usableFps = durationMilliseconds <= 0
            ? 0
            : usableFrames * 1000.0 / durationMilliseconds;
        return
            $"{samples.Count} captured, {landmarkFrames} landmark, {usableFrames}/{GestureTemplateFactory.MinimumUsableSampleCount} usable, " +
            $"fps {fps:0.0} / usable {usableFps:0.0}";
    }

    private static GestureRecording CreateGestureRecording(
        string recordingId,
        int takeIndex,
        IReadOnlyList<GestureFrameSample> samples,
        IReadOnlyList<GestureFrameSetSample> frameSets,
        GestureTemplateCreationResult creation,
        bool accepted,
        string reason)
    {
        var ordered = samples.OrderBy(x => x.Timestamp).ToList();
        var firstTimestamp = ordered.Count == 0 ? DateTimeOffset.UtcNow : ordered[0].Timestamp;
        var durationMilliseconds = ordered.Count < 2
            ? 0
            : Math.Max(0, (ordered[^1].Timestamp - ordered[0].Timestamp).TotalMilliseconds);
        var fps = durationMilliseconds <= 0 ? 0 : ordered.Count * 1000.0 / durationMilliseconds;
        var usableFps = durationMilliseconds <= 0
            ? 0
            : creation.HighConfidenceFrameCount * 1000.0 / durationMilliseconds;
        var featureTrack = GestureFeatureExtractor.BuildFeatureTrack(ordered);
        var featureByTime = featureTrack.Frames
            .GroupBy(x => Math.Round(x.TimeOffsetMilliseconds, 3))
            .ToDictionary(x => x.Key, x => x.First());

        return new GestureRecording(
            recordingId,
            takeIndex,
            DateTimeOffset.UtcNow,
            durationMilliseconds,
            ordered.Count == 0 ? 0 : (float)ordered.Average(x => x.Confidence),
            new GestureRecordingQuality(
                accepted,
                creation.SourceFrameCount,
                creation.ValidLandmarkFrameCount,
                creation.HighConfidenceFrameCount,
                fps,
                usableFps,
                reason),
            ordered.Select(sample =>
                {
                    var timeOffset = Math.Max(0, (sample.Timestamp - firstTimestamp).TotalMilliseconds);
                    var fingerPose = sample.Landmarks.Count >= 21
                        ? GestureFeatureExtractor.AnalyzeFingerPose(sample.Landmarks)
                        : new GestureFingerPose([0, 0, 0, 0, 0], 0);
                    featureByTime.TryGetValue(Math.Round(timeOffset, 3), out var features);
                    return new GestureRecordingFrame(
                        timeOffset,
                        sample.Confidence,
                        sample.Handedness,
                        sample.BoundingBox is null
                            ? null
                            : new GestureRecordingBox(
                                sample.BoundingBox.X1,
                                sample.BoundingBox.Y1,
                                sample.BoundingBox.X2,
                                sample.BoundingBox.Y2),
                        sample.Landmarks
                            .Take(21)
                            .Select(point => new GestureSkeletonPoint(point.X, point.Y, point.Z))
                            .ToList(),
                        fingerPose.Straightness,
                        fingerPose.StateMask,
                        features);
                })
                .ToList(),
            featureTrack,
            CreateMultiHandRecordingFrames(frameSets));
    }

    private static List<GestureMultiHandRecordingFrame>? CreateMultiHandRecordingFrames(
        IReadOnlyList<GestureFrameSetSample> frameSets)
    {
        var ordered = frameSets
            .Where(x => x.Hands.Count >= 2)
            .OrderBy(x => x.Timestamp)
            .ToList();
        if (ordered.Count == 0)
        {
            return null;
        }

        var firstTimestamp = ordered[0].Timestamp;
        return ordered.Select(frameSet =>
        {
            var timeOffset = Math.Max(0, (frameSet.Timestamp - firstTimestamp).TotalMilliseconds);
            return new GestureMultiHandRecordingFrame(
                timeOffset,
                OrderPairedHands(frameSet.Hands)
                    .Select(hand => CreateRecordingFrame(timeOffset, hand))
                    .ToList());
        }).ToList();
    }

    private static IReadOnlyList<GestureFrameSample> OrderPairedHands(IReadOnlyList<GestureFrameSample> hands)
    {
        var selected = hands
            .Where(x => x.Landmarks.Count >= 21)
            .OrderByDescending(x => x.Confidence)
            .Take(2)
            .ToList();
        if (selected.Count < 2)
        {
            return selected;
        }

        var handednessGap = MathF.Abs(selected[0].Handedness - selected[1].Handedness);
        return handednessGap >= 0.20f
            ? selected.OrderBy(x => x.Handedness).ToList()
            : selected.OrderBy(x => ((x.Landmarks[0].X + x.Landmarks[9].X) / 2f)).ToList();
    }

    private static GestureRecordingFrame CreateRecordingFrame(double timeOffset, GestureFrameSample sample)
    {
        var fingerPose = sample.Landmarks.Count >= 21
            ? GestureFeatureExtractor.AnalyzeFingerPose(sample.Landmarks)
            : new GestureFingerPose([0, 0, 0, 0, 0], 0);
        return new GestureRecordingFrame(
            timeOffset,
            sample.Confidence,
            sample.Handedness,
            sample.BoundingBox is null
                ? null
                : new GestureRecordingBox(
                    sample.BoundingBox.X1,
                    sample.BoundingBox.Y1,
                    sample.BoundingBox.X2,
                    sample.BoundingBox.Y2),
            sample.Landmarks
                .Take(21)
                .Select(point => new GestureSkeletonPoint(point.X, point.Y, point.Z))
                .ToList(),
            fingerPose.Straightness,
            fingerPose.StateMask);
    }

    private static string FormatTemplateCreationCounts(GestureTemplateCreationResult creation)
    {
        return
            $"Captured={creation.SourceFrameCount}, landmarks={creation.ValidLandmarkFrameCount}, " +
            $"usable={creation.HighConfidenceFrameCount}, minConf={creation.MinimumConfidence:0.00}, " +
            $"duration={creation.DurationMilliseconds:0} ms, sequence=unified, " +
            $"motion={creation.Template?.MotionSummary?.MotionScore ?? 0:0.000}.";
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
        while (existing.Any(x => StringComparer.OrdinalIgnoreCase.Equals(x.Id, id)))
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
        while (ActionLogList.Items.Count > MaxUiLogLines)
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
        _debugPlaybackTimer.Stop();
        _gesturePlaybackTimer.Stop();
        _cameraLoopCts?.Cancel();
        await DisposeRuntimeAsync();
        base.OnClosed(e);
    }

    private readonly record struct ImageBounds(double X, double Y, double Width, double Height, double ScaleX, double ScaleY);
    private sealed record OverlayStyle(Brush Line, Brush PointFill, Brush Box);

    private sealed record GestureChoice(string Id, string DisplayName, string Status = "")
    {
        public override string ToString() => string.IsNullOrWhiteSpace(Status)
            ? DisplayName
            : $"{DisplayName}  {Status}";
    }

    private sealed record GestureListItem(string Id, string DisplayName, string Status = "")
    {
        public override string ToString() => $"{DisplayName}  {Status}";
    }

    private sealed record BindingListItem(string GestureId, string DisplayName, string ActionType, string Value)
    {
        public override string ToString() => $"{GestureId}  ->  {ActionType}  {Value}";
    }

    private sealed record DebugTimelinePoint(
        DateTimeOffset Timestamp,
        int FrameCount,
        int UsableFrameCount,
        double WindowDurationMilliseconds,
        double EffectiveFps,
        double UsableFps,
        string? BestDisplayName,
        float? BestConfidence,
        GestureRecognitionPath Path,
        float MotionScore,
        string TriggerState,
        GestureConfirmedMatch? ConfirmedMatch);

    private void SelectGestureInList(string gestureId)
    {
        foreach (var item in GesturesList.Items.OfType<GestureListItem>())
        {
            if (StringComparer.OrdinalIgnoreCase.Equals(item.Id, gestureId))
            {
                GesturesList.SelectedItem = item;
                return;
            }
        }
    }

    private void StartGesturePlayback(string gestureName, IReadOnlyList<GestureSkeletonFrame> frames)
    {
        _playbackGestureName = gestureName;
        _playbackFrames = frames;
        _playbackFrameIndex = 0;
        DrawGesturePlaybackFrame();
        _gesturePlaybackTimer.Start();
    }

    private void StopGesturePlayback(string message)
    {
        _gesturePlaybackTimer.Stop();
        _playbackFrames = [];
        _playbackFrameIndex = 0;
        _playbackGestureName = string.Empty;
        GesturePlaybackText.Text = "Recorded hand skeleton playback";
        Log("Gesture playback stopped: " + message);
        GesturePlaybackCanvas.Children.Clear();
        GesturePlaybackCanvas.Children.Add(new TextBlock
        {
            Text = "No recorded skeleton selected.",
            Foreground = Brushes.Gray,
            Margin = new Thickness(10)
        });
    }

    private void GesturePlaybackTimer_Tick(object? sender, EventArgs e)
    {
        if (_playbackFrames.Count == 0)
        {
            return;
        }

        _playbackFrameIndex = (_playbackFrameIndex + 1) % _playbackFrames.Count;
        DrawGesturePlaybackFrame();
    }

    private void DrawGesturePlaybackFrame()
    {
        GesturePlaybackCanvas.Children.Clear();
        if (_playbackFrames.Count == 0)
        {
            return;
        }

        var frame = _playbackFrames[Math.Clamp(_playbackFrameIndex, 0, _playbackFrames.Count - 1)];
        if (frame.Landmarks.Count < 21)
        {
            GesturePlaybackText.Text = "Recorded hand skeleton playback";
            Log($"{_playbackGestureName}: playback frame has {frame.Landmarks.Count}/21 landmarks");
            return;
        }

        GesturePlaybackText.Text = "Recorded hand skeleton playback";

        var width = Math.Max(1, GesturePlaybackCanvas.ActualWidth > 1 ? GesturePlaybackCanvas.ActualWidth : 400);
        var height = Math.Max(1, GesturePlaybackCanvas.ActualHeight > 1 ? GesturePlaybackCanvas.ActualHeight : 146);
        var pad = 16d;
        var minX = frame.Landmarks.Min(x => x.X);
        var maxX = frame.Landmarks.Max(x => x.X);
        var minY = frame.Landmarks.Min(x => x.Y);
        var maxY = frame.Landmarks.Max(x => x.Y);
        var xSpan = Math.Max(0.001f, maxX - minX);
        var ySpan = Math.Max(0.001f, maxY - minY);

        Point Map(GestureSkeletonPoint point)
        {
            var x = width - pad - ((point.X - minX) / xSpan * Math.Max(1, width - (pad * 2)));
            var y = pad + ((point.Y - minY) / ySpan * Math.Max(1, height - (pad * 2)));
            return new Point(x, y);
        }

        foreach (var (start, end) in HandConnections)
        {
            var a = Map(frame.Landmarks[start]);
            var b = Map(frame.Landmarks[end]);
            GesturePlaybackCanvas.Children.Add(new Line
            {
                X1 = a.X,
                Y1 = a.Y,
                X2 = b.X,
                Y2 = b.Y,
                Stroke = Brushes.DeepSkyBlue,
                StrokeThickness = 2,
                StrokeStartLineCap = PenLineCap.Round,
                StrokeEndLineCap = PenLineCap.Round
            });
        }

        for (var i = 0; i < frame.Landmarks.Count; i++)
        {
            var point = Map(frame.Landmarks[i]);
            var radius = i == 0 ? 4.2 : 3.2;
            var dot = new Ellipse
            {
                Width = radius * 2,
                Height = radius * 2,
                Fill = i == 0 ? Brushes.Gold : Brushes.White,
                Stroke = Brushes.Black,
                StrokeThickness = 0.6
            };
            Canvas.SetLeft(dot, point.X - radius);
            Canvas.SetTop(dot, point.Y - radius);
            GesturePlaybackCanvas.Children.Add(dot);
        }
    }

    private void UpdateDebugLab(GestureDebugFrame frame)
    {
        var snapshot = frame.Snapshot;
        TrackClassifierLiveLog(snapshot);

        if (PauseDebugCheckBox.IsChecked == true || !_debugLiveFollow)
        {
            UpdateDebugSlider(frame);
            return;
        }

        TrackDebugTimelinePoint(snapshot);
        RenderDebugFrame(frame);
    }

    private void SaveDebugFrame(GestureDebugFrame frame)
    {
        try
        {
            GestureDebugSession.AppendJsonLine(_debugTimelineLogPath, frame);
        }
        catch (Exception ex)
        {
            Log("Gesture debug timeline write failed: " + ex.Message);
        }
    }

    private static GestureRecognitionDebugSnapshot CreateTrackingOnlyDebugSnapshot(
        GestureFrameSample sample,
        string triggerState)
    {
        return new GestureRecognitionDebugSnapshot(
            Timestamp: sample.Timestamp,
            BufferFrameCount: 1,
            UsableFrameCount: sample.Landmarks.Count >= 21 && sample.Confidence >= 0.35f ? 1 : 0,
            BufferDurationMilliseconds: 0,
            BufferEffectiveFps: 0,
            UsableEffectiveFps: 0,
            CandidateTemplate: null,
            CandidateFailureReason: triggerState,
            Scores: [],
            BestGestureId: null,
            BestDisplayName: null,
            BestConfidence: null,
            BestTemplate: null,
            MatchThreshold: 0.72f,
            triggerState,
            ConfirmedMatch: null,
            WindowFrames:
            [
                new GestureDebugWindowFrame(
                    Index: 0,
                    TimeOffsetMilliseconds: 0,
                    AgeMilliseconds: 0,
                    Confidence: sample.Confidence,
                    IsUsable: sample.Landmarks.Count >= 21 && sample.Confidence >= 0.35f,
                    IsAcceptedMatch: false)
            ]);
    }

    private void RenderDebugFrame(GestureDebugFrame frame)
    {
        UpdateSkeletonDebuggers(frame);

        var snapshot = frame.Snapshot;
        UpdateDebugSlider(frame);
        DebugStateText.Text = _debugLiveFollow && PauseDebugCheckBox.IsChecked != true
            ? "Live follow"
            : "Scrubbed frame";
        DebugRecordText.Text =
            $"Frames {_debugSession.Frames.Count}  dropped {_debugSession.DroppedFrameCount}  selected #{frame.SequenceNumber}";
        DebugSelectionText.Text = frame.Kind == GestureDebugFrameKind.NoHand
            ? $"{frame.Timestamp:HH:mm:ss.fff} no hand: {frame.NoHandReason}"
            : $"{frame.Timestamp:HH:mm:ss.fff} {snapshot.TriggerState}  conf={frame.Sample?.Confidence.ToString("0.00") ?? "-"}  action={frame.ActionState}";

        DebugScoresList.Items.Clear();
        DebugScoresList.Items.Add(
            $"{frame.Timestamp:HH:mm:ss.fff} {frame.Kind} source={frame.Source} {snapshot.TriggerState}");
        DebugScoresList.Items.Add(
            $"path={snapshot.DetectedPath} motion={snapshot.MotionScore:0.000} frames={snapshot.UsableFrameCount}/{snapshot.BufferFrameCount} fps={snapshot.UsableEffectiveFps:0.0}");
        DebugScoresList.Items.Add(
            $"candidate={(snapshot.CandidateTemplate is null ? "none" : "unified")} best={snapshot.BestDisplayName ?? "-"} conf={snapshot.BestConfidence?.ToString("0.00") ?? "-"} action={frame.ActionState}");
        if (snapshot.CandidateTemplate?.Topology is { } topology)
        {
            DebugScoresList.Items.Add(
                $"topology change={topology.TopologyChangeScore:0.000} travel={topology.PalmTravel:0.000} angle={topology.PalmOrientationRangeRadians * 180 / MathF.PI:0.0}deg handed={topology.HandednessRange:0.000}");
            DebugScoresList.Items.Add(
                $"palm turn={topology.PalmTurnScore:0.000} areaRange={topology.SignedPalmAreaRange:0.000} crossings={topology.SignedPalmAreaSignChanges} compressionDrop={topology.PalmCompressionDrop:0.000} depthMax={topology.PalmDepthRangeMax:0.000} fingerTransitions={topology.FingerStateTransitionCount} curlRange={topology.FingerStraightnessRangeMax:0.000}");
            DebugScoresList.Items.Add(
                $"hand side mean={topology.HandednessMean:0.000} sizeRange={topology.HandScaleRatioRange:0.000} sizeDelta={topology.HandScaleRatioDelta:0.000} bboxAreaDelta={topology.BoundingBoxAreaRatioDelta:0.000} translation=({topology.TranslationDeltaX:0.000},{topology.TranslationDeltaY:0.000}) dist={topology.TranslationDistance:0.000}");
        }
        if (snapshot.ScoreBreakdown is { } breakdown)
        {
            DebugScoresList.Items.Add(
                $"score parts joint={breakdown.JointScore:0.000} bone={breakdown.BoneScore:0.000} curl={breakdown.CurlScore:0.000} finger={breakdown.FingerStateScore:0.000} spacing={breakdown.SpacingScore:0.000} motion={breakdown.MotionScore:0.000} palm={breakdown.PalmTurnScore:0.000} depth={breakdown.DepthScore:0.000} hand={breakdown.HandednessScore:0.000} size={breakdown.SizeScore:0.000} trans={breakdown.TranslationScore:0.000}");
        }
        if (frame.Sample?.Landmarks is { Count: >= 21 } landmarks)
        {
            var fingerPose = GestureFeatureExtractor.AnalyzeFingerPose(landmarks);
            DebugScoresList.Items.Add(
                $"selected fingers={GestureFeatureExtractor.FormatFingerMask(fingerPose.StateMask)} straight={FormatFingerStraightness(fingerPose.Straightness)}");
        }
        if (!string.IsNullOrWhiteSpace(snapshot.CandidateFailureReason))
        {
            DebugScoresList.Items.Add("candidate failure: " + snapshot.CandidateFailureReason);
        }
        if (!string.IsNullOrWhiteSpace(snapshot.RejectionReason))
        {
            DebugScoresList.Items.Add("rejection: " + snapshot.RejectionReason);
        }
        if (!string.IsNullOrWhiteSpace(frame.NoHandReason))
        {
            DebugScoresList.Items.Add("no hand: " + frame.NoHandReason);
        }
        if (snapshot.ConfirmedMatch is not null)
        {
            DebugScoresList.Items.Add(
                $"CONFIRMED {snapshot.ConfirmedMatch.DisplayName} conf={snapshot.ConfirmedMatch.Confidence:0.00}");
        }

        if (snapshot.Scores.Count == 0)
        {
            DebugScoresList.Items.Add(string.IsNullOrWhiteSpace(snapshot.CandidateFailureReason)
                ? "No custom templates scored."
                : snapshot.CandidateFailureReason);
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
                var rawScore = FormatDebugScore(score.Score);
                var warp = score.WarpRatio is null ? string.Empty : $" warp={score.WarpRatio:0.00}";
                var parts = score.Breakdown is null
                    ? string.Empty
                    : $" parts=j{score.Breakdown.JointScore:0.00}/b{score.Breakdown.BoneScore:0.00}/c{score.Breakdown.CurlScore:0.00}/f{score.Breakdown.FingerStateScore:0.00}/s{score.Breakdown.SpacingScore:0.00}/m{score.Breakdown.MotionScore:0.00}/p{score.Breakdown.PalmTurnScore:0.00}/z{score.Breakdown.DepthScore:0.00}/h{score.Breakdown.HandednessScore:0.00}/sz{score.Breakdown.SizeScore:0.00}/t{score.Breakdown.TranslationScore:0.00}";
                DebugScoresList.Items.Add(
                    TrimDebugLine($"{marker}{score.DisplayName} unified t{template} conf={score.Confidence:0.00} score={rawScore} threshold={snapshot.MatchThreshold:0.00}{warp}{parts} {score.Reason}", 112));
            }
        }

        DrawDebugSessionTimeline(frame);
        DrawSelectedRawSkeleton(frame);
        DrawRollingWindowFrames(DebugMatchCanvas, snapshot, "selected frame rolling 2s input window");
        UpdateDebugVectors(snapshot);
        DrawDebugPaths(snapshot);
    }

    private void UpdateDebugSlider(GestureDebugFrame? selected)
    {
        _isUpdatingDebugSlider = true;
        try
        {
            var frameCount = _debugSession.Frames.Count;
            DebugFrameSlider.Maximum = Math.Max(0, frameCount - 1);
            DebugFrameSlider.IsEnabled = frameCount > 0;
            if (selected is null)
            {
                DebugFrameSlider.Value = 0;
                return;
            }

            var index = _debugSession.Frames
                .Select((frame, frameIndex) => (frame, frameIndex))
                .FirstOrDefault(x => x.frame.SequenceNumber == selected.SequenceNumber)
                .frameIndex;
        DebugFrameSlider.Value = Math.Clamp(index, 0, Math.Max(0, frameCount - 1));
        }
        finally
        {
            _isUpdatingDebugSlider = false;
        }
    }

    private void UpdateSkeletonDebuggers(GestureDebugFrame frame)
    {
        SkeletonVideoDebugView.UpdateSession(
            _debugSession.Frames,
            frame,
            _debugLiveFollow,
            _debugSession.DroppedFrameCount,
            _debugTimelineLogPath);
        _skeletonDebugWindow?.UpdateSession(
            _debugSession.Frames,
            frame,
            _debugLiveFollow,
            _debugSession.DroppedFrameCount,
            _debugTimelineLogPath);
        SkeletonDebuggerStatusText.Text =
            $"Saved {_debugSession.Frames.Count} in-memory frames; writing every frame to {System.IO.Path.GetFileName(_debugTimelineLogPath)}";
    }

    private void AppendDebugCascadeLine(string line)
    {
        DebugScoresList.Items.Insert(0, line);
        while (DebugScoresList.Items.Count > MaxUiLogLines)
        {
            DebugScoresList.Items.RemoveAt(DebugScoresList.Items.Count - 1);
        }
    }

    private void TrackClassifierLiveLog(GestureRecognitionDebugSnapshot snapshot)
    {
        if (snapshot.Timestamp == DateTimeOffset.MinValue)
        {
            return;
        }

        var closest = FindClosestScore(snapshot);
        WriteClassifierFrameLog(snapshot, closest);
        Log(FormatClassifierLiveLog(snapshot, closest));
    }

    private void WriteClassifierFrameLog(
        GestureRecognitionDebugSnapshot snapshot,
        GestureTemplateScore? closest)
    {
        var record = new
        {
            Timestamp = snapshot.Timestamp,
            Snapshot = new
            {
                snapshot.TriggerState,
                Path = snapshot.DetectedPath.ToString(),
                snapshot.MotionScore,
                snapshot.BufferFrameCount,
                snapshot.UsableFrameCount,
                snapshot.BufferDurationMilliseconds,
                snapshot.BufferEffectiveFps,
                snapshot.UsableEffectiveFps,
                snapshot.CandidateFailureReason,
                snapshot.RejectionReason,
                snapshot.MatchThreshold,
                snapshot.BestGestureId,
                snapshot.BestDisplayName,
                snapshot.BestConfidence,
                snapshot.StaticPoseScore,
                snapshot.DtwScore,
                snapshot.DtwWarpRatio,
                snapshot.ScoreBreakdown,
                snapshot.ActiveSegmentStartMilliseconds,
                snapshot.ActiveSegmentEndMilliseconds
            },
            Confirmed = snapshot.ConfirmedMatch,
            Definitions = new
            {
                Count = _gestureDefinitions.Count,
                Ids = _gestureDefinitions.Select(x => x.Id).ToList()
            },
            ClosestScore = closest is null ? null : CreateScoreLogRecord(closest),
            Scores = snapshot.Scores.Select(CreateScoreLogRecord).ToList(),
            CandidateTemplate = CreateTemplateLogRecord(snapshot.CandidateTemplate),
            BestTemplate = CreateTemplateLogRecord(snapshot.BestTemplate),
            DtwPath = snapshot.DtwPath,
            WindowFrames = snapshot.WindowFrames
        };

        try
        {
            System.IO.File.AppendAllText(
                _classifierFrameLogPath,
                JsonSerializer.Serialize(record) + Environment.NewLine);
        }
        catch
        {
        }
    }

    private static object CreateScoreLogRecord(GestureTemplateScore score)
    {
        return new
        {
            score.GestureId,
            score.DisplayName,
            score.TemplateIndex,
            Kind = score.Kind.ToString(),
            score.Score,
            score.Confidence,
            score.IsEligible,
            score.IsBest,
            score.Reason,
            score.WarpRatio,
            score.Breakdown
        };
    }

    private static object? CreateTemplateLogRecord(GestureTemplate? template)
    {
        if (template is null)
        {
            return null;
        }

        return new
        {
            Kind = template.Kind.ToString(),
            template.SourceFrameCount,
            template.SourceSkeletonFrameCount,
            template.DurationMilliseconds,
            template.AverageConfidence,
            Motion = template.MotionSummary,
            ActiveSegment = template.ActiveSegment,
            Topology = template.Topology,
            template.SourceRecordingId,
            SampleCount = template.Samples.Count,
            FeatureFrameCount = template.FeatureFrames?.Count ?? 0,
            SkeletonFrameCount = template.SkeletonFrames?.Count ?? 0,
            Samples = template.Samples.Select(x => new
            {
                x.TimeOffsetMilliseconds,
                x.CenterX,
                x.CenterY,
                x.Values
            }).ToList(),
            FeatureFrames = template.FeatureFrames?.Select(x => new
            {
                x.TimeOffsetMilliseconds,
                x.CenterX,
                x.CenterY,
                x.PalmOrientationRadians,
                x.PalmVelocity,
                x.Values
            }).ToList(),
            SkeletonFrames = template.SkeletonFrames?.Select(x => new
            {
                x.TimeOffsetMilliseconds,
                x.Landmarks
            }).ToList()
        };
    }

    private static string FormatClassifierLiveLog(
        GestureRecognitionDebugSnapshot snapshot,
        GestureTemplateScore? closest)
    {
        var match = snapshot.ConfirmedMatch is null
            ? snapshot.BestDisplayName is null
                ? "match=-"
                : $"best={snapshot.BestDisplayName} conf={snapshot.BestConfidence:0.00}"
            : $"CONFIRMED {snapshot.ConfirmedMatch.DisplayName} conf={snapshot.ConfirmedMatch.Confidence:0.00}";
        var closestText = closest is null
            ? "closest=-"
            : $"closest={closest.DisplayName} unified t{closest.TemplateIndex} conf={closest.Confidence:0.00} score={FormatDebugScore(closest.Score)} {closest.Reason}";
        var breakdown = snapshot.ScoreBreakdown is null
            ? string.Empty
            : $" breakdown=j{snapshot.ScoreBreakdown.JointScore:0.000}/b{snapshot.ScoreBreakdown.BoneScore:0.000}/c{snapshot.ScoreBreakdown.CurlScore:0.000}/f{snapshot.ScoreBreakdown.FingerStateScore:0.000}/s{snapshot.ScoreBreakdown.SpacingScore:0.000}/m{snapshot.ScoreBreakdown.MotionScore:0.000}/p{snapshot.ScoreBreakdown.PalmTurnScore:0.000}/z{snapshot.ScoreBreakdown.DepthScore:0.000}/h{snapshot.ScoreBreakdown.HandednessScore:0.000}/sz{snapshot.ScoreBreakdown.SizeScore:0.000}/t{snapshot.ScoreBreakdown.TranslationScore:0.000}";
        var activeSegment = snapshot.ActiveSegmentStartMilliseconds is null
            ? "active=-"
            : $"active={snapshot.ActiveSegmentStartMilliseconds:0}-{snapshot.ActiveSegmentEndMilliseconds:0}ms";
        var rejection = string.IsNullOrWhiteSpace(snapshot.RejectionReason)
            ? string.Empty
            : $" reject=\"{snapshot.RejectionReason}\"";

        return
            $"Classifier: {snapshot.TriggerState}; {match}; path={snapshot.DetectedPath}; motion={snapshot.MotionScore:0.000}; " +
            $"{activeSegment}; frames={snapshot.UsableFrameCount}/{snapshot.BufferFrameCount}; fps={snapshot.UsableEffectiveFps:0.0}; defs={snapshot.Scores.Select(x => x.GestureId).Distinct().Count()}; {closestText}{breakdown}{rejection}";
    }

    private void TrackDebugTimelinePoint(GestureRecognitionDebugSnapshot snapshot)
    {
        if (snapshot.Timestamp == DateTimeOffset.MinValue)
        {
            return;
        }

        _debugTimelineHistory.Add(new DebugTimelinePoint(
            snapshot.Timestamp,
            snapshot.BufferFrameCount,
            snapshot.UsableFrameCount,
            snapshot.BufferDurationMilliseconds,
            snapshot.BufferEffectiveFps,
            snapshot.UsableEffectiveFps,
            snapshot.BestDisplayName,
            snapshot.BestConfidence,
            snapshot.DetectedPath,
            snapshot.MotionScore,
            snapshot.TriggerState,
            snapshot.ConfirmedMatch));

        var cutoff = snapshot.Timestamp - DebugTimelineHistoryDuration;
        _debugTimelineHistory.RemoveAll(x => x.Timestamp < cutoff);
    }

    private void DrawDebugSessionTimeline(GestureDebugFrame selectedFrame)
    {
        DebugTimelineCanvas.Children.Clear();
        var frames = _debugSession.Frames;
        var width = Math.Max(1, DebugTimelineCanvas.ActualWidth > 1 ? DebugTimelineCanvas.ActualWidth : 400);
        var height = Math.Max(1, DebugTimelineCanvas.ActualHeight > 1 ? DebugTimelineCanvas.ActualHeight : 66);
        var pad = 10d;
        var top = 24d;
        var usableWidth = Math.Max(1, width - (pad * 2));
        var barHeight = Math.Max(12, height - 42);

        DebugTimelineCanvas.Children.Add(new TextBlock
        {
            Text = $"session timeline | {frames.Count} frames | selected #{selectedFrame.SequenceNumber}",
            Foreground = Brushes.LightGray,
            FontSize = 12,
            Margin = new Thickness(8, 4, 0, 0)
        });

        if (frames.Count == 0)
        {
            return;
        }

        var background = new Rectangle
        {
            Width = usableWidth,
            Height = barHeight,
            Fill = new SolidColorBrush(Color.FromRgb(26, 31, 39)),
            Stroke = new SolidColorBrush(Color.FromRgb(51, 58, 72)),
            StrokeThickness = 1
        };
        Canvas.SetLeft(background, pad);
        Canvas.SetTop(background, top);
        DebugTimelineCanvas.Children.Add(background);

        var first = frames[0].Timestamp;
        var last = frames[^1].Timestamp;
        var duration = Math.Max(1, (last - first).TotalMilliseconds);
        var frameWidth = Math.Max(2, usableWidth / Math.Max(1, frames.Count));
        foreach (var frame in frames)
        {
            var x = pad + Math.Clamp((frame.Timestamp - first).TotalMilliseconds / duration, 0, 1)
                * Math.Max(1, usableWidth - frameWidth);
            var rect = new Rectangle
            {
                Width = Math.Max(1.5, frameWidth - 1),
                Height = frame.SequenceNumber == selectedFrame.SequenceNumber ? barHeight : Math.Max(5, barHeight * 0.72),
                Fill = DebugFrameBrush(frame),
                Opacity = frame.SequenceNumber == selectedFrame.SequenceNumber ? 1.0 : 0.82
            };
            Canvas.SetLeft(rect, x);
            Canvas.SetTop(rect, top + barHeight - rect.Height);
            DebugTimelineCanvas.Children.Add(rect);
        }
    }

    private void DrawSelectedRawSkeleton(GestureDebugFrame frame)
    {
        DebugSkeletonCanvas.Children.Clear();
        var hands = HandsForDebugFrame(frame).ToList();
        if (hands.Count == 0)
        {
            DebugSkeletonCanvas.Children.Add(new TextBlock
            {
                Text = string.IsNullOrWhiteSpace(frame.NoHandReason)
                    ? "No hand skeleton for selected frame"
                    : frame.NoHandReason,
                Foreground = Brushes.Gray,
                Margin = new Thickness(10)
            });
            return;
        }

        var width = Math.Max(1, DebugSkeletonCanvas.ActualWidth > 1 ? DebugSkeletonCanvas.ActualWidth : 400);
        var height = Math.Max(1, DebugSkeletonCanvas.ActualHeight > 1 ? DebugSkeletonCanvas.ActualHeight : 114);
        var pad = 12d;
        var allLandmarks = DebugWindowLandmarks(frame).ToList();
        if (allLandmarks.Count == 0)
        {
            allLandmarks = hands.SelectMany(x => x.Landmarks).ToList();
        }
        var useCameraSpace = LooksLikeNormalizedCameraSpace(allLandmarks);
        var minX = useCameraSpace ? 0 : allLandmarks.Min(x => x.X);
        var maxX = useCameraSpace ? 1 : allLandmarks.Max(x => x.X);
        var minY = useCameraSpace ? 0 : allLandmarks.Min(x => x.Y);
        var maxY = useCameraSpace ? 1 : allLandmarks.Max(x => x.Y);
        var xSpan = Math.Max(0.001f, maxX - minX);
        var ySpan = Math.Max(0.001f, maxY - minY);

        Point Map(HandLandmark point)
        {
            var x = width - pad - ((point.X - minX) / xSpan * Math.Max(1, width - (pad * 2)));
            var y = pad + ((point.Y - minY) / ySpan * Math.Max(1, height - (pad * 2)));
            return new Point(x, y);
        }

        for (var handIndex = 0; handIndex < hands.Count; handIndex++)
        {
            var landmarks = hands[handIndex].Landmarks;
            var brush = handIndex == 0 ? Brushes.Gold : Brushes.DeepSkyBlue;
            foreach (var (start, end) in HandConnections)
            {
                var a = Map(landmarks[start]);
                var b = Map(landmarks[end]);
                DebugSkeletonCanvas.Children.Add(new Line
                {
                    X1 = a.X,
                    Y1 = a.Y,
                    X2 = b.X,
                    Y2 = b.Y,
                    Stroke = brush,
                    StrokeThickness = 1.6,
                    StrokeStartLineCap = PenLineCap.Round,
                    StrokeEndLineCap = PenLineCap.Round
                });
            }

            for (var i = 0; i < Math.Min(21, landmarks.Count); i++)
            {
                var point = Map(landmarks[i]);
                var radius = i == 0 ? 4.4 : 3.2;
                var dot = new Ellipse
                {
                    Width = radius * 2,
                    Height = radius * 2,
                    Fill = i == 0 ? brush : Brushes.White,
                    Stroke = Brushes.Black,
                    StrokeThickness = 0.6
                };
                Canvas.SetLeft(dot, point.X - radius);
                Canvas.SetTop(dot, point.Y - radius);
                DebugSkeletonCanvas.Children.Add(dot);
            }
        }

        DebugSkeletonCanvas.Children.Add(new TextBlock
        {
            Text = $"raw skeleton | hands={hands.Count} | conf={frame.Sample?.Confidence.ToString("0.00") ?? "-"} | handedness={frame.Sample?.Handedness.ToString("0.00") ?? "-"}",
            Foreground = Brushes.LightGray,
            FontSize = 12,
            Margin = new Thickness(8, 4, 0, 0)
        });
    }

    private static IEnumerable<GestureFrameSample> HandsForDebugFrame(GestureDebugFrame frame)
    {
        if (frame.FrameSet?.Hands is { Count: > 0 } hands)
        {
            return hands.Where(x => x.Landmarks.Count >= 21);
        }

        return frame.Sample?.Landmarks is { Count: >= 21 } ? [frame.Sample] : [];
    }

    private static IEnumerable<HandLandmark> DebugWindowLandmarks(GestureDebugFrame frame)
    {
        if (frame.Snapshot.CandidateTemplate?.SkeletonFrames is { Count: > 0 } candidateFrames)
        {
            return candidateFrames
                .SelectMany(x => x.Landmarks)
                .Select(x => new HandLandmark(x.X, x.Y, x.Z));
        }

        return frame.FrameSet?.Hands is { Count: > 0 } hands
            ? hands.SelectMany(x => x.Landmarks)
            : [];
    }

    private static bool LooksLikeNormalizedCameraSpace(IReadOnlyList<HandLandmark> landmarks)
    {
        return landmarks.Count > 0
            && landmarks.All(point =>
                point.X >= -0.05f && point.X <= 1.05f
                && point.Y >= -0.05f && point.Y <= 1.05f);
    }

    private static Brush DebugFrameBrush(GestureDebugFrame frame)
    {
        if (frame.Kind == GestureDebugFrameKind.NoHand)
        {
            return Brushes.DimGray;
        }
        if (frame.Snapshot.ConfirmedMatch is not null)
        {
            return Brushes.Lime;
        }
        if (frame.Recognition is not null)
        {
            return Brushes.Gold;
        }
        if (frame.Snapshot.CandidateTemplate is null)
        {
            return Brushes.SteelBlue;
        }
        if (!string.IsNullOrWhiteSpace(frame.Snapshot.RejectionReason)
            || frame.ClosestScore is { IsEligible: false })
        {
            return Brushes.OrangeRed;
        }

        return Brushes.DeepSkyBlue;
    }

    private static void DrawRollingWindowFrames(
        Canvas canvas,
        GestureRecognitionDebugSnapshot snapshot,
        string label)
    {
        canvas.Children.Clear();
        var frames = snapshot.WindowFrames ?? [];
        var width = Math.Max(1, canvas.ActualWidth > 1 ? canvas.ActualWidth : 400);
        var height = Math.Max(1, canvas.ActualHeight > 1 ? canvas.ActualHeight : 66);
        var pad = 10d;
        var top = 22d;
        var usableWidth = Math.Max(1, width - (pad * 2));
        var barHeight = Math.Max(8, height - 38);

        var background = new Rectangle
        {
            Width = usableWidth,
            Height = barHeight,
            Fill = new SolidColorBrush(Color.FromRgb(26, 31, 39)),
            Stroke = new SolidColorBrush(Color.FromRgb(51, 58, 72)),
            StrokeThickness = 1
        };
        Canvas.SetLeft(background, pad);
        Canvas.SetTop(background, top);
        canvas.Children.Add(background);

        if (frames.Count == 0)
        {
            canvas.Children.Add(new TextBlock
            {
                Text = "no frames in rolling window",
                Foreground = Brushes.Gray,
                FontSize = 12,
                Margin = new Thickness(10, 4, 0, 0)
            });
            return;
        }

        var maxTime = Math.Max(1, frames.Max(x => x.TimeOffsetMilliseconds));
        var frameWidth = Math.Max(2, usableWidth / Math.Max(1, frames.Count));
        foreach (var frame in frames)
        {
            var x = pad + ((frame.TimeOffsetMilliseconds / maxTime) * Math.Max(1, usableWidth - frameWidth));
            var confidenceHeight = Math.Clamp(frame.Confidence, 0, 1) * barHeight;
            var fill = frame.IsAcceptedMatch
                ? Brushes.Lime
                : frame.IsUsable
                    ? Brushes.DeepSkyBlue
                    : Brushes.DimGray;
            var rect = new Rectangle
            {
                Width = Math.Max(1.5, frameWidth - 1),
                Height = Math.Max(3, confidenceHeight),
                Fill = fill,
                Opacity = frame.IsAcceptedMatch ? 1 : 0.82
            };
            Canvas.SetLeft(rect, x);
            Canvas.SetTop(rect, top + barHeight - rect.Height);
            canvas.Children.Add(rect);
        }

        var status = snapshot.ConfirmedMatch is null
            ? $"accepted match frames: 0"
            : $"accepted match frames: {frames.Count(x => x.IsAcceptedMatch)} ({snapshot.ConfirmedMatch.DisplayName} {snapshot.ConfirmedMatch.Confidence:0.00})";
        canvas.Children.Add(new TextBlock
        {
            Text = $"{label} | frames {snapshot.UsableFrameCount}/{snapshot.BufferFrameCount} | fps {snapshot.UsableEffectiveFps:0.0} | {status}",
            Foreground = snapshot.ConfirmedMatch is null ? Brushes.LightGray : Brushes.Lime,
            FontSize = 12,
            Margin = new Thickness(8, 4, 0, 0)
        });
    }

    private static double MapDebugTimelineTime(
        DateTimeOffset timestamp,
        DateTimeOffset start,
        DateTimeOffset end,
        double left,
        double width)
    {
        var duration = Math.Max(1, (end - start).TotalMilliseconds);
        var offset = Math.Clamp((timestamp - start).TotalMilliseconds / duration, 0, 1);
        return left + (offset * width);
    }

    private void DrawDebugSkeletonComparison(GestureRecognitionDebugSnapshot snapshot)
    {
        DebugSkeletonCanvas.Children.Clear();
        var candidateFrames = snapshot.CandidateTemplate?.SkeletonFrames;
        var templateFrames = snapshot.BestTemplate?.SkeletonFrames;
        if (candidateFrames is not { Count: > 0 } && templateFrames is not { Count: > 0 })
        {
            DebugSkeletonCanvas.Children.Add(new TextBlock
            {
                Text = string.IsNullOrWhiteSpace(snapshot.CandidateFailureReason)
                    ? "Waiting for candidate skeleton"
                    : snapshot.CandidateFailureReason,
                Foreground = Brushes.Gray,
                Margin = new Thickness(10)
            });
            return;
        }

        var width = Math.Max(1, DebugSkeletonCanvas.ActualWidth > 1 ? DebugSkeletonCanvas.ActualWidth : 400);
        var height = Math.Max(1, DebugSkeletonCanvas.ActualHeight > 1 ? DebugSkeletonCanvas.ActualHeight : 114);
        var halfWidth = width / 2;
        if (candidateFrames is { Count: > 0 })
        {
            DrawSkeletonStrip(DebugSkeletonCanvas, candidateFrames, 0, halfWidth, height, Brushes.Gold);
        }

        if (templateFrames is { Count: > 0 })
        {
            DrawSkeletonStrip(DebugSkeletonCanvas, templateFrames, halfWidth, halfWidth, height, Brushes.DeepSkyBlue);
        }

        DebugSkeletonCanvas.Children.Add(new Line
        {
            X1 = halfWidth,
            X2 = halfWidth,
            Y1 = 8,
            Y2 = height - 8,
            Stroke = new SolidColorBrush(Color.FromRgb(43, 49, 61)),
            StrokeThickness = 1
        });
    }

    private static void DrawSkeletonStrip(
        Canvas canvas,
        IReadOnlyList<GestureSkeletonFrame> frames,
        double left,
        double width,
        double height,
        Brush stroke)
    {
        var selectedFrames = SelectSkeletonStripFrames(frames, 4);
        if (selectedFrames.Count == 0)
        {
            return;
        }

        var cellWidth = Math.Max(1, width / selectedFrames.Count);
        for (var i = 0; i < selectedFrames.Count; i++)
        {
            DrawMiniSkeleton(canvas, selectedFrames[i], left + (i * cellWidth), 0, cellWidth, height, stroke);
        }
    }

    private static List<GestureSkeletonFrame> SelectSkeletonStripFrames(
        IReadOnlyList<GestureSkeletonFrame> frames,
        int count)
    {
        if (frames.Count == 0 || count <= 0)
        {
            return [];
        }

        if (frames.Count <= count)
        {
            return frames.ToList();
        }

        var result = new List<GestureSkeletonFrame>(count);
        for (var i = 0; i < count; i++)
        {
            var index = (int)Math.Round(i * (frames.Count - 1) / (double)(count - 1));
            result.Add(frames[index]);
        }

        return result;
    }

    private static void DrawMiniSkeleton(
        Canvas canvas,
        GestureSkeletonFrame frame,
        double left,
        double top,
        double width,
        double height,
        Brush stroke)
    {
        if (frame.Landmarks.Count < 21)
        {
            return;
        }

        var pad = 8d;
        var minX = frame.Landmarks.Min(x => x.X);
        var maxX = frame.Landmarks.Max(x => x.X);
        var minY = frame.Landmarks.Min(x => x.Y);
        var maxY = frame.Landmarks.Max(x => x.Y);
        var xSpan = Math.Max(0.001f, maxX - minX);
        var ySpan = Math.Max(0.001f, maxY - minY);

        Point Map(GestureSkeletonPoint point)
        {
            var x = left + width - pad - ((point.X - minX) / xSpan * Math.Max(1, width - (pad * 2)));
            var y = top + pad + ((point.Y - minY) / ySpan * Math.Max(1, height - (pad * 2)));
            return new Point(x, y);
        }

        foreach (var (start, end) in HandConnections)
        {
            var a = Map(frame.Landmarks[start]);
            var b = Map(frame.Landmarks[end]);
            canvas.Children.Add(new Line
            {
                X1 = a.X,
                Y1 = a.Y,
                X2 = b.X,
                Y2 = b.Y,
                Stroke = stroke,
                StrokeThickness = 1.2,
                StrokeStartLineCap = PenLineCap.Round,
                StrokeEndLineCap = PenLineCap.Round,
                Opacity = 0.9
            });
        }

        var wrist = Map(frame.Landmarks[0]);
        var dot = new Ellipse
        {
            Width = 4,
            Height = 4,
            Fill = Brushes.White,
            Opacity = 0.9
        };
        Canvas.SetLeft(dot, wrist.X - 2);
        Canvas.SetTop(dot, wrist.Y - 2);
        canvas.Children.Add(dot);
    }

    private void DrawDebugMatchAlignment(GestureRecognitionDebugSnapshot snapshot)
    {
        DebugMatchCanvas.Children.Clear();
        var candidate = snapshot.CandidateTemplate?.Samples;
        var template = snapshot.BestTemplate?.Samples ?? FindBestTemplate(snapshot);
        if (candidate is not { Count: > 0 } || template is not { Count: > 0 })
        {
            DebugMatchCanvas.Children.Add(new TextBlock
            {
                Text = string.IsNullOrWhiteSpace(snapshot.CandidateFailureReason)
                    ? "Waiting for a candidate/template comparison"
                    : snapshot.CandidateFailureReason,
                Foreground = Brushes.Gray,
                Margin = new Thickness(10)
            });
            return;
        }

        var count = Math.Min(candidate.Count, template.Count);
        if (count == 0)
        {
            return;
        }

        var distances = new float[count];
        var sum = 0f;
        var worst = 0f;
        var worstIndex = 0;
        for (var i = 0; i < count; i++)
        {
            var distance = SampleDistance(candidate[i], template[i]);
            distances[i] = distance;
            sum += distance;
            if (distance > worst)
            {
                worst = distance;
                worstIndex = i;
            }
        }

        var average = sum / count;
        var closestScore = FindClosestScore(snapshot);
        var confidence = closestScore?.Confidence ?? Math.Clamp(1f - (average / 0.9f), 0, 1);
        var width = Math.Max(1, DebugMatchCanvas.ActualWidth > 1 ? DebugMatchCanvas.ActualWidth : 400);
        var height = Math.Max(1, DebugMatchCanvas.ActualHeight > 1 ? DebugMatchCanvas.ActualHeight : 88);
        var pad = 14d;
        var usableWidth = Math.Max(1, width - (pad * 2));
        var topY = 24d;
        var bottomY = 52d;
        var barBaseY = height - 8;
        var maxDistance = Math.Max(0.35f, worst);

        DebugMatchCanvas.Children.Add(new Line
        {
            X1 = pad,
            X2 = pad + usableWidth,
            Y1 = topY,
            Y2 = topY,
            Stroke = new SolidColorBrush(Color.FromRgb(70, 74, 85)),
            StrokeThickness = 1
        });
        DebugMatchCanvas.Children.Add(new Line
        {
            X1 = pad,
            X2 = pad + usableWidth,
            Y1 = bottomY,
            Y2 = bottomY,
            Stroke = new SolidColorBrush(Color.FromRgb(70, 74, 85)),
            StrokeThickness = 1
        });

        for (var i = 0; i < count; i++)
        {
            var x = pad + (count <= 1 ? 0 : usableWidth * i / (count - 1));
            var brush = DistanceBrush(distances[i]);
            DebugMatchCanvas.Children.Add(new Line
            {
                X1 = x,
                X2 = x,
                Y1 = topY,
                Y2 = bottomY,
                Stroke = brush,
                StrokeThickness = i == worstIndex ? 2.4 : 1.2,
                Opacity = i == worstIndex ? 1.0 : 0.72
            });

            var radius = i == worstIndex ? 3.2 : 2.2;
            var topDot = new Ellipse
            {
                Width = radius * 2,
                Height = radius * 2,
                Fill = Brushes.Gold,
                Stroke = brush,
                StrokeThickness = 0.8
            };
            Canvas.SetLeft(topDot, x - radius);
            Canvas.SetTop(topDot, topY - radius);
            DebugMatchCanvas.Children.Add(topDot);

            var bottomDot = new Ellipse
            {
                Width = radius * 2,
                Height = radius * 2,
                Fill = Brushes.DeepSkyBlue,
                Stroke = brush,
                StrokeThickness = 0.8
            };
            Canvas.SetLeft(bottomDot, x - radius);
            Canvas.SetTop(bottomDot, bottomY - radius);
            DebugMatchCanvas.Children.Add(bottomDot);

            var barHeight = Math.Clamp(distances[i] / maxDistance, 0, 1) * 18;
            var bar = new Rectangle
            {
                Width = Math.Max(2, usableWidth / count * 0.62),
                Height = barHeight,
                Fill = brush,
                Opacity = 0.85
            };
            Canvas.SetLeft(bar, x - (bar.Width / 2));
            Canvas.SetTop(bar, barBaseY - barHeight);
            DebugMatchCanvas.Children.Add(bar);
        }

        var label = closestScore is null
            ? $"closest: -  avg d={average:0.000}  worst #{worstIndex + 1}={worst:0.000}"
            : $"closest: {closestScore.DisplayName} t{closestScore.TemplateIndex}  conf={confidence:0.00}/{snapshot.MatchThreshold:0.00}  avg d={average:0.000}  worst #{worstIndex + 1}={worst:0.000}";
        DebugMatchCanvas.Children.Add(new TextBlock
        {
            Text = label,
            Foreground = confidence >= snapshot.MatchThreshold ? Brushes.Lime : Brushes.LightGray,
            FontSize = 12,
            Margin = new Thickness(8, 3, 0, 0)
        });

        DebugMatchCanvas.Children.Add(new TextBlock
        {
            Text = "candidate",
            Foreground = Brushes.Gold,
            FontSize = 10,
            Margin = new Thickness(8, topY - 18, 0, 0)
        });
        DebugMatchCanvas.Children.Add(new TextBlock
        {
            Text = "template",
            Foreground = Brushes.DeepSkyBlue,
            FontSize = 10,
            Margin = new Thickness(8, bottomY - 2, 0, 0)
        });
    }

    private void DrawDebugPaths(GestureRecognitionDebugSnapshot snapshot)
    {
        DebugPathCanvas.Children.Clear();
        var paths = new List<(IReadOnlyList<GestureTemplateSample> Samples, Brush Stroke, double Thickness)>
        {
            (snapshot.CandidateTemplate?.Samples ?? [], Brushes.Gold, 3)
        };

        var bestScore = FindClosestScore(snapshot);
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
            DebugCandidateText.Text = "Raw classifier vectors";
            return;
        }

        DebugCandidateText.Text = "Raw classifier vectors";
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
        if (snapshot.BestTemplate?.Samples is { Count: > 0 } samples)
        {
            return samples;
        }

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

    private static GestureTemplateScore? FindClosestScore(GestureRecognitionDebugSnapshot snapshot)
    {
        return snapshot.Scores
            .Where(x => x.TemplateIndex > 0 && float.IsFinite(x.Score))
            .OrderBy(x => x.Score)
            .FirstOrDefault();
    }

    private static string FormatDebugScore(float score)
    {
        return !float.IsFinite(score) || score > 999
            ? "-"
            : score.ToString("0.000");
    }

    private static string FormatFingerStraightness(IReadOnlyList<float> straightness)
    {
        return string.Join(",", straightness.Select(x => x.ToString("0.00")));
    }

    private static string TrimDebugLine(string value, int maxLength)
    {
        return value.Length <= maxLength
            ? value
            : value[..Math.Max(0, maxLength - 1)] + "...";
    }

    private static Brush DistanceBrush(float distance)
    {
        var normalized = Math.Clamp(distance / 0.42f, 0, 1);
        byte red;
        byte green;
        if (normalized < 0.5f)
        {
            red = (byte)(80 + normalized * 2 * 175);
            green = 220;
        }
        else
        {
            red = 255;
            green = (byte)(220 - ((normalized - 0.5f) * 2 * 150));
        }

        return new SolidColorBrush(Color.FromRgb(red, green, 70));
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
        var x = width - pad - ((point.CenterX - minX) / xSpan * Math.Max(1, width - (pad * 2)));
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

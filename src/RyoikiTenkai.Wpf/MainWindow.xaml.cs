using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows;
using System.Windows.Threading;
using System.Windows.Controls;
using RyoikiTenkai.Wpf.Native;
using RyoikiTenkai.Wpf.Interaction;
using RyoikiTenkai.Actions;
using RyoikiTenkai.Core;
using RyoikiTenkai.Wpf.Interaction.Handoff;

namespace RyoikiTenkai.Wpf;

public partial class MainWindow : Window
{
    private static readonly TimeSpan RecordingCountdown = TimeSpan.FromSeconds(3);
    private static readonly TimeSpan RecordingDuration = TimeSpan.FromSeconds(2.5);
    private readonly string _logPath;
    private readonly string? _nativeMetricsPath;
    private readonly DispatcherTimer _nativePollTimer;
    private readonly DispatcherTimer _recordingDisplayTimer;
    private string? _lastNativeRuntimeError;
    private ulong _lastSampledNativeFrameId;
    private ulong _lastLoggedGestureFrameId;
    private DateTimeOffset _recordingStartedAt;
    private bool _isGestureRecording;
    private bool _nativeRecordingBegan;
    private bool _isRecordingCountdown;
    private bool _awaitingNativeRecordingStart;
    private uint _pendingTemplateId;
    private uint _validationTemplateId;
    private uint _currentRecordingTrial = 1;
    private GestureDtwDebugWindow? _dtwDebugWindow;
    private GestureRecordingPlaybackWindow? _recordingPlaybackWindow;
    private GestureActionWindow? _gestureActionWindow;
    private DateTimeOffset _lastValidationMatchAt;
    private readonly MutableGestureBindingResolver _gestureBindings = new();
    private readonly GestureCatalog _gestureCatalog;
    private readonly NativeGestureEventPump _gestureEventPump;
    private CancellationTokenSource _gestureDispatchCancellation = new();
    private readonly HandoffService _handoffService;
    private HandoffEffectWindow? _handoffEffect;
    private HandoffState _lastHandoffState = HandoffState.Idle;
    private string? _lastTransferKey;
    private string? _lastTransferPath;
    private System.Windows.Media.ImageSource? _lastTransferImage;

    /// <summary>Wide enough to recognize a screenshot, far short of a viewer.</summary>
    private const int TransferPreviewWidth = 480;

    /// <summary>Semantic colours come from Theme.xaml so code-driven states match the XAML.</summary>
    private static System.Windows.Media.Brush ThemeBrush(string key) =>
        (System.Windows.Media.Brush)Application.Current.Resources[key];

    public MainWindow()
    {
        InitializeComponent();
        _logPath = System.IO.Path.Combine(AppContext.BaseDirectory, "ryoikitenkai.log");
        var metricsPath = Environment.GetEnvironmentVariable("RYOIKI_NATIVE_METRICS_CSV");
        _nativeMetricsPath = string.IsNullOrWhiteSpace(metricsPath)
            ? null
            : System.IO.Path.GetFullPath(metricsPath);
        NativeVisionHostControl.DiagnosticLogged += Log;
        _handoffService = new HandoffService(new WpfHandoffPayloadProvider(this), Log);
        _gestureCatalog = new GestureCatalog(NativeVisionHostControl, _gestureBindings);
        _gestureCatalog.Refreshed += (_, _) => GestureCatalogSummaryText.Text = _gestureCatalog.Status;
        _gestureEventPump = new NativeGestureEventPump(
            NativeVisionHostControl,
            new GestureCommandDispatcher(_gestureBindings, new ActionExecutor(_handoffService)),
            Log);
        _nativePollTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(250) };
        _nativePollTimer.Tick += NativePollTimer_Tick;
        _recordingDisplayTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(50) };
        _recordingDisplayTimer.Tick += RecordingDisplayTimer_Tick;
        Log($"Ready. Native runtime is required. Log file: {_logPath}");
    }

    private void StartButton_Click(object sender, RoutedEventArgs e)
    {
        StartButton.IsEnabled = false;
        StopButton.IsEnabled = true;
        SetCameraStatus("Starting camera and hand models…", "Warning");
        NativeVisionHostControl.Visibility = Visibility.Visible;
        PreviewPlaceholder.Visibility = Visibility.Collapsed;
        if (!NativeVisionHostControl.StartNativeRuntime())
        {
            NativeVisionHostControl.Visibility = Visibility.Collapsed;
            PreviewPlaceholder.Visibility = Visibility.Visible;
            SetCameraStatus("Camera could not start", "Danger");
            StartButton.IsEnabled = true;
            StopButton.IsEnabled = false;
            Log("Native runtime is required; no managed inference fallback exists.");
            return;
        }
        _lastNativeRuntimeError = null;
        _lastSampledNativeFrameId = 0;
        _lastLoggedGestureFrameId = 0;
        _gestureDispatchCancellation.Dispose();
        _gestureDispatchCancellation = new CancellationTokenSource();
        _gestureEventPump.Reset();
        InitializeNativeMetricsFile();
        _nativePollTimer.Start();
        SetCameraStatus("Camera running", "Success");
        _gestureCatalog.Refresh();
        if (RegistrationPanel.Visibility == Visibility.Visible)
        {
            StartGestureRecordingButton.IsEnabled = true;
            RecordingInstructionText.Text = "Keep only the hand you want to record in view.";
        }
        Log("Native camera, inference, landmark processing, and rendering started.");
    }

    private void StopButton_Click(object sender, RoutedEventArgs e)
    {
        StopNativeRuntime();
        SetCameraStatus("Camera stopped", "TextMuted");
        StartButton.IsEnabled = true;
        StopButton.IsEnabled = false;
    }

    private void SetCameraStatus(string message, string brushKey)
    {
        StateText.Text = message;
        CameraStatusDot.Fill = ThemeBrush(brushKey);
    }

    private void OpenCadViewerButton_Click(object sender, RoutedEventArgs e)
    {
        var viewer = new CadViewerWindow(NativeVisionHostControl) { Owner = this };
        viewer.Show();
    }

    private void OpenDtwDebuggerButton_Click(object sender, RoutedEventArgs e)
    {
        if (_dtwDebugWindow is { IsVisible: true })
        {
            _dtwDebugWindow.Activate();
            return;
        }
        _dtwDebugWindow = new GestureDtwDebugWindow(NativeVisionHostControl, _gestureCatalog) { Owner = this };
        _dtwDebugWindow.Closed += (_, _) => _dtwDebugWindow = null;
        _dtwDebugWindow.Show();
    }

    private void OpenGestureActionsButton_Click(object sender, RoutedEventArgs e)
    {
        if (_gestureActionWindow is { IsVisible: true })
        {
            _gestureActionWindow.Activate();
            return;
        }
        _gestureCatalog.Refresh();
        _gestureActionWindow = new GestureActionWindow(_gestureCatalog) { Owner = this };
        _gestureActionWindow.ReviewRequested += GestureActionWindow_ReviewRequested;
        _gestureActionWindow.Closed += (_, _) => _gestureActionWindow = null;
        _gestureActionWindow.Show();
    }

    private void GestureActionWindow_ReviewRequested(object? sender, SavedGestureItem item)
    {
        // Playback keeps a non-owning native runtime pointer, so this window
        // owns its lifetime and destroys it before the runtime stops.
        if (_recordingPlaybackWindow is { IsVisible: true })
        {
            _recordingPlaybackWindow.Close();
        }
        _recordingPlaybackWindow = new GestureRecordingPlaybackWindow(
            NativeVisionHostControl, item.Id, item.Name) { Owner = this };
        _recordingPlaybackWindow.Closed += (_, _) => _recordingPlaybackWindow = null;
        _recordingPlaybackWindow.Show();
    }

    private void EnterGestureRegistrationModeButton_Click(object sender, RoutedEventArgs e)
    {
        RegistrationColumn.Width = new GridLength(420);
        RegistrationPanel.Visibility = Visibility.Visible;
        OpenGestureRegistrationButton.IsEnabled = false;
        _gestureCatalog.Refresh();
        ResetRecordingUi(NativeVisionHostControl.IsStarted
            ? "Keep only the hand you want to record in view."
            : "Start the camera before recording a gesture.");
    }

    private void ExitGestureRegistrationModeButton_Click(object sender, RoutedEventArgs e)
    {
        if (_pendingTemplateId != 0) NativeVisionHostControl.CancelGestureRecording();
        _recordingDisplayTimer.Stop();
        _isGestureRecording = false;
        _awaitingNativeRecordingStart = false;
        _pendingTemplateId = 0;
        RegistrationPanel.Visibility = Visibility.Collapsed;
        RegistrationColumn.Width = new GridLength(0);
        OpenGestureRegistrationButton.IsEnabled = true;
    }

    private void BeginExplicitGestureRecordingButton_Click(object sender, RoutedEventArgs e)
    {
        if (!NativeVisionHostControl.IsStarted)
        {
            RecordingQualityText.Text = "Start the camera before recording a gesture.";
            return;
        }
        var name = GestureNameText.Text.Trim();
        if (string.IsNullOrWhiteSpace(name))
        {
            RecordingQualityText.Text = "Enter a name for this gesture.";
            return;
        }
        if (!TryGetSingleVisibleHand(out var message))
        {
            RecordingQualityText.Text = message;
            return;
        }

        var startingNewSession = _pendingTemplateId == 0 || _validationTemplateId != 0;
        if (startingNewSession)
        {
            // Completed and cancelled status snapshots intentionally remain
            // pollable in native code. Explicitly terminate the previous
            // workflow before reusing a stable ID for three fresh takes.
            NativeVisionHostControl.CancelGestureRecording();
            _validationTemplateId = 0;
            _currentRecordingTrial = 1;
            AcceptedTrialsProgress.Value = 0;
            GestureValidationPanel.Visibility = Visibility.Collapsed;
        }
        _pendingTemplateId = CreateStableTemplateId(name);
        if (!_gestureCatalog.PrepareDefinition(_pendingTemplateId, name))
        {
            // The catalog status text now lives in the action window, so the
            // recording panel has to report its own failure.
            RecordingQualityText.Text = _gestureCatalog.Status;
            return;
        }
        _isGestureRecording = true;
        _nativeRecordingBegan = false;
        _isRecordingCountdown = true;
        _recordingStartedAt = DateTimeOffset.UtcNow;
        GestureNameText.IsEnabled = false;
        StartGestureRecordingButton.Visibility = Visibility.Collapsed;
        StopGestureRecordingButton.Visibility = Visibility.Collapsed;
        RecordingPhaseText.Text = "GET READY";
        RecordingPhaseText.Foreground = ThemeBrush("Warning");
        RecordingClockText.Text = "3";
        RecordingProgressBar.Value = 0;
        RecordingTrialText.Text = $"TRIAL {_currentRecordingTrial} OF 3";
        RecordingInstructionText.Text = "Wait for GO, then perform the whole movement once. Recording ends automatically after 2.5 seconds.";
        RecordingQualityText.Text = string.Empty;
        _recordingDisplayTimer.Start();
        Log($"Gesture recording trial {_currentRecordingTrial}/3 countdown started: name=\"{name}\" template_id={_pendingTemplateId}.");
    }

    private void FinishExplicitGestureRecordingButton_Click(object sender, RoutedEventArgs e)
    {
        if (!_isGestureRecording || !_nativeRecordingBegan) return;
        _recordingDisplayTimer.Stop();
        _isGestureRecording = false;
        RecordingPhaseText.Text = "PROCESSING";
        RecordingPhaseText.Foreground = ThemeBrush("Warning");
        RecordingClockText.Text = string.Empty;
        RecordingInstructionText.Text = "Checking motion quality and building the gesture template.";
        StopGestureRecordingButton.IsEnabled = false;
        if (!NativeVisionHostControl.FinishGestureRecording())
        {
            RecordingPhaseText.Text = "TRY AGAIN";
            RecordingPhaseText.Foreground = ThemeBrush("Danger");
            RecordingQualityText.Text = "The recording could not be saved. Please try again.";
            ShowRecordingRetry();
        }
    }

    private void CancelExplicitGestureRecordingButton_Click(object sender, RoutedEventArgs e)
    {
        if (_pendingTemplateId != 0) NativeVisionHostControl.CancelGestureRecording();
        ResetRecordingUi("Nothing was saved. You can record again when ready.");
    }

    private void RecordingDisplayTimer_Tick(object? sender, EventArgs e)
    {
        if (!_isGestureRecording) return;
        var elapsed = DateTimeOffset.UtcNow - _recordingStartedAt;
        if (_isRecordingCountdown)
        {
            var remaining = RecordingCountdown - elapsed;
            if (remaining > TimeSpan.Zero)
            {
                RecordingClockText.Text = Math.Max(1, (int)Math.Ceiling(remaining.TotalSeconds)).ToString(CultureInfo.InvariantCulture);
                return;
            }

            _isRecordingCountdown = false;
            RecordingClockText.Text = "GO";
            RecordingPhaseText.Text = "START NOW";
            if (!NativeVisionHostControl.BeginGestureRecording(_pendingTemplateId))
            {
                _awaitingNativeRecordingStart = false;
                RecordingPhaseText.Text = "TRY AGAIN";
                RecordingQualityText.Text = "Recording could not start. Keep one hand visible and try again.";
                ShowRecordingRetry();
            }
            else
            {
                // Begin is delivered through the native worker mailbox. Until
                // AwaitingHand/Recording is observed, the polled snapshot may
                // still be Completed from the previous registration.
                _awaitingNativeRecordingStart = true;
            }
            return;
        }
        if (!_nativeRecordingBegan) return;
        var recordingElapsed = DateTimeOffset.UtcNow - _recordingStartedAt;
        RecordingClockText.Text = $"{recordingElapsed.TotalSeconds:0.0} / {RecordingDuration.TotalSeconds:0.0} s";
        RecordingProgressBar.Value = Math.Min(recordingElapsed.TotalSeconds, RecordingDuration.TotalSeconds);
        if (recordingElapsed >= RecordingDuration)
        {
            FinishExplicitGestureRecordingButton_Click(this, new RoutedEventArgs());
        }
    }

    private async void NativePollTimer_Tick(object? sender, EventArgs e)
    {
        // Offers arrive and expire independently of the camera pipeline.
        UpdateHandoffStatus();
        if (!NativeVisionHostControl.IsStarted || !NativeVisionHostControl.TryGetMetrics(out var metrics)) return;
        var nativeError = NativeVisionHostControl.GetLastErrorMessage();
        if (!string.IsNullOrWhiteSpace(nativeError)
            && !StringComparer.Ordinal.Equals(nativeError, _lastNativeRuntimeError))
        {
            _lastNativeRuntimeError = nativeError;
            Log("Native runtime error: " + nativeError);
        }
        if (metrics.FrameId == 0) return;

        InferenceTimeText.Text = $"{metrics.PalmInferenceMs + metrics.HandInferenceMs:0.0} ms";
        StateText.Text = TryDescribeVisibleHands();
        PollGestureRecordingStatus();
        LogGestureRecognitionSnapshot();
        AppendNativeMetrics(metrics);
        try
        {
            await _gestureEventPump.PollAsync(_gestureDispatchCancellation.Token);
        }
        catch (OperationCanceledException) when (_gestureDispatchCancellation.IsCancellationRequested) { }
        catch (Exception exception) { Log("Gesture dispatch failed: " + exception.Message); }
    }

    private void PollGestureRecordingStatus()
    {
        if (!NativeVisionHostControl.TryGetGestureRecordingStatus(out var status)) return;
        if (_isRecordingCountdown) return;
        if (_awaitingNativeRecordingStart)
        {
            if (status.State is not (NativeGestureRecordingState.AwaitingHand
                or NativeGestureRecordingState.Recording))
            {
                return;
            }
            _awaitingNativeRecordingStart = false;
        }
        var requiredTakeCount = Math.Max(1U, status.RequiredTakeCount);
        // Native current_take is already one-based and, after an accepted take,
        // already identifies the next take. Never increment it in the UI. Zero
        // only occurs before a session exists, where the ready screen is take 1.
        var currentTake = status.CurrentTake == 0
            ? 1U
            : Math.Min(status.CurrentTake, requiredTakeCount);
        _currentRecordingTrial = currentTake;
        AcceptedTrialsProgress.Maximum = requiredTakeCount;
        AcceptedTrialsProgress.Value = Math.Min(status.AcceptedTakeCount, requiredTakeCount);
        if (status.State == NativeGestureRecordingState.AwaitingHand && _isGestureRecording)
        {
            RecordingTrialText.Text = $"TRIAL {currentTake} OF {requiredTakeCount}";
            RecordingPhaseText.Text = "WAITING FOR ONE HAND";
            RecordingInstructionText.Text = "Keep only the hand you want to register in view.";
            RecordingQualityText.Text = string.Empty;
        }
        else if (status.State == NativeGestureRecordingState.Recording && _isGestureRecording)
        {
            RecordingTrialText.Text = $"TRIAL {currentTake} OF {requiredTakeCount}";
            if (!_nativeRecordingBegan)
            {
                _nativeRecordingBegan = true;
                _recordingStartedAt = DateTimeOffset.UtcNow;
            }
            RecordingPhaseText.Text = "RECORDING";
            RecordingPhaseText.Foreground = ThemeBrush("Danger");
            RecordingInstructionText.Text = "Move now. Hold the final pose until recording stops.";
        }
        else if (status.State == NativeGestureRecordingState.AwaitingNextTake)
        {
            _isGestureRecording = false;
            _nativeRecordingBegan = false;
            _isRecordingCountdown = false;
            _recordingDisplayTimer.Stop();
            RecordingTrialText.Text = $"TRIAL {currentTake} OF {requiredTakeCount}";
            RecordingPhaseText.Text = "ACCEPTED";
            RecordingPhaseText.Foreground = ThemeBrush("Success");
            RecordingClockText.Text = string.Empty;
            RecordingProgressBar.Value = 0;
            RecordingInstructionText.Text =
                $"Trial {status.AcceptedTakeCount} was accepted. Return to the starting pose, then record trial {currentTake}.";
            RecordingQualityText.Text = string.Empty;
            GestureNameText.IsEnabled = false;
            StartGestureRecordingButton.Content = $"Start Trial {currentTake}";
            StartGestureRecordingButton.Visibility = Visibility.Visible;
            StartGestureRecordingButton.IsEnabled = true;
            StopGestureRecordingButton.Visibility = Visibility.Collapsed;
        }
        else if (status.State == NativeGestureRecordingState.Completed)
        {
            var savedTemplateId = status.LastResultTemplateId;
            if (savedTemplateId != 0
                && savedTemplateId == _pendingTemplateId
                && _validationTemplateId != savedTemplateId)
            {
                var name = GestureNameText.Text.Trim();
                _gestureCatalog.NoteName(savedTemplateId, name);
                _validationTemplateId = savedTemplateId;
                _gestureCatalog.Refresh();
                _gestureActionWindow?.SelectGesture(savedTemplateId);
                BeginGestureValidation(name);
            }
            AcceptedTrialsProgress.Value = requiredTakeCount;
            RecordingTrialText.Text = $"{requiredTakeCount} OF {requiredTakeCount} ACCEPTED";
            RecordingPhaseText.Text = "ALL TRIALS SAVED";
            RecordingPhaseText.Foreground = ThemeBrush("Success");
            RecordingInstructionText.Text = "Repeat the gesture in the camera. Recognition updates automatically.";
            RecordingQualityText.Text = string.Empty;
            _recordingDisplayTimer.Stop();
            _isGestureRecording = false;
            _nativeRecordingBegan = false;
            GestureNameText.IsEnabled = true;
            StartGestureRecordingButton.Content = "Record Three New Trials";
            StartGestureRecordingButton.Visibility = Visibility.Visible;
            StopGestureRecordingButton.Visibility = Visibility.Collapsed;
        }
        else if (status.State is NativeGestureRecordingState.Rejected or NativeGestureRecordingState.TrackLost)
        {
            RecordingTrialText.Text = $"TRIAL {currentTake} OF {requiredTakeCount}";
            RecordingPhaseText.Text = "TRY AGAIN";
            RecordingPhaseText.Foreground = ThemeBrush("Danger");
            RecordingQualityText.Text = status.GetReason();
            RecordingInstructionText.Text =
                $"Trial {currentTake} was not saved. Return to the starting pose and retry the same trial.";
            ShowRecordingRetry();
        }
    }

    private string TryDescribeVisibleHands()
    {
        if (!NativeVisionHostControl.TryGetHands(out var hands) || hands.FrameId == 0 || hands.HandCount == 0)
            return "Camera running — show one hand";
        if (hands.HandCount > 1)
            return "Two hands detected";
        return "One hand detected";
    }

    private bool TryGetSingleVisibleHand(out string message)
    {
        message = "Keep one hand visible.";
        if (!NativeVisionHostControl.TryGetHands(out var hands) || hands.FrameId == 0 || hands.HandCount == 0)
            return false;
        if (hands.HandCount > 1)
        {
            message = "Two hands are visible. Keep only the hand you want to register in view.";
            return false;
        }
        message = string.Empty;
        return true;
    }

    private void ResetRecordingUi(string instruction)
    {
        _recordingDisplayTimer.Stop();
        _isGestureRecording = false;
        _nativeRecordingBegan = false;
        _isRecordingCountdown = false;
        _awaitingNativeRecordingStart = false;
        _pendingTemplateId = 0;
        _validationTemplateId = 0;
        _currentRecordingTrial = 1;
        GestureNameText.IsEnabled = true;
        RecordingTrialText.Text = "TRIAL 1 OF 3";
        AcceptedTrialsProgress.Value = 0;
        StartGestureRecordingButton.Content = "Start Trial 1";
        StartGestureRecordingButton.Visibility = Visibility.Visible;
        StartGestureRecordingButton.IsEnabled = NativeVisionHostControl.IsStarted;
        StopGestureRecordingButton.Visibility = Visibility.Collapsed;
        StopGestureRecordingButton.IsEnabled = true;
        RecordingPhaseText.Text = "SHOW ONE HAND";
        RecordingPhaseText.Foreground = ThemeBrush("Info");
        RecordingClockText.Text = string.Empty;
        RecordingClockText.Visibility = Visibility.Visible;
        RecordingProgressBar.Value = 0;
        RecordingProgressBar.Visibility = Visibility.Visible;
        RecordingInstructionText.Text = instruction;
        RecordingQualityText.Text = string.Empty;
        GestureValidationPanel.Visibility = Visibility.Collapsed;
    }

    private void ShowRecordingRetry()
    {
        _recordingDisplayTimer.Stop();
        _isGestureRecording = false;
        _nativeRecordingBegan = false;
        _isRecordingCountdown = false;
        GestureNameText.IsEnabled = true;
        StartGestureRecordingButton.Content = $"Retry Trial {_currentRecordingTrial}";
        StartGestureRecordingButton.Visibility = Visibility.Visible;
        StopGestureRecordingButton.Visibility = Visibility.Collapsed;
    }

    private static uint CreateStableTemplateId(string name)
    {
        const uint offsetBasis = 2166136261;
        const uint prime = 16777619;
        var hash = offsetBasis;
        foreach (var value in Encoding.UTF8.GetBytes(name.Trim().ToUpperInvariant()))
            hash = unchecked((hash ^ value) * prime);
        return hash == 0 ? 1U : hash;
    }

    private unsafe void LogGestureRecognitionSnapshot()
    {
        if (!NativeVisionHostControl.TryGetGestureRecognition(out var snapshot)
            || snapshot.FrameId == 0 || snapshot.FrameId == _lastLoggedGestureFrameId) return;
        _lastLoggedGestureFrameId = snapshot.FrameId;
        var rejection = Marshal.PtrToStringUTF8((IntPtr)snapshot.BestRejectionReason) ?? string.Empty;
        var registration = Marshal.PtrToStringUTF8((IntPtr)snapshot.LastRegistrationReason) ?? string.Empty;
        UpdateGestureValidation(snapshot, rejection);
        Log($"gesture_recognition frame={snapshot.FrameId} track_id={snapshot.TrackId} "
            + $"templates={snapshot.TemplateCount} candidates={snapshot.CandidateCount} "
            + $"template_id={snapshot.BestTemplateId} score={snapshot.BestScore:0.000} "
            + $"confidence={snapshot.BestConfidence:0.000} rejection=\"{rejection}\" "
            + $"registration={snapshot.LastRegistrationStatus}:{snapshot.LastRegistrationTemplateId} \"{registration}\"");
        if (NativeVisionHostControl.TryGetGestureDtwDebug(out var dtw))
        {
            Log($"gesture_dtw frame={dtw.FrameId} template_id={dtw.TemplateId} "
                + $"duration_ms={dtw.CandidateDurationMs} eligible={dtw.Eligible} "
                + $"score={dtw.Score:0.0000} threshold={dtw.ThresholdScore:0.0000} "
                + $"confidence={dtw.Confidence:0.000} warp={dtw.WarpRatio:0.000} "
                + $"reverse={dtw.ReverseScore:0.0000} path_points={dtw.PathCount} "
                + $"reason=\"{dtw.GetReason()}\"");
        }
    }

    private void BeginGestureValidation(string name)
    {
        RecordingClockText.Visibility = Visibility.Collapsed;
        RecordingProgressBar.Visibility = Visibility.Collapsed;
        GestureValidationPanel.Visibility = Visibility.Visible;
        ValidationPromptText.Text = $"Perform “{name}” again";
        ValidationResultText.Text = "WATCHING";
        ValidationResultText.Foreground = ThemeBrush("Info");
        ValidationConfidenceBar.Value = 0;
        ValidationConfidenceText.Text = "0%";
        ValidationWindowBar.Value = 0;
        ValidationWindowText.Text = "Motion window: waiting";
        ValidationReasonText.Text = "Move the registered hand naturally. The result updates automatically.";
        _lastValidationMatchAt = DateTimeOffset.MinValue;
    }

    private void UpdateGestureValidation(NativeGestureRecognitionSnapshot snapshot, string rejection)
    {
        if (_validationTemplateId == 0 || GestureValidationPanel.Visibility != Visibility.Visible) return;

        ValidationWindowBar.Value = Math.Min(snapshot.BestCandidateDurationMs, 2500U);
        ValidationWindowText.Text = snapshot.BestCandidateDurationMs == 0
            ? "Motion window: collecting frames"
            : $"Motion window: {snapshot.BestCandidateDurationMs / 1000.0:0.0} / 2.5 s";

        if (DateTimeOffset.UtcNow - _lastValidationMatchAt < TimeSpan.FromSeconds(1.2)) return;

        if (snapshot.CandidateCount == 0 || snapshot.BestTemplateId == 0)
        {
            ValidationResultText.Text = "WATCHING";
            ValidationResultText.Foreground = ThemeBrush("Info");
            ValidationConfidenceBar.Value = 0;
            ValidationConfidenceText.Text = "0%";
            ValidationReasonText.Text = "Start the gesture and complete it in one continuous movement.";
            return;
        }

        var confidence = snapshot.BestConfidence > 0
            ? snapshot.BestConfidence
            : Math.Clamp(1.0F - snapshot.BestScore / 0.9F, 0.0F, 1.0F);
        var percent = Math.Clamp(confidence * 100.0F, 0.0F, 100.0F);
        ValidationConfidenceBar.Value = percent;
        ValidationConfidenceText.Text = $"{percent:0}%";

        if (snapshot.BestEligible != 0 && snapshot.BestTemplateId == _validationTemplateId)
        {
            _lastValidationMatchAt = DateTimeOffset.UtcNow;
            ValidationResultText.Text = "MATCH";
            ValidationResultText.Foreground = ThemeBrush("Success");
            ValidationReasonText.Text = $"Recognized as “{_gestureCatalog.NameOf(_validationTemplateId, "registered gesture")}”.";
            return;
        }

        if (snapshot.BestActiveSegmentValid == 0)
        {
            ValidationResultText.Text = "WATCHING";
            ValidationResultText.Foreground = ThemeBrush("Info");
            ValidationReasonText.Text = "Make one clear movement, then return to a resting pose.";
            return;
        }

        if (snapshot.BestEligible != 0)
        {
            ValidationResultText.Text = "DIFFERENT GESTURE";
            ValidationResultText.Foreground = ThemeBrush("Warning");
            ValidationReasonText.Text = _gestureCatalog.TryGetName(snapshot.BestTemplateId, out var otherName)
                ? $"The closest recognized gesture was “{otherName}”."
                : "Another registered gesture was closer.";
            return;
        }

        ValidationResultText.Text = percent >= 65 ? "CLOSE" : "NOT YET";
        ValidationResultText.Foreground = percent >= 65
            ? ThemeBrush("Warning")
            : ThemeBrush("Danger");
        ValidationReasonText.Text = HumanizeRecognitionReason(rejection);
    }

    private static string HumanizeRecognitionReason(string reason)
    {
        if (string.IsNullOrWhiteSpace(reason)) return "Complete the movement once more.";
        if (reason.Contains("duration", StringComparison.OrdinalIgnoreCase)) return "Keep the movement going a little longer.";
        if (reason.Contains("usable", StringComparison.OrdinalIgnoreCase)) return "Keep the hand clearly visible throughout the movement.";
        if (reason.Contains("warp", StringComparison.OrdinalIgnoreCase)) return "Use a speed closer to the registered movement.";
        if (reason.Contains("reverse", StringComparison.OrdinalIgnoreCase)) return "The movement direction appears reversed.";
        if (reason.Contains("topology", StringComparison.OrdinalIgnoreCase)) return "Match the registered hand shape more closely.";
        return reason;
    }

    private void InitializeNativeMetricsFile()
    {
        if (_nativeMetricsPath is null) return;
        try
        {
            var directory = System.IO.Path.GetDirectoryName(_nativeMetricsPath);
            if (!string.IsNullOrEmpty(directory)) System.IO.Directory.CreateDirectory(directory);
            System.IO.File.WriteAllText(_nativeMetricsPath,
                "sample_time,frame_id,camera_fps,display_fps,perception_fps,camera_wait_ms,"
                + "frame_copy_ms,preprocess_ms,palm_inference_ms,palm_postprocess_ms,"
                + "roi_crop_warp_ms,hand_inference_ms,landmark_postprocess_ms,tracking_update_ms,"
                + "camera_upload_ms,camera_draw_ms,overlay_draw_ms,hand_3d_draw_ms,end_draw_ms,"
                + "present_wait_ms,render_total_ms,end_to_end_ms,frame_pool_drops,"
                + "perception_drops,gpu_camera_frames,gpu_rendered_frames,gpu_dxgi_format,"
                + "gpu_subresource" + Environment.NewLine);
        }
        catch (Exception exception) { Log("Native metrics file initialization failed: " + exception.Message); }
    }

    private void AppendNativeMetrics(NativeVisionMetrics metrics)
    {
        if (_nativeMetricsPath is null || metrics.FrameId == _lastSampledNativeFrameId) return;
        _lastSampledNativeFrameId = metrics.FrameId;
        object[] values =
        [
            DateTimeOffset.Now.ToString("O", CultureInfo.InvariantCulture), metrics.FrameId,
            metrics.CameraFps, metrics.DisplayFps, metrics.PerceptionFps, metrics.CameraWaitMs,
            metrics.FrameCopyMs, metrics.PreprocessMs, metrics.PalmInferenceMs, metrics.PalmPostprocessMs,
            metrics.RoiCropWarpMs, metrics.HandInferenceMs, metrics.LandmarkPostprocessMs,
            metrics.TrackingUpdateMs, metrics.CameraUploadMs, metrics.CameraDrawMs, metrics.OverlayDrawMs,
            metrics.Hand3dDrawMs, metrics.EndDrawMs, metrics.PresentWaitMs, metrics.OverlayRenderMs,
            metrics.EndToEndLatencyMs, metrics.FramePoolDroppedFrames, metrics.PerceptionDroppedFrames,
            metrics.GpuCameraFrames, metrics.GpuRenderedFrames, metrics.GpuCameraDxgiFormat,
            metrics.GpuCameraSubresource
        ];
        try
        {
            System.IO.File.AppendAllText(_nativeMetricsPath,
                string.Join(',', values.Select(static value =>
                    Convert.ToString(value, CultureInfo.InvariantCulture))) + Environment.NewLine);
        }
        catch (Exception exception) { Log("Native metrics sample failed: " + exception.Message); }
    }

    private void StopNativeRuntime()
    {
        if (_pendingTemplateId != 0) NativeVisionHostControl.CancelGestureRecording();
        _recordingDisplayTimer.Stop();
        _isGestureRecording = false;
        _awaitingNativeRecordingStart = false;
        _pendingTemplateId = 0;
        _nativePollTimer.Stop();
        _gestureDispatchCancellation.Cancel();
        NativeVisionHostControl.StopNativeRuntime();
        NativeVisionHostControl.Visibility = Visibility.Collapsed;
        PreviewPlaceholder.Visibility = Visibility.Visible;
        if (RegistrationPanel.Visibility == Visibility.Visible)
        {
            ResetRecordingUi("Start the camera before recording a gesture.");
        }
    }

    private void Log(string message)
    {
        var line = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}  {message}";
        try { System.IO.File.AppendAllText(_logPath, line + Environment.NewLine); }
        catch { }
    }

    protected override void OnClosed(EventArgs e)
    {
        // Playback retains a non-owning native runtime pointer, so its HwndHost
        // must destroy the playback handle before the source runtime stops.
        _recordingPlaybackWindow?.Close();
        _recordingPlaybackWindow = null;
        _gestureActionWindow?.Close();
        _gestureActionWindow = null;
        _handoffEffect?.Close();
        _handoffEffect = null;
        StopNativeRuntime();
        _ = _handoffService.DisposeAsync();
        base.OnClosed(e);
    }

    // LAN handoff is advanced only by recognized gestures bound to handoff.grab
    // and handoff.release, so the main window reports state without offering
    // any control that could bypass the gesture path.
    private void UpdateHandoffStatus()
    {
        var status = _handoffService.GetStatus();
        var text = $"LAN handoff {status.State.ToString().ToLowerInvariant()} · {status.Message}";
        if (!StringComparer.Ordinal.Equals(HandoffStatusText.Text, text))
        {
            HandoffStatusText.Text = text;
        }
        var accent = status.State switch
        {
            HandoffState.Failed => ThemeBrush("Danger"),
            HandoffState.Completed => ThemeBrush("Success"),
            HandoffState.Advertising or HandoffState.OfferAvailable or HandoffState.Claiming
                => ThemeBrush("Info"),
            _ => ThemeBrush("TextMuted")
        };
        HandoffStatusText.Foreground = accent;
        HandoffStatusDot.Fill = accent;
        // Decode before the effect so a completed transfer can show its payload.
        UpdateLastTransfer(status.LastTransfer);
        PlayHandoffEffectOnChange(status);
    }

    /// <summary>
    /// Renders the receipt for a newly completed transfer. Nothing clears it:
    /// after the effect fades this is the only record of what moved.
    /// </summary>
    private void UpdateLastTransfer(HandoffTransfer? transfer)
    {
        if (transfer is null) return;
        var key = $"{transfer.Direction}:{transfer.PayloadId}";
        if (StringComparer.Ordinal.Equals(key, _lastTransferKey)) return;
        _lastTransferKey = key;
        _lastTransferPath = transfer.LocalPath;
        _lastTransferImage = TryDecodeTransferPreview(transfer);

        var received = transfer.Direction == HandoffDirection.Received;
        LastTransferGlyph.Text = received ? "↓" : "↑";
        LastTransferGlyph.Foreground = ThemeBrush(received ? "Success" : "Accent");
        LastTransferHeadline.Text = received
            ? $"Received from {transfer.PeerName}"
            : $"Sent to {transfer.PeerName}";
        LastTransferDetail.Text =
            $"{transfer.FileName} · {DescribeByteSize(transfer.Length)} · {transfer.CompletedAt.ToLocalTime():HH:mm:ss}";
        LastTransferThumbnail.Background = _lastTransferImage is null
            ? null
            : new System.Windows.Media.ImageBrush(_lastTransferImage)
            {
                Stretch = System.Windows.Media.Stretch.UniformToFill
            };
        LastTransferThumbnailFallback.Visibility = _lastTransferImage is null
            ? Visibility.Visible
            : Visibility.Collapsed;
        var openable = _lastTransferPath is not null && System.IO.File.Exists(_lastTransferPath);
        LastTransferCard.Cursor = openable ? System.Windows.Input.Cursors.Hand : null;
        LastTransferCard.ToolTip = BuildTransferTooltip(transfer, openable);
        LastTransferCard.Visibility = Visibility.Visible;
    }

    /// <summary>
    /// A larger look at the payload, built in code so the preview brush and the
    /// pointer hint stay together with the transfer they describe.
    /// </summary>
    private object BuildTransferTooltip(HandoffTransfer transfer, bool openable)
    {
        var content = new StackPanel { MaxWidth = 360 };
        if (_lastTransferImage is not null)
        {
            content.Children.Add(new Border
            {
                Height = 200,
                CornerRadius = new CornerRadius(6),
                Background = new System.Windows.Media.ImageBrush(_lastTransferImage)
                {
                    Stretch = System.Windows.Media.Stretch.Uniform
                }
            });
        }
        content.Children.Add(new TextBlock
        {
            Text = transfer.FileName,
            FontWeight = FontWeights.SemiBold,
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, _lastTransferImage is null ? 0 : 10, 0, 0)
        });
        var caption = transfer.Direction == HandoffDirection.Received
            ? $"Received from {transfer.PeerName} · {DescribeByteSize(transfer.Length)}"
            : $"Sent to {transfer.PeerName} · {DescribeByteSize(transfer.Length)}";
        if (transfer.LocalPath is { } path) caption += "\n" + path;
        if (openable) caption += "\nClick to open";
        content.Children.Add(new TextBlock
        {
            Text = caption,
            Foreground = ThemeBrush("TextSecondary"),
            FontSize = 11,
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 4, 0, 0)
        });
        return new ToolTip
        {
            Content = new Border
            {
                Style = (Style)Application.Current.Resources["Card"],
                Padding = new Thickness(12),
                Child = content
            },
            Background = System.Windows.Media.Brushes.Transparent,
            BorderThickness = new Thickness(0),
            Padding = new Thickness(0),
            HasDropShadow = false
        };
    }

    private System.Windows.Media.ImageSource? TryDecodeTransferPreview(HandoffTransfer transfer)
    {
        if (!transfer.ContentType.StartsWith("image/", StringComparison.OrdinalIgnoreCase)
            || transfer.Data.IsEmpty)
        {
            return null;
        }
        try
        {
            using var stream = new System.IO.MemoryStream(transfer.Data.ToArray());
            var image = new System.Windows.Media.Imaging.BitmapImage();
            image.BeginInit();
            // OnLoad decodes while the stream is alive; the receipt outlives it.
            image.CacheOption = System.Windows.Media.Imaging.BitmapCacheOption.OnLoad;
            image.DecodePixelWidth = TransferPreviewWidth;
            image.StreamSource = stream;
            image.EndInit();
            image.Freeze();
            return image;
        }
        catch (Exception exception) when (exception is NotSupportedException
            or ArgumentException or System.IO.IOException or OverflowException)
        {
            Log("Handoff preview could not be decoded: " + exception.Message);
            return null;
        }
    }

    private void LastTransferCard_MouseLeftButtonUp(object sender, System.Windows.Input.MouseButtonEventArgs e)
    {
        if (_lastTransferPath is not { } path || !System.IO.File.Exists(path)) return;
        try
        {
            System.Diagnostics.Process.Start(
                new System.Diagnostics.ProcessStartInfo(path) { UseShellExecute = true });
        }
        catch (Exception exception) when (exception is System.ComponentModel.Win32Exception
            or InvalidOperationException or System.IO.IOException)
        {
            Log("Could not open the received file: " + exception.Message);
        }
    }

    private static string DescribeByteSize(long bytes) => bytes switch
    {
        >= 1024 * 1024 => $"{bytes / (1024.0 * 1024.0):0.0} MB",
        >= 1024 => $"{bytes / 1024.0:0} KB",
        _ => $"{bytes} B"
    };

    /// <summary>
    /// The transfer is invisible otherwise: the operator's hand is on the
    /// gesture, not the screen, so each phase change gets one short flourish.
    /// </summary>
    private void PlayHandoffEffectOnChange(HandoffStatus status)
    {
        if (status.State == _lastHandoffState) return;
        var previous = _lastHandoffState;
        _lastHandoffState = status.State;
        var kind = status.State switch
        {
            HandoffState.Advertising => HandoffEffectKind.Offering,
            HandoffState.Claiming => HandoffEffectKind.Claiming,
            HandoffState.Completed => HandoffEffectKind.Completed,
            HandoffState.Failed => HandoffEffectKind.Failed,
            // An offer appearing is worth one cue, but only on the way up from
            // idle; repeated re-advertising by a peer must not strobe.
            HandoffState.OfferAvailable when previous is HandoffState.Idle
                => HandoffEffectKind.Claiming,
            _ => (HandoffEffectKind?)null
        };
        if (kind is not { } effect) return;
        var title = status.State switch
        {
            HandoffState.Advertising => "Offering over LAN",
            HandoffState.Claiming => "Receiving…",
            HandoffState.Completed => "Handoff complete",
            HandoffState.Failed => "Handoff failed",
            _ => "Offer available"
        };
        var detail = status.Message;
        System.Windows.Media.ImageSource? preview = null;
        // On completion the payload itself is the message: name what moved,
        // where it went or came from, and show it.
        if (status.State == HandoffState.Completed && status.LastTransfer is { } transfer)
        {
            title = transfer.Direction == HandoffDirection.Received
                ? $"Received from {transfer.PeerName}"
                : $"Sent to {transfer.PeerName}";
            detail = $"{transfer.FileName} · {DescribeByteSize(transfer.Length)}";
            preview = _lastTransferImage;
        }
        _handoffEffect ??= new HandoffEffectWindow { Owner = this };
        _handoffEffect.Play(this, effect, title, detail, preview);
    }
}

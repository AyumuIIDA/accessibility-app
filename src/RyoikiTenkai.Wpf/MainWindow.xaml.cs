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
    private uint _pendingTemplateId;
    private uint _validationTemplateId;
    private uint _currentRecordingTrial = 1;
    private readonly Dictionary<uint, string> _gestureNames = [];
    private readonly Dictionary<uint, GestureCommandBinding> _savedGestureBindings = [];
    private GestureDtwDebugWindow? _dtwDebugWindow;
    private GestureRecordingPlaybackWindow? _recordingPlaybackWindow;
    private DateTimeOffset _lastValidationMatchAt;
    private readonly MutableGestureBindingResolver _gestureBindings = new();
    private readonly NativeGestureEventPump _gestureEventPump;
    private CancellationTokenSource _gestureDispatchCancellation = new();
    private readonly HandoffService _handoffService;

    private sealed record SavedGestureItem(uint Id, string Name, bool Enabled, uint TakeCount)
    {
        public override string ToString() =>
            $"{Name}  ·  {TakeCount} takes  ·  {(Enabled ? "Enabled" : "Disabled")}";
    }

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
        StateText.Text = "Starting camera and hand models…";
        NativeVisionHostControl.Visibility = Visibility.Visible;
        if (!NativeVisionHostControl.StartNativeRuntime())
        {
            NativeVisionHostControl.Visibility = Visibility.Collapsed;
            StateText.Text = "Camera could not start";
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
        StateText.Text = "Camera running";
        RefreshSavedGestures();
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
        StateText.Text = "Camera stopped";
        StartButton.IsEnabled = true;
        StopButton.IsEnabled = false;
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
        _dtwDebugWindow = new GestureDtwDebugWindow(NativeVisionHostControl) { Owner = this };
        _dtwDebugWindow.Closed += (_, _) => _dtwDebugWindow = null;
        _dtwDebugWindow.Show();
    }

    private void EnterGestureRegistrationModeButton_Click(object sender, RoutedEventArgs e)
    {
        RegistrationColumn.Width = new GridLength(420);
        RegistrationPanel.Visibility = Visibility.Visible;
        OpenGestureRegistrationButton.IsEnabled = false;
        RefreshSavedGestures();
        ResetRecordingUi(NativeVisionHostControl.IsStarted
            ? "Keep only the hand you want to record in view."
            : "Start the camera before recording a gesture.");
    }

    private void ExitGestureRegistrationModeButton_Click(object sender, RoutedEventArgs e)
    {
        if (_isGestureRecording) NativeVisionHostControl.CancelGestureRecording();
        _recordingDisplayTimer.Stop();
        _isGestureRecording = false;
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

        _pendingTemplateId = CreateStableTemplateId(name);
        if (!TryRepositoryOperation(
                () => NativeVisionHostControl.SetGestureDefinitionMetadata(_pendingTemplateId, name, true),
                "Ready to record.", "The gesture name could not be prepared for saving."))
        {
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
        RecordingPhaseText.Foreground = System.Windows.Media.Brushes.Gold;
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
        RecordingPhaseText.Foreground = System.Windows.Media.Brushes.Gold;
        RecordingClockText.Text = string.Empty;
        RecordingInstructionText.Text = "Checking motion quality and building the gesture template.";
        StopGestureRecordingButton.IsEnabled = false;
        if (!NativeVisionHostControl.FinishGestureRecording())
        {
            RecordingPhaseText.Text = "TRY AGAIN";
            RecordingPhaseText.Foreground = System.Windows.Media.Brushes.OrangeRed;
            RecordingQualityText.Text = "The recording could not be saved. Please try again.";
            ShowRecordingRetry();
        }
    }

    private void CancelExplicitGestureRecordingButton_Click(object sender, RoutedEventArgs e)
    {
        if (_isGestureRecording) NativeVisionHostControl.CancelGestureRecording();
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
                RecordingPhaseText.Text = "TRY AGAIN";
                RecordingQualityText.Text = "Recording could not start. Keep one hand visible and try again.";
                ShowRecordingRetry();
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
        if (_handoffService.LatestOffer is { } offer)
            HandoffStatusText.Text = $"LAN offer: {offer.FileName} from {offer.SenderName} ({Math.Max(0, (offer.ExpiresAt - DateTimeOffset.UtcNow).TotalSeconds):0}s)";
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
        if (_isRecordingCountdown) return;
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
            RecordingPhaseText.Foreground = System.Windows.Media.Brushes.OrangeRed;
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
            RecordingPhaseText.Foreground = System.Windows.Media.Brushes.LightGreen;
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
            if (savedTemplateId != 0 && _validationTemplateId != savedTemplateId)
            {
                var name = GestureNameText.Text.Trim();
                _gestureNames[savedTemplateId] = name;
                _validationTemplateId = savedTemplateId;
                RefreshSavedGestures(savedTemplateId);
                BeginGestureValidation(name);
            }
            AcceptedTrialsProgress.Value = requiredTakeCount;
            RecordingTrialText.Text = $"{requiredTakeCount} OF {requiredTakeCount} ACCEPTED";
            RecordingPhaseText.Text = "ALL TRIALS SAVED";
            RecordingPhaseText.Foreground = System.Windows.Media.Brushes.LightGreen;
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
            RecordingPhaseText.Foreground = System.Windows.Media.Brushes.OrangeRed;
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
        RecordingPhaseText.Foreground = System.Windows.Media.Brushes.DeepSkyBlue;
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

    private void SavedGesturesList_SelectionChanged(
        object sender,
        System.Windows.Controls.SelectionChangedEventArgs e)
    {
        if (SavedGesturesList.SelectedItem is not SavedGestureItem item) return;
        GestureNameText.Text = item.Name;
        SelectedGestureEnabledCheckBox.IsChecked = item.Enabled;
        GestureRepositoryStatusText.Text =
            $"{item.TakeCount} accepted takes. Changes are saved by the native repository.";
        if (_savedGestureBindings.TryGetValue(item.Id, out var binding))
        {
            GestureBindingEnabledCheckBox.IsChecked = binding.Enabled;
            GestureActionParameterText.Text = binding.Action.Params.Values.FirstOrDefault() ?? string.Empty;
            foreach (var option in GestureActionTypeComboBox.Items.OfType<ComboBoxItem>())
                if (StringComparer.Ordinal.Equals(option.Tag as string, binding.Action.Type))
                    GestureActionTypeComboBox.SelectedItem = option;
        }
        else
        {
            GestureBindingEnabledCheckBox.IsChecked = true;
            GestureActionParameterText.Clear();
            GestureActionTypeComboBox.SelectedIndex = 0;
        }
    }

    private void ReviewGestureRecordingsButton_Click(object sender, RoutedEventArgs e)
    {
        if (SavedGesturesList.SelectedItem is not SavedGestureItem item)
        {
            GestureRepositoryStatusText.Text = "Select a saved gesture to review.";
            return;
        }
        if (_recordingPlaybackWindow is { IsVisible: true })
        {
            _recordingPlaybackWindow.Close();
        }
        _recordingPlaybackWindow = new GestureRecordingPlaybackWindow(
            NativeVisionHostControl, item.Id, item.Name) { Owner = this };
        _recordingPlaybackWindow.Closed += (_, _) => _recordingPlaybackWindow = null;
        _recordingPlaybackWindow.Show();
    }

    private void SaveGestureBindingButton_Click(object sender, RoutedEventArgs e)
    {
        if (SavedGesturesList.SelectedItem is not SavedGestureItem item
            || GestureActionTypeComboBox.SelectedItem is not ComboBoxItem option
            || option.Tag is not string actionType)
        {
            GestureRepositoryStatusText.Text = "Select a saved gesture and action type.";
            return;
        }
        var parameter = GestureActionParameterText.Text;
        var parameterRequired = !actionType.StartsWith("handoff.", StringComparison.Ordinal);
        if ((parameterRequired && string.IsNullOrWhiteSpace(parameter))
            || Encoding.UTF8.GetByteCount(parameter) > 255)
        {
            GestureRepositoryStatusText.Text = "Enter an action value of at most 255 UTF-8 bytes.";
            return;
        }
        if (!TryRepositoryOperation(() => NativeVisionHostControl.UpsertGestureBinding(
                item.Id, actionType, parameter, GestureBindingEnabledCheckBox.IsChecked == true),
            "Action binding saved.", "The action binding could not be saved.")) return;
        RefreshGestureBindings();
        SavedGesturesList_SelectionChanged(SavedGesturesList,
            new SelectionChangedEventArgs(
                System.Windows.Controls.Primitives.Selector.SelectionChangedEvent,
                new System.Collections.ArrayList(), new System.Collections.ArrayList()));
    }

    private void RemoveGestureBindingButton_Click(object sender, RoutedEventArgs e)
    {
        if (SavedGesturesList.SelectedItem is not SavedGestureItem item)
        {
            GestureRepositoryStatusText.Text = "Select a saved gesture first.";
            return;
        }
        if (!TryRepositoryOperation(() => NativeVisionHostControl.DeleteGestureBinding(item.Id),
            "Action binding removed.", "The action binding could not be removed.")) return;
        RefreshGestureBindings();
        GestureActionParameterText.Clear();
    }

    private async void UpdateGestureDefinitionButton_Click(object sender, RoutedEventArgs e)
    {
        if (SavedGesturesList.SelectedItem is not SavedGestureItem item)
        {
            GestureRepositoryStatusText.Text = "Select a saved gesture to update.";
            return;
        }
        var name = GestureNameText.Text.Trim();
        if (string.IsNullOrWhiteSpace(name) || Encoding.UTF8.GetByteCount(name) > 63)
        {
            GestureRepositoryStatusText.Text = "The name must contain 1–63 UTF-8 bytes.";
            return;
        }
        if (!TryRepositoryOperation(() => NativeVisionHostControl.SetGestureDefinitionMetadata(
                item.Id, name, SelectedGestureEnabledCheckBox.IsChecked == true),
            "Gesture updated.", "The gesture could not be updated.")) return;
        await RefreshAfterRepositoryCommandAsync(item.Id);
    }

    private async void DeleteGestureDefinitionButton_Click(object sender, RoutedEventArgs e)
    {
        if (SavedGesturesList.SelectedItem is not SavedGestureItem item)
        {
            GestureRepositoryStatusText.Text = "Select a saved gesture to delete.";
            return;
        }
        if (MessageBox.Show(this,
                $"Delete “{item.Name}” and all {item.TakeCount} recorded takes?",
                "Delete gesture", MessageBoxButton.YesNo, MessageBoxImage.Warning)
            != MessageBoxResult.Yes) return;
        if (!TryRepositoryOperation(() => NativeVisionHostControl.DeleteGestureDefinition(item.Id),
            "Gesture deleted.", "The gesture could not be deleted.")) return;
        _gestureNames.Remove(item.Id);
        await RefreshAfterRepositoryCommandAsync();
    }

    private async void ReloadGestureDefinitionsButton_Click(object sender, RoutedEventArgs e)
    {
        if (!TryRepositoryOperation(NativeVisionHostControl.ReloadGestureDefinitions,
            "Saved gestures reloaded.", "Saved gestures could not be reloaded.")) return;
        await RefreshAfterRepositoryCommandAsync();
    }

    private bool TryRepositoryOperation(Func<bool> operation, string success, string failure)
    {
        if (!NativeVisionHostControl.IsStarted)
        {
            GestureRepositoryStatusText.Text = "Start the camera to change saved gestures.";
            return false;
        }
        try
        {
            if (!operation())
            {
                GestureRepositoryStatusText.Text = failure;
                return false;
            }
            GestureRepositoryStatusText.Text = success;
            return true;
        }
        catch (Exception exception) when (exception is EntryPointNotFoundException
            or DllNotFoundException or BadImageFormatException)
        {
            GestureRepositoryStatusText.Text = "Native gesture storage is unavailable: " + exception.Message;
            return false;
        }
    }

    private async Task RefreshAfterRepositoryCommandAsync(uint selectId = 0)
    {
        await Task.Delay(500);
        var error = NativeVisionHostControl.GetLastErrorMessage();
        if (error.StartsWith("Gesture repository:", StringComparison.Ordinal))
        {
            GestureRepositoryStatusText.Text = error;
            return;
        }
        RefreshSavedGestures(selectId);
    }

    private void RefreshSavedGestures(uint selectId = 0)
    {
        try
        {
            if (!NativeVisionHostControl.TryListGestureDefinitions(out var definitions))
            {
                GestureRepositoryStatusText.Text = "Saved gestures could not be read.";
                return;
            }
            SavedGesturesList.Items.Clear();
            _gestureNames.Clear();
            var count = (int)Math.Min(definitions.Count, (uint)NativeVisionInterop.MaxGestureDefinitions);
            for (var index = 0; index < count; ++index)
            {
                var metadata = definitions.GetItem(index);
                var item = new SavedGestureItem(
                    metadata.DefinitionId, metadata.GetName(), metadata.Enabled != 0, metadata.TakeCount);
                SavedGesturesList.Items.Add(item);
                _gestureNames[item.Id] = item.Name;
                if (item.Id == selectId) SavedGesturesList.SelectedItem = item;
            }
            RefreshGestureBindings();
            if (SavedGesturesList.SelectedItem is SavedGestureItem)
                SavedGesturesList_SelectionChanged(SavedGesturesList,
                    new SelectionChangedEventArgs(
                        System.Windows.Controls.Primitives.Selector.SelectionChangedEvent,
                        new System.Collections.ArrayList(), new System.Collections.ArrayList()));
            var error = definitions.GetError();
            GestureRepositoryStatusText.Text = !string.IsNullOrWhiteSpace(error)
                ? error
                : count == 0 ? "No saved gestures yet." : $"{count} saved gesture{(count == 1 ? string.Empty : "s")}.";
        }
        catch (Exception exception) when (exception is EntryPointNotFoundException
            or DllNotFoundException or BadImageFormatException)
        {
            GestureRepositoryStatusText.Text = "Native gesture storage is unavailable: " + exception.Message;
        }
    }

    private void RefreshGestureBindings()
    {
        _savedGestureBindings.Clear();
        if (!NativeVisionHostControl.TryListGestureBindings(out var bindings))
        {
            _gestureBindings.Replace([]);
            return;
        }
        var count = (int)Math.Min(bindings.Count, (uint)NativeVisionInterop.MaxGestureDefinitions);
        for (var index = 0; index < count; ++index)
        {
            var native = bindings.GetItem(index);
            var actionType = native.GetActionType();
            var parameterName = actionType switch
            {
                "app.launch" => "path",
                "keyboard.typeText" => "text",
                "keyboard.hotkey" => "hotkey",
                "handoff.grab" => string.Empty,
                "handoff.release" => string.Empty,
                _ => string.Empty
            };
            if (actionType is not ("app.launch" or "keyboard.typeText" or "keyboard.hotkey"
                or "handoff.grab" or "handoff.release")) continue;
            var binding = new GestureCommandBinding(native.DefinitionId,
                _gestureNames.GetValueOrDefault(native.DefinitionId, $"Gesture {native.DefinitionId}"),
                native.Enabled != 0,
                new ActionSpec(actionType, string.IsNullOrEmpty(parameterName)
                    ? [] : new Dictionary<string, string> { [parameterName] = native.GetActionParameter() }));
            _savedGestureBindings[native.DefinitionId] = binding;
        }
        _gestureBindings.Replace(_savedGestureBindings.Values);
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
        ValidationResultText.Foreground = System.Windows.Media.Brushes.DeepSkyBlue;
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
            ValidationResultText.Foreground = System.Windows.Media.Brushes.DeepSkyBlue;
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
            ValidationResultText.Foreground = System.Windows.Media.Brushes.LightGreen;
            ValidationReasonText.Text = $"Recognized as “{_gestureNames.GetValueOrDefault(_validationTemplateId, "registered gesture")}”.";
            return;
        }

        if (snapshot.BestActiveSegmentValid == 0)
        {
            ValidationResultText.Text = "WATCHING";
            ValidationResultText.Foreground = System.Windows.Media.Brushes.DeepSkyBlue;
            ValidationReasonText.Text = "Make one clear movement, then return to a resting pose.";
            return;
        }

        if (snapshot.BestEligible != 0)
        {
            ValidationResultText.Text = "DIFFERENT GESTURE";
            ValidationResultText.Foreground = System.Windows.Media.Brushes.Gold;
            ValidationReasonText.Text = _gestureNames.TryGetValue(snapshot.BestTemplateId, out var otherName)
                ? $"The closest recognized gesture was “{otherName}”."
                : "Another registered gesture was closer.";
            return;
        }

        ValidationResultText.Text = percent >= 65 ? "CLOSE" : "NOT YET";
        ValidationResultText.Foreground = percent >= 65
            ? System.Windows.Media.Brushes.Gold
            : System.Windows.Media.Brushes.OrangeRed;
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
        if (_isGestureRecording) NativeVisionHostControl.CancelGestureRecording();
        _recordingDisplayTimer.Stop();
        _isGestureRecording = false;
        _nativePollTimer.Stop();
        _gestureDispatchCancellation.Cancel();
        NativeVisionHostControl.StopNativeRuntime();
        NativeVisionHostControl.Visibility = Visibility.Collapsed;
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
        StopNativeRuntime();
        _ = _handoffService.DisposeAsync();
        base.OnClosed(e);
    }

    private async void OfferScreenButton_Click(object sender, RoutedEventArgs e)
    {
        try { await _handoffService.GrabScreenshotAsync(CancellationToken.None); HandoffStatusText.Text = "LAN screenshot offer is active."; }
        catch (Exception exception) { HandoffStatusText.Text = "LAN offer failed: " + exception.Message; }
    }

    private async void ReceiveOfferButton_Click(object sender, RoutedEventArgs e)
    {
        try { var path = await _handoffService.ReleaseHereAsync(CancellationToken.None); HandoffStatusText.Text = "Received: " + path; }
        catch (Exception exception) { HandoffStatusText.Text = "LAN receive failed: " + exception.Message; }
    }
}

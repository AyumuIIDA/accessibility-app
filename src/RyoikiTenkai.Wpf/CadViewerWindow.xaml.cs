using System.Windows;
using System.Windows.Input;
using System.Windows.Threading;
using RyoikiTenkai.Wpf.Native;

namespace RyoikiTenkai.Wpf;

public partial class CadViewerWindow : Window
{
    private readonly NativeVisionHost _visionHost;
    private readonly DispatcherTimer _timer;
    private NativeCadHandInteractionMode _interactionMode;
    private NativeHandPresentationMode _presentationMode =
        NativeHandPresentationMode.MirrorDirect;
    private float _rotationSensitivity = 1.0f;
    private bool _isInitialized;

    internal CadViewerWindow(NativeVisionHost visionHost)
    {
        _visionHost = visionHost ?? throw new ArgumentNullException(nameof(visionHost));
        InitializeComponent();
        _isInitialized = true;
        CadViewport.DiagnosticLogged += message => InteractionStateText.Text = message;
        _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(33) };
        _timer.Tick += Timer_Tick;
        _timer.Start();
        Loaded += (_, _) => UpdateNativeCadInteraction();
        Deactivated += (_, _) =>
        {
            _interactionMode = NativeCadHandInteractionMode.None;
            UpdateNativeCadInteraction();
        };
        PreviewKeyDown += CadViewerWindow_PreviewKeyDown;
        PreviewKeyUp += CadViewerWindow_PreviewKeyUp;
    }

    private void Timer_Tick(object? sender, EventArgs e)
    {
        if (!string.IsNullOrWhiteSpace(CadViewport.InitializationError))
        {
            InteractionStateText.Text = "RENDER ERROR";
            InstructionText.Text = CadViewport.InitializationError;
            return;
        }
        if (!_visionHost.IsStarted)
        {
            InteractionStateText.Text = "START NATIVE RUNTIME";
            InstructionText.Text = "Start the camera runtime";
            return;
        }
        if (GestureEnabledCheckBox.IsChecked != true)
        {
            if (_interactionMode != NativeCadHandInteractionMode.None)
            {
                _interactionMode = NativeCadHandInteractionMode.None;
                UpdateNativeCadInteraction();
            }
        }

        if (!_visionHost.TryGetCadHandInteraction(out var interaction))
        {
            InteractionStateText.Text = "HAND INTERACTION ERROR";
            return;
        }

        var state = InteractionState(interaction.State);
        InteractionStateText.Text = GestureEnabledCheckBox.IsChecked == true
            ? state
            : "HAND CONTROL OFF";
        ZoomText.Text =
            $"{interaction.Zoom:0.00}x  P {interaction.PanX:0.00}, {interaction.PanY:0.00}";
        NormalAngleText.Text =
            $"Y {interaction.YawDeltaDegrees:0.0} deg / P {interaction.PitchDeltaDegrees:0.0} deg";
        InstructionText.Text = GestureEnabledCheckBox.IsChecked != true
            ? "Use mouse controls"
            : state switch
            {
                "ROTATING" => "Keep Space held and rotate the hand",
                "PANNING" => "Keep Shift+Space held and move the hand",
                "ZOOMING" => "Keep Ctrl+Space held and move toward/away",
                "PAUSED: TRACKING" => "Keep Space held; reacquiring hand pose",
                "RELEASE AND HOLD SPACE AGAIN" =>
                    "Release Space, then hold it to recenter",
                "READY - HOLD SPACE" => "Hold Space to capture the reference pose",
                _ => "Hold Space to rotate"
            };
    }

    private void CadViewerWindow_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Space || e.IsRepeat) return;
        _interactionMode = Keyboard.Modifiers.HasFlag(ModifierKeys.Control)
            ? NativeCadHandInteractionMode.Zoom
            : Keyboard.Modifiers.HasFlag(ModifierKeys.Shift)
                ? NativeCadHandInteractionMode.Pan
                : NativeCadHandInteractionMode.Rotate;
        UpdateNativeCadInteraction();
        e.Handled = true;
    }

    private void CadViewerWindow_PreviewKeyUp(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Space) return;
        _interactionMode = NativeCadHandInteractionMode.None;
        UpdateNativeCadInteraction();
        e.Handled = true;
    }

    private void ResetViewButton_Click(object sender, RoutedEventArgs e)
    {
        _interactionMode = NativeCadHandInteractionMode.None;
        UpdateNativeCadInteraction();
        CadViewport.ResetView();
        InteractionStateText.Text = "INACTIVE";
        ZoomText.Text = "1.00x";
        NormalAngleText.Text = "Y 0.0 deg / P 0.0 deg";
    }

    private void RotationSensitivitySlider_ValueChanged(
        object sender,
        RoutedPropertyChangedEventArgs<double> e)
    {
        _rotationSensitivity = Math.Clamp((float)e.NewValue, 0.25f, 2.0f);
        if (RotationSensitivityText is not null)
        {
            RotationSensitivityText.Text = $"{_rotationSensitivity:0.00}x";
        }
        UpdateNativeCadInteraction();
    }

    private void PresentationModeComboBox_SelectionChanged(
        object sender,
        System.Windows.Controls.SelectionChangedEventArgs e)
    {
        _presentationMode = PresentationModeComboBox.SelectedIndex == 1
            ? NativeHandPresentationMode.Physical
            : NativeHandPresentationMode.MirrorDirect;
        if (!IsLoaded) return;
        _interactionMode = NativeCadHandInteractionMode.None;
        _visionHost.SetHandPresentationMode(_presentationMode);
        UpdateNativeCadInteraction();
    }

    private void UpdateNativeCadInteraction()
    {
        if (!_isInitialized
            || !_visionHost.IsStarted
            || !CadViewport.IsAvailable)
        {
            return;
        }
        _visionHost.ConfigureCadHandInteraction(
            CadViewport,
            _interactionMode,
            _presentationMode,
            _rotationSensitivity);
    }

    private static string InteractionState(
        NativeCadHandInteractionState state) => state switch
    {
        NativeCadHandInteractionState.Ready => "READY - HOLD SPACE",
        NativeCadHandInteractionState.Rotating => "ROTATING",
        NativeCadHandInteractionState.Panning => "PANNING",
        NativeCadHandInteractionState.Zooming => "ZOOMING",
        NativeCadHandInteractionState.Suspended => "PAUSED: TRACKING",
        NativeCadHandInteractionState.AwaitingRelease =>
            "RELEASE AND HOLD SPACE AGAIN",
        _ => "NO HAND"
    };

    protected override void OnClosed(EventArgs e)
    {
        _interactionMode = NativeCadHandInteractionMode.None;
        UpdateNativeCadInteraction();
        _timer.Stop();
        base.OnClosed(e);
    }
}

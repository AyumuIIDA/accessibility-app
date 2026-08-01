using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace RyoikiTenkai.Wpf;

internal enum HandoffEffectKind
{
    /// <summary>A payload was captured and is now advertised: energy leaves this screen.</summary>
    Offering,
    /// <summary>A claim is in flight: energy arrives at this screen.</summary>
    Claiming,
    Completed,
    Failed
}

/// <summary>
/// Full-window flourish for gesture-driven LAN handoff. It is a separate
/// top-level window because WPF content cannot reliably overlay the native
/// preview's HwndHost, and it is click-through so it never intercepts the
/// input that the gesture path depends on.
/// </summary>
public partial class HandoffEffectWindow : Window
{
    private const int GwlExStyle = -20;
    private const int WsExTransparent = 0x00000020;
    private const int WsExNoActivate = 0x08000000;
    private const int WsExToolWindow = 0x00000080;

    // A cue with a payload preview has to stay up long enough to read the file
    // name and recognize the image; a phase cue does not.
    private static readonly TimeSpan CueHold = TimeSpan.FromMilliseconds(780);
    private static readonly TimeSpan PreviewHold = TimeSpan.FromMilliseconds(2200);
    private static readonly TimeSpan FadeOut = TimeSpan.FromMilliseconds(420);

    private readonly Storyboard _storyboard = new();

    internal HandoffEffectWindow()
    {
        InitializeComponent();
        SourceInitialized += MakeClickThrough;
        _storyboard.Completed += (_, _) => Hide();
    }

    /// <summary>
    /// Positions the effect over <paramref name="owner"/> and plays one pass.
    /// Replays restart the storyboard rather than stacking windows.
    /// <paramref name="preview"/> is the transferred payload, shown so the
    /// operator sees what moved and not merely that something did.
    /// </summary>
    internal void Play(Window owner, HandoffEffectKind kind, string title, string detail,
        ImageSource? preview = null)
    {
        if (!TryTrackOwner(owner)) return;

        var accent = kind switch
        {
            HandoffEffectKind.Completed => ThemeBrush("Success"),
            HandoffEffectKind.Failed => ThemeBrush("Danger"),
            HandoffEffectKind.Claiming => ThemeBrush("Info"),
            _ => ThemeBrush("Accent")
        };
        EdgeGlow.BorderBrush = accent;
        EdgeLine.BorderBrush = accent;
        Ring.Stroke = accent;
        RingTrail.Stroke = accent;
        Badge.BorderBrush = accent;
        BadgeGlyph.Foreground = accent;
        BadgeGlyph.Text = kind switch
        {
            HandoffEffectKind.Completed => "✓",
            HandoffEffectKind.Failed => "!",
            HandoffEffectKind.Claiming => "⇩",
            _ => "⇧"
        };
        BadgeTitle.Text = title;
        BadgeDetail.Text = detail;
        BadgeDetail.Visibility = string.IsNullOrWhiteSpace(detail)
            ? Visibility.Collapsed
            : Visibility.Visible;
        BadgeThumbnail.Background = preview is null
            ? null
            : new ImageBrush(preview) { Stretch = Stretch.UniformToFill };
        BadgeThumbnailFrame.Visibility = preview is null ? Visibility.Collapsed : Visibility.Visible;
        BadgeThumbnailFrame.BorderBrush = accent;

        BuildStoryboard(kind, preview is not null);
        Show();
        _storyboard.Begin(this, true);
    }

    private bool TryTrackOwner(Window owner)
    {
        var width = owner.ActualWidth;
        var height = owner.ActualHeight;
        if (owner.WindowState == WindowState.Minimized || width < 1 || height < 1) return false;
        var topLeft = owner.PointToScreen(new Point(0, 0));
        var source = PresentationSource.FromVisual(owner)?.CompositionTarget;
        var scaleX = source?.TransformToDevice.M11 ?? 1.0;
        var scaleY = source?.TransformToDevice.M22 ?? 1.0;
        Left = topLeft.X / (scaleX <= 0 ? 1.0 : scaleX);
        Top = topLeft.Y / (scaleY <= 0 ? 1.0 : scaleY);
        Width = width;
        Height = height;
        return true;
    }

    private void BuildStoryboard(HandoffEffectKind kind, bool hasPreview)
    {
        _storyboard.Stop(this);
        _storyboard.Children.Clear();

        var hold = hasPreview ? PreviewHold : CueHold;

        // Offering pushes outward; claiming pulls inward. Terminal states settle.
        var (ringFrom, ringTo) = kind switch
        {
            HandoffEffectKind.Claiming => (2.6, 0.55),
            HandoffEffectKind.Offering => (0.35, 2.6),
            _ => (1.35, 0.95)
        };
        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };

        AddDouble(Root, UIElement.OpacityProperty, 0.0, 1.0, TimeSpan.Zero,
            TimeSpan.FromMilliseconds(140));
        AddDouble(Root, UIElement.OpacityProperty, 1.0, 0.0, hold, FadeOut);

        AddDouble(Ring, UIElement.OpacityProperty, 0.0, 0.85, TimeSpan.Zero,
            TimeSpan.FromMilliseconds(120));
        AddDouble(Ring, UIElement.OpacityProperty, 0.85, 0.0, TimeSpan.FromMilliseconds(420),
            TimeSpan.FromMilliseconds(520));
        AddScale(RingScale, ringFrom, ringTo, TimeSpan.Zero, TimeSpan.FromMilliseconds(900), ease);

        AddDouble(RingTrail, UIElement.OpacityProperty, 0.0, 0.5,
            TimeSpan.FromMilliseconds(110), TimeSpan.FromMilliseconds(120));
        AddDouble(RingTrail, UIElement.OpacityProperty, 0.5, 0.0,
            TimeSpan.FromMilliseconds(480), TimeSpan.FromMilliseconds(520));
        AddScale(RingTrailScale, ringFrom, ringTo, TimeSpan.FromMilliseconds(110),
            TimeSpan.FromMilliseconds(900), ease);

        AddDouble(EdgeGlow, UIElement.OpacityProperty, 0.0, 0.85, TimeSpan.Zero,
            TimeSpan.FromMilliseconds(160));
        AddDouble(EdgeGlow, UIElement.OpacityProperty, 0.85, 0.0, TimeSpan.FromMilliseconds(560),
            TimeSpan.FromMilliseconds(560));

        AddScale(BadgeScale, 0.9, 1.0, TimeSpan.Zero, TimeSpan.FromMilliseconds(260),
            new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.4 });

        _storyboard.Duration = new Duration(hold + FadeOut);
    }

    private void AddDouble(DependencyObject target, DependencyProperty property,
        double from, double to, TimeSpan begin, TimeSpan duration)
    {
        var animation = new DoubleAnimation(from, to, new Duration(duration))
        {
            BeginTime = begin
        };
        Storyboard.SetTarget(animation, target);
        Storyboard.SetTargetProperty(animation, new PropertyPath(property));
        _storyboard.Children.Add(animation);
    }

    private void AddScale(ScaleTransform target, double from, double to,
        TimeSpan begin, TimeSpan duration, IEasingFunction ease)
    {
        foreach (var property in new[] { ScaleTransform.ScaleXProperty, ScaleTransform.ScaleYProperty })
        {
            var animation = new DoubleAnimation(from, to, new Duration(duration))
            {
                BeginTime = begin,
                EasingFunction = ease
            };
            Storyboard.SetTarget(animation, target);
            Storyboard.SetTargetProperty(animation, new PropertyPath(property));
            _storyboard.Children.Add(animation);
        }
    }

    private static Brush ThemeBrush(string key) => (Brush)Application.Current.Resources[key];

    private void MakeClickThrough(object? sender, EventArgs e)
    {
        var handle = new WindowInteropHelper(this).Handle;
        if (handle == IntPtr.Zero) return;
        var style = GetWindowLong(handle, GwlExStyle);
        SetWindowLong(handle, GwlExStyle, style | WsExTransparent | WsExNoActivate | WsExToolWindow);
    }

    [DllImport("user32.dll", EntryPoint = "GetWindowLongW")]
    private static extern int GetWindowLong(IntPtr hWnd, int index);

    [DllImport("user32.dll", EntryPoint = "SetWindowLongW")]
    private static extern int SetWindowLong(IntPtr hWnd, int index, int newStyle);
}

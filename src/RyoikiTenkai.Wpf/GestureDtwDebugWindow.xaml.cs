using System.Windows;
using System.Windows.Media;
using System.Windows.Threading;
using RyoikiTenkai.Wpf.Interaction;
using RyoikiTenkai.Wpf.Native;

namespace RyoikiTenkai.Wpf;

public partial class GestureDtwDebugWindow : Window
{
    private const double BreakdownBarWidth = 168.0;

    private static readonly string[] BreakdownNames =
    [
        "Joint", "Bone", "Curl", "Finger state", "Spacing", "Motion",
        "Palm turn", "Depth", "Handedness", "Size", "Translation", "Total"
    ];

    private readonly NativeVisionHost _visionHost;
    private readonly GestureCatalog _catalog;
    private readonly DispatcherTimer _pollTimer;
    private uint _shownTemplateId = uint.MaxValue;

    internal GestureDtwDebugWindow(NativeVisionHost visionHost, GestureCatalog catalog)
    {
        InitializeComponent();
        _visionHost = visionHost;
        _catalog = catalog;
        DtwViewport.Attach(visionHost);
        _pollTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(200) };
        _pollTimer.Tick += PollTimer_Tick;
        _pollTimer.Start();
    }

    /// <summary>Label/value row for the facts and breakdown lists.</summary>
    private sealed record InfoRow(string Label, string Value);

    private sealed record BreakdownRow(string Label, string Value, double BarWidth, Brush Fill);

    private void PollTimer_Tick(object? sender, EventArgs e)
    {
        if (!_visionHost.TryGetGestureDtwDebug(out var snapshot) || snapshot.FrameId == 0) return;
        RenderMetadata(snapshot);
    }

    private unsafe void RenderMetadata(NativeGestureDtwDebugSnapshot snapshot)
    {
        var eligible = snapshot.Eligible != 0;
        var compared = snapshot.PathCount > 0;

        // The recognizer picks the template, so the identity of what is drawn
        // changes without warning. Name it, do not leave a bare numeric ID.
        if (snapshot.TemplateId != _shownTemplateId)
        {
            _shownTemplateId = snapshot.TemplateId;
            GestureNameText.Text = snapshot.TemplateId == 0
                ? "No gesture compared"
                : _catalog.NameOf(snapshot.TemplateId, $"Unknown gesture {snapshot.TemplateId}");
            GestureSubText.Text = snapshot.TemplateId == 0
                ? "The recognizer has not reached the DTW stage on this frame."
                : $"definition {snapshot.TemplateId} · chosen by the recognizer as the closest saved gesture";
        }

        DecisionText.Text = eligible ? "ELIGIBLE MATCH" : compared ? "REJECTED AFTER DTW" : "REJECTED BEFORE DTW";
        var decisionBrush = eligible ? ThemeBrush("Success") : compared ? ThemeBrush("Warning") : ThemeBrush("Danger");
        DecisionText.Foreground = decisionBrush;
        DecisionBadge.BorderBrush = decisionBrush;

        var confidence = Math.Clamp(snapshot.Confidence * 100.0F, 0.0F, 100.0F);
        ConfidenceBar.Value = confidence;
        ConfidenceBar.Foreground = decisionBrush;
        ConfidenceValueText.Text = $"{confidence:0.0}%";
        ConfidenceValueText.Foreground = decisionBrush;
        ThresholdText.Text = $"threshold {snapshot.ThresholdScore:0.0000} · score {snapshot.Score:0.0000}";
        ReasonText.Text = eligible ? "All gates passed." : snapshot.GetReason();
        ReasonText.Foreground = eligible ? ThemeBrush("Success") : ThemeBrush("Warning");

        FactsList.ItemsSource = new[]
        {
            new InfoRow("Frame", snapshot.FrameId.ToString()),
            new InfoRow("Candidate window", $"{snapshot.CandidateDurationMs} ms"),
            new InfoRow("DTW path", compared ? $"{snapshot.PathCount} points" : "not reached"),
            new InfoRow("Warp ratio", snapshot.HasWarpRatio != 0 ? $"{snapshot.WarpRatio:0.00}" : "n/a"),
            new InfoRow("Reverse score", snapshot.HasReverseScore != 0 ? $"{snapshot.ReverseScore:0.0000}" : "n/a")
        };

        float* values = snapshot.ScoreBreakdown;
        // Bars are relative to the largest term, so the dominant contributor is
        // obvious even when every absolute value is small.
        var largest = 0.0F;
        for (var index = 0; index < BreakdownNames.Length - 1; ++index)
            largest = Math.Max(largest, Math.Abs(values[index]));
        var rows = new List<BreakdownRow>(BreakdownNames.Length);
        for (var index = 0; index < BreakdownNames.Length; ++index)
        {
            var isTotal = index == BreakdownNames.Length - 1;
            var magnitude = Math.Abs(values[index]);
            var fraction = largest > 0.0F ? Math.Clamp(magnitude / largest, 0.0F, 1.0F) : 0.0F;
            rows.Add(new BreakdownRow(
                BreakdownNames[index],
                values[index].ToString("0.000"),
                isTotal ? BreakdownBarWidth : BreakdownBarWidth * fraction,
                isTotal ? decisionBrush
                    : fraction > 0.85F ? ThemeBrush("Warning") : ThemeBrush("Accent")));
        }
        BreakdownList.ItemsSource = rows;
    }

    private static Brush ThemeBrush(string key) => (Brush)Application.Current.Resources[key];

    private void CloseButton_Click(object sender, RoutedEventArgs e) => Close();

    protected override void OnClosed(EventArgs e)
    {
        _pollTimer.Stop();
        base.OnClosed(e);
    }
}

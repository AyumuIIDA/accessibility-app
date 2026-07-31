using System.Text;
using System.Windows;
using System.Windows.Media;
using System.Windows.Threading;
using RyoikiTenkai.Wpf.Native;

namespace RyoikiTenkai.Wpf;

public partial class GestureDtwDebugWindow : Window
{
    private static readonly string[] BreakdownNames =
    [
        "Joint", "Bone", "Curl", "Finger state", "Spacing", "Motion",
        "Palm turn", "Depth", "Handedness", "Size", "Translation", "Total"
    ];

    private readonly NativeVisionHost _visionHost;
    private readonly DispatcherTimer _pollTimer;

    internal GestureDtwDebugWindow(NativeVisionHost visionHost)
    {
        InitializeComponent();
        _visionHost = visionHost;
        DtwViewport.Attach(visionHost);
        _pollTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(200) };
        _pollTimer.Tick += PollTimer_Tick;
        _pollTimer.Start();
    }

    private void PollTimer_Tick(object? sender, EventArgs e)
    {
        if (!_visionHost.TryGetGestureDtwDebug(out var snapshot) || snapshot.FrameId == 0) return;
        RenderMetadata(snapshot);
    }

    private unsafe void RenderMetadata(NativeGestureDtwDebugSnapshot snapshot)
    {
        var eligible = snapshot.Eligible != 0;
        DecisionText.Text = eligible
            ? "ELIGIBLE MATCH"
            : snapshot.PathCount > 0 ? "DTW COMPARED — REJECTED" : "REJECTED BEFORE DTW";
        DecisionText.Foreground = eligible
            ? Brushes.LightGreen : snapshot.PathCount > 0 ? Brushes.Gold : Brushes.OrangeRed;
        ConfidenceBar.Value = Math.Clamp(snapshot.Confidence * 100.0F, 0.0F, 100.0F);
        FactsText.Text = $"frame {snapshot.FrameId}\n"
            + $"template {snapshot.TemplateId} | window {snapshot.CandidateDurationMs} ms\n"
            + $"score {snapshot.Score:0.0000} | threshold {snapshot.ThresholdScore:0.0000}\n"
            + $"confidence {snapshot.Confidence * 100:0.0}% | path {snapshot.PathCount} points\n"
            + (snapshot.HasWarpRatio != 0 ? $"warp ratio {snapshot.WarpRatio:0.00}" : "warp ratio n/a")
            + (snapshot.HasReverseScore != 0 ? $" | reverse {snapshot.ReverseScore:0.0000}" : string.Empty);
        ReasonText.Text = eligible ? "All gates passed." : snapshot.GetReason();

        var builder = new StringBuilder(256);
        float* values = snapshot.ScoreBreakdown;
        for (var index = 0; index < BreakdownNames.Length; ++index)
            builder.Append(BreakdownNames[index].PadRight(14)).Append(' ')
                .AppendLine(values[index].ToString("0.000"));
        BreakdownText.Text = builder.ToString();
    }

    private void CloseButton_Click(object sender, RoutedEventArgs e) => Close();

    protected override void OnClosed(EventArgs e)
    {
        _pollTimer.Stop();
        base.OnClosed(e);
    }
}

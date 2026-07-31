using Microsoft.Win32;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using RyoikiTenkai.Wpf.Native;

namespace RyoikiTenkai.Wpf;

public partial class GestureRecordingPlaybackWindow : Window
{
    private sealed record TakeItem(NativeGestureRecordingMetadata Metadata)
    {
        public override string ToString() => $"Take {Metadata.TakeIndex}";
    }

    private readonly NativeVisionHost _visionHost;
    private readonly uint _definitionId;
    private readonly DispatcherTimer _timer;
    private bool _updatingSlider;
    private bool _playing;

    internal GestureRecordingPlaybackWindow(
        NativeVisionHost visionHost, uint definitionId, string definitionName)
    {
        InitializeComponent();
        _visionHost = visionHost;
        _definitionId = definitionId;
        TitleText.Text = definitionName;
        PlaybackHost.DiagnosticLogged += message => PlaybackErrorText.Text = message;
        PlaybackHost.Attach(visionHost);
        LoadTakes();
        _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
        _timer.Tick += Timer_Tick;
        _timer.Start();
    }

    private void LoadTakes()
    {
        if (!_visionHost.TryListGestureRecordings(_definitionId, out var recordings))
        {
            PlaybackErrorText.Text = "Recorded takes could not be read.";
            return;
        }
        TakesList.Items.Clear();
        var count = (int)Math.Min(recordings.Count, (uint)NativeVisionInterop.MaxGestureRecordings);
        for (var index = 0; index < count; ++index)
            TakesList.Items.Add(new TakeItem(recordings.GetItem(index)));
        PlaybackErrorText.Text = recordings.GetError();
        if (count > 0) TakesList.SelectedIndex = 0;
    }

    private void TakesList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (TakesList.SelectedItem is not TakeItem item) return;
        var value = item.Metadata;
        var captured = DateTimeOffset.FromUnixTimeMilliseconds((long)(value.CapturedAtUs / 1000));
        TakeFactsText.Text = $"Captured  {captured.LocalDateTime:g}\n"
            + $"Duration  {value.DurationMs / 1000.0:0.00} s\n"
            + $"Frames  {value.FrameCount} ({value.ValidLandmarkFrameCount} valid)\n"
            + $"Usable FPS  {value.UsableEffectiveFps:0.0}\n"
            + $"Average confidence  {value.AverageConfidence:P0}\n\n"
            + (string.IsNullOrWhiteSpace(value.GetQualityReason()) ? "Accepted" : value.GetQualityReason());
        _playing = false;
        PlayPauseButton.Content = "Play";
        if (!PlaybackHost.Select(_definitionId, value.TakeIndex))
            PlaybackErrorText.Text = "This take could not be loaded for playback.";
    }

    private void PlayPauseButton_Click(object sender, RoutedEventArgs e)
    {
        _playing = !_playing;
        if (!PlaybackHost.SetPlaying(_playing)) _playing = false;
        PlayPauseButton.Content = _playing ? "Pause" : "Play";
    }

    private void PlaybackSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_updatingSlider || !IsLoaded) return;
        PlaybackHost.Seek((float)e.NewValue);
    }

    private void Timer_Tick(object? sender, EventArgs e)
    {
        if (!PlaybackHost.TryGetStatus(out var status)) return;
        _updatingSlider = true;
        PlaybackSlider.Value = Math.Clamp(status.NormalizedProgress, 0.0F, 1.0F);
        _updatingSlider = false;
        _playing = status.Playing != 0;
        PlayPauseButton.Content = _playing ? "Pause" : "Play";
        PlaybackPositionText.Text = $"{status.PositionMs / 1000.0:0.0} / {status.DurationMs / 1000.0:0.0} s";
    }

    private void ExportButton_Click(object sender, RoutedEventArgs e)
    {
        if (TakesList.SelectedItem is not TakeItem item) return;
        var dialog = new SaveFileDialog
        {
            Title = "Export gesture recording",
            Filter = "Ryoiki gesture recording (*.ryoiki-recording)|*.ryoiki-recording|All files (*.*)|*.*",
            FileName = $"gesture-{_definitionId}-take-{item.Metadata.TakeIndex}.ryoiki-recording"
        };
        if (dialog.ShowDialog(this) != true) return;
        PlaybackErrorText.Text = _visionHost.ExportGestureRecording(
            _definitionId, item.Metadata.TakeIndex, dialog.FileName)
            ? "Recording exported successfully."
            : "Recording export failed: " + _visionHost.GetLastErrorMessage();
    }

    private void CloseButton_Click(object sender, RoutedEventArgs e) => Close();
    protected override void OnClosed(EventArgs e)
    {
        _timer.Stop();
        PlaybackHost.SetPlaying(false);
        base.OnClosed(e);
    }
}

using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using System.Threading;
using RyoikiTenkai.Core;
using RyoikiTenkai.Vision;

namespace RyoikiTenkai.Wpf;

public partial class SkeletonVideoDebugView : UserControl
{
    private readonly DispatcherTimer _playbackTimer;
    private IReadOnlyList<GestureDebugFrame> _frames = [];
    private GestureDebugFrame? _selectedFrame;
    private bool _liveFollow = true;
    private bool _isPlaying;
    private double _playbackSpeed = 1.0;
    private double _timelinePixelsPerFrame = 2.0;
    private long _droppedFrames;
    private string _recordingPath = string.Empty;

    public SkeletonVideoDebugView()
    {
        InitializeComponent();
        _playbackTimer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(33)
        };
        _playbackTimer.Tick += PlaybackTimer_Tick;
        Timeline.FrameSelected += Timeline_FrameSelected;
    }

    internal void UpdateSession(
        IReadOnlyList<GestureDebugFrame> frames,
        GestureDebugFrame? selectedFrame,
        bool liveFollow,
        long droppedFrames,
        string recordingPath)
    {
        _frames = frames;
        _droppedFrames = droppedFrames;
        _recordingPath = recordingPath;
        if (!liveFollow && !_isPlaying)
        {
            _liveFollow = false;
        }
        if (_liveFollow || _selectedFrame is null)
        {
            _selectedFrame = selectedFrame ?? _frames.LastOrDefault();
        }
        else if (_selectedFrame is not null
            && !_frames.Any(x => x.SequenceNumber == _selectedFrame.SequenceNumber))
        {
            _selectedFrame = _frames.LastOrDefault();
        }

        Render();
    }

    internal void Clear()
    {
        StopPlayback();
        _frames = [];
        _selectedFrame = null;
        _liveFollow = true;
        _droppedFrames = 0;
        _recordingPath = string.Empty;
        Render();
    }

    private void Timeline_FrameSelected(int frameIndex)
    {
        if (_frames.Count == 0)
        {
            return;
        }

        StopPlayback();
        _liveFollow = false;
        _selectedFrame = _frames[Math.Clamp(frameIndex, 0, _frames.Count - 1)];
        Render();
    }

    private void PlaybackTimer_Tick(object? sender, EventArgs e)
    {
        if (_frames.Count == 0)
        {
            StopPlayback();
            return;
        }

        var index = SelectedIndex();
        if (index >= _frames.Count - 1)
        {
            StopPlayback();
            return;
        }

        _selectedFrame = _frames[index + 1];
        Render();
    }

    private void PlayPause_Click(object sender, RoutedEventArgs e)
    {
        if (_isPlaying)
        {
            StopPlayback();
            return;
        }

        if (_frames.Count == 0)
        {
            return;
        }

        _liveFollow = false;
        _isPlaying = true;
        PlayPauseButton.Content = "Pause";
        _playbackTimer.Start();
        Render();
    }

    private void StepBack_Click(object sender, RoutedEventArgs e)
    {
        StopPlayback();
        SelectRelative(-1);
    }

    private void StepForward_Click(object sender, RoutedEventArgs e)
    {
        StopPlayback();
        SelectRelative(1);
    }

    private void JumpStart_Click(object sender, RoutedEventArgs e)
    {
        StopPlayback();
        SelectIndex(0);
    }

    private void JumpEnd_Click(object sender, RoutedEventArgs e)
    {
        StopPlayback();
        SelectIndex(_frames.Count - 1);
    }

    private void PreviousEvent_Click(object sender, RoutedEventArgs e)
    {
        StopPlayback();
        JumpEvent(-1);
    }

    private void NextEvent_Click(object sender, RoutedEventArgs e)
    {
        StopPlayback();
        JumpEvent(1);
    }

    private void Live_Click(object sender, RoutedEventArgs e)
    {
        StopPlayback();
        _liveFollow = true;
        _selectedFrame = _frames.LastOrDefault();
        Render();
    }

    private void SpeedCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (SpeedCombo.SelectedItem is ComboBoxItem { Tag: string tag }
            && double.TryParse(tag, out var speed))
        {
            _playbackSpeed = Math.Clamp(speed, 0.25, 4.0);
            if (_playbackTimer is not null)
            {
                _playbackTimer.Interval = TimeSpan.FromMilliseconds(33 / _playbackSpeed);
            }
        }
    }

    private void TimelineZoomSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        _timelinePixelsPerFrame = Math.Clamp(e.NewValue, 1.0, 24.0);
        if (Timeline is null)
        {
            return;
        }

        Timeline.SetZoom(_timelinePixelsPerFrame);
        Render();
    }

    private void SelectRelative(int delta)
    {
        SelectIndex(SelectedIndex() + delta);
    }

    private void SelectIndex(int index)
    {
        if (_frames.Count == 0)
        {
            return;
        }

        _liveFollow = false;
        _selectedFrame = _frames[Math.Clamp(index, 0, _frames.Count - 1)];
        Render();
    }

    private void JumpEvent(int direction)
    {
        if (_frames.Count == 0)
        {
            return;
        }

        var start = SelectedIndex();
        for (var index = start + direction; index >= 0 && index < _frames.Count; index += direction)
        {
            if (IsEventFrame(_frames[index]))
            {
                SelectIndex(index);
                return;
            }
        }
    }

    private void StopPlayback()
    {
        _playbackTimer.Stop();
        _isPlaying = false;
        PlayPauseButton.Content = "Play";
    }

    private int SelectedIndex()
    {
        if (_frames.Count == 0 || _selectedFrame is null)
        {
            return 0;
        }

        var index = _frames
            .Select((frame, frameIndex) => (frame, frameIndex))
            .FirstOrDefault(x => x.frame.SequenceNumber == _selectedFrame.SequenceNumber)
            .frameIndex;
        return Math.Clamp(index, 0, _frames.Count - 1);
    }

    private void Render()
    {
        var selectedIndex = SelectedIndex();
        var selected = _selectedFrame;
        Viewport.SetFrame(selected, CreateGhostTrail(selectedIndex));
        Timeline.SetZoom(_timelinePixelsPerFrame);
        Timeline.SetFrames(_frames, selectedIndex);
        ScrollSelectedTimelineFrameIntoView(selectedIndex);
        WindowStrip.SetSnapshot(selected?.Snapshot);

        var frameText = selected is null
            ? "No skeleton frames"
            : $"{selected.Kind} frame {selectedIndex + 1}/{Math.Max(1, _frames.Count)}";
        ViewportTitleText.Text = frameText;
        ViewportSubtitleText.Text = selected is null
            ? "Start camera/perception to record skeleton video"
            : $"{selected.Timestamp:HH:mm:ss.fff}  seq #{selected.SequenceNumber}  {_frames.Count} frames in memory";

        RecordingText.Text = string.IsNullOrWhiteSpace(_recordingPath)
            ? $"memory frames={_frames.Count} dropped={_droppedFrames}"
            : $"saving every frame -> {System.IO.Path.GetFileName(_recordingPath)} | memory={_frames.Count} dropped={_droppedFrames}";
        LiveButton.FontWeight = _liveFollow ? FontWeights.Bold : FontWeights.Normal;
        FillInspector(selected, selectedIndex);
    }

    private void ScrollSelectedTimelineFrameIntoView(int selectedIndex)
    {
        if (_frames.Count == 0 || TimelineScrollViewer.ViewportWidth <= 0)
        {
            return;
        }

        var selectedX = selectedIndex * _timelinePixelsPerFrame;
        var left = TimelineScrollViewer.HorizontalOffset;
        var right = left + TimelineScrollViewer.ViewportWidth;
        if (selectedX < left)
        {
            TimelineScrollViewer.ScrollToHorizontalOffset(Math.Max(0, selectedX - 32));
        }
        else if (selectedX > right - 32)
        {
            TimelineScrollViewer.ScrollToHorizontalOffset(Math.Max(0, selectedX - TimelineScrollViewer.ViewportWidth + 32));
        }
    }

    private IReadOnlyList<GestureDebugFrame> CreateGhostTrail(int selectedIndex)
    {
        if (_frames.Count == 0)
        {
            return [];
        }

        var start = Math.Max(0, selectedIndex - 15);
        return _frames.Skip(start).Take(Math.Max(0, selectedIndex - start)).ToList();
    }

    private void FillInspector(GestureDebugFrame? frame, int selectedIndex)
    {
        ScoresList.Items.Clear();
        if (frame is null)
        {
            FrameFactsText.Text = "-";
            DecisionText.Text = "-";
            ReasonText.Text = "-";
            return;
        }

        var sample = frame.Sample;
        FrameFactsText.Text = sample is null
            ? $"#{selectedIndex + 1} seq={frame.SequenceNumber} no-hand event\n{frame.Timestamp:HH:mm:ss.fff}"
            : $"#{selectedIndex + 1} seq={frame.SequenceNumber} {frame.Timestamp:HH:mm:ss.fff}\nconfidence={sample.Confidence:0.000} handedness={sample.Handedness:0.000} source={frame.Source}";

        var snapshot = frame.Snapshot;
        DecisionText.Text = snapshot.ConfirmedMatch is not null
            ? $"CONFIRMED {snapshot.ConfirmedMatch.DisplayName} ({snapshot.ConfirmedMatch.Confidence:0.00})"
            : snapshot.BestDisplayName is not null
                ? $"{snapshot.TriggerState}: {snapshot.BestDisplayName} ({snapshot.BestConfidence:0.00})"
                : snapshot.TriggerState;

        ReasonText.Text = FirstNonEmpty(
            frame.NoHandReason,
            snapshot.RejectionReason,
            snapshot.CandidateFailureReason,
            frame.ClosestScore?.Reason,
            "No rejection reason reported.");

        ScoresList.Items.Add($"path={snapshot.DetectedPath} motion={snapshot.MotionScore:0.000}");
        ScoresList.Items.Add($"window={snapshot.UsableFrameCount}/{snapshot.BufferFrameCount} usable fps={snapshot.UsableEffectiveFps:0.0}");
        if (snapshot.DtwWarpRatio is not null)
        {
            ScoresList.Items.Add($"dtw={snapshot.DtwScore:0.000} warp={snapshot.DtwWarpRatio:0.00}");
        }

        foreach (var score in snapshot.Scores
            .OrderByDescending(x => x.IsBest)
            .ThenByDescending(x => x.Confidence)
            .Take(20))
        {
            var marker = score.IsBest ? "* " : "  ";
            var template = score.TemplateIndex < 0 ? "-" : score.TemplateIndex.ToString();
            ScoresList.Items.Add(
                $"{marker}{score.DisplayName} {score.Kind} t{template} conf={score.Confidence:0.00} score={FormatScore(score.Score)} {score.Reason}");
        }
    }

    private static bool IsEventFrame(GestureDebugFrame frame)
    {
        return frame.Kind == GestureDebugFrameKind.NoHand
            || frame.Snapshot.ConfirmedMatch is not null
            || frame.Recognition is not null
            || !string.IsNullOrWhiteSpace(frame.Snapshot.RejectionReason)
            || frame.Snapshot.CandidateTemplate is not null;
    }

    private static string FirstNonEmpty(params string?[] values)
    {
        return values.FirstOrDefault(x => !string.IsNullOrWhiteSpace(x)) ?? string.Empty;
    }

    private static string FormatScore(float score)
    {
        return !float.IsFinite(score) || score > 999 ? "-" : score.ToString("0.000");
    }
}

public sealed class SkeletonViewport : FrameworkElement
{
    private static readonly (int Start, int End)[] Connections =
    [
        (0, 1), (1, 2), (2, 3), (3, 4),
        (0, 5), (5, 6), (6, 7), (7, 8),
        (5, 9), (9, 10), (10, 11), (11, 12),
        (9, 13), (13, 14), (14, 15), (15, 16),
        (13, 17), (17, 18), (18, 19), (19, 20),
        (0, 17)
    ];

    private GestureDebugFrame? _frame;
    private IReadOnlyList<GestureDebugFrame> _ghostTrail = [];

    internal void SetFrame(GestureDebugFrame? frame, IReadOnlyList<GestureDebugFrame> ghostTrail)
    {
        _frame = frame;
        _ghostTrail = ghostTrail;
        InvalidateVisual();
    }

    protected override void OnRender(DrawingContext drawingContext)
    {
        base.OnRender(drawingContext);
        var bounds = new Rect(0, 0, ActualWidth, ActualHeight);
        drawingContext.DrawRectangle(new SolidColorBrush(Color.FromRgb(5, 6, 8)), null, bounds);
        if (_frame?.Sample?.Landmarks is not { Count: >= 21 } landmarks)
        {
            DrawCenteredText(drawingContext, _frame?.NoHandReason ?? "No skeleton frame selected", bounds);
            return;
        }

        var allLandmarks = _ghostTrail
            .SelectMany(x => x.Sample?.Landmarks ?? [])
            .Concat(landmarks)
            .ToList();
        var mapper = CreateMapper(allLandmarks, bounds);
        for (var i = 0; i < _ghostTrail.Count; i++)
        {
            if (_ghostTrail[i].Sample?.Landmarks is { Count: >= 21 } ghost)
            {
                DrawSkeleton(drawingContext, ghost, mapper, 0.08 + (i / (double)Math.Max(1, _ghostTrail.Count)) * 0.22,
                    new SolidColorBrush(Color.FromRgb(74, 132, 205)), 1.4);
            }
        }

        DrawSkeleton(drawingContext, landmarks, mapper, 1.0, Brushes.Gold, 3.0);
    }

    private static void DrawSkeleton(
        DrawingContext drawingContext,
        IReadOnlyList<HandLandmark> landmarks,
        Func<HandLandmark, Point> map,
        double opacity,
        Brush brush,
        double thickness)
    {
        var pen = new Pen(brush.CloneCurrentValue(), thickness)
        {
            StartLineCap = PenLineCap.Round,
            EndLineCap = PenLineCap.Round
        };
        pen.Brush.Opacity = opacity;
        foreach (var (start, end) in Connections)
        {
            drawingContext.DrawLine(pen, map(landmarks[start]), map(landmarks[end]));
        }

        for (var i = 0; i < Math.Min(21, landmarks.Count); i++)
        {
            var point = map(landmarks[i]);
            var radius = i is 0 or 4 or 8 or 12 or 16 or 20 ? 5.2 : 3.5;
            var fill = i == 0 ? Brushes.DeepSkyBlue : Brushes.White;
            drawingContext.PushOpacity(opacity);
            drawingContext.DrawEllipse(fill, new Pen(Brushes.Black, 0.8), point, radius, radius);
            drawingContext.Pop();
        }
    }

    private static Func<HandLandmark, Point> CreateMapper(IReadOnlyList<HandLandmark> landmarks, Rect bounds)
    {
        var minX = landmarks.Min(x => x.X);
        var maxX = landmarks.Max(x => x.X);
        var minY = landmarks.Min(x => x.Y);
        var maxY = landmarks.Max(x => x.Y);
        var spanX = Math.Max(0.001f, maxX - minX);
        var spanY = Math.Max(0.001f, maxY - minY);
        var pad = Math.Min(bounds.Width, bounds.Height) * 0.08;
        var usableWidth = Math.Max(1, bounds.Width - (pad * 2));
        var usableHeight = Math.Max(1, bounds.Height - (pad * 2));
        var scale = Math.Min(usableWidth / spanX, usableHeight / spanY);
        var drawnWidth = spanX * scale;
        var drawnHeight = spanY * scale;
        var left = bounds.Left + (bounds.Width - drawnWidth) / 2;
        var top = bounds.Top + (bounds.Height - drawnHeight) / 2;
        return point => new Point(
            left + ((point.X - minX) * scale),
            top + ((point.Y - minY) * scale));
    }

    private static void DrawCenteredText(DrawingContext drawingContext, string text, Rect bounds)
    {
        var formatted = new FormattedText(
            text,
            Thread.CurrentThread.CurrentCulture,
            FlowDirection.LeftToRight,
            new Typeface("Segoe UI"),
            18,
            Brushes.Gray,
            VisualTreeHelper.GetDpi(Application.Current.MainWindow).PixelsPerDip);
        drawingContext.DrawText(
            formatted,
            new Point(bounds.Left + (bounds.Width - formatted.Width) / 2, bounds.Top + (bounds.Height - formatted.Height) / 2));
    }
}

public sealed class SkeletonTimeline : FrameworkElement
{
    private IReadOnlyList<GestureDebugFrame> _frames = [];
    private int _selectedIndex;
    private double _pixelsPerFrame = 2.0;

    internal event Action<int>? FrameSelected;

    public SkeletonTimeline()
    {
        MouseDown += OnMouseDown;
    }

    internal void SetFrames(IReadOnlyList<GestureDebugFrame> frames, int selectedIndex)
    {
        _frames = frames;
        _selectedIndex = Math.Clamp(selectedIndex, 0, Math.Max(0, frames.Count - 1));
        Width = Math.Max(ActualWidth, _frames.Count * _pixelsPerFrame);
        InvalidateVisual();
    }

    internal void SetZoom(double pixelsPerFrame)
    {
        _pixelsPerFrame = Math.Clamp(pixelsPerFrame, 1.0, 24.0);
        Width = Math.Max(ActualWidth, _frames.Count * _pixelsPerFrame);
        InvalidateVisual();
    }

    protected override void OnRender(DrawingContext drawingContext)
    {
        base.OnRender(drawingContext);
        var bounds = new Rect(0, 0, ActualWidth, ActualHeight);
        drawingContext.DrawRectangle(new SolidColorBrush(Color.FromRgb(15, 17, 21)), null, bounds);
        if (_frames.Count == 0)
        {
            return;
        }

        var rowTop = 8d;
        var rowHeight = Math.Max(16, bounds.Height - 18);
        var frameWidth = Math.Max(1, _pixelsPerFrame);
        for (var i = 0; i < _frames.Count; i++)
        {
            var rect = new Rect(i * frameWidth, rowTop, Math.Max(1, frameWidth - 0.5), rowHeight);
            drawingContext.DrawRectangle(FrameBrush(_frames[i]), null, rect);
        }

        var selectedX = _selectedIndex * frameWidth;
        drawingContext.DrawRectangle(null, new Pen(Brushes.White, 2), new Rect(selectedX, 2, Math.Max(3, frameWidth), bounds.Height - 4));
    }

    private void OnMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (_frames.Count == 0 || ActualWidth <= 0)
        {
            return;
        }

        var index = (int)Math.Floor(e.GetPosition(this).X / Math.Max(1, _pixelsPerFrame));
        FrameSelected?.Invoke(Math.Clamp(index, 0, _frames.Count - 1));
    }

    private static Brush FrameBrush(GestureDebugFrame frame)
    {
        if (frame.Kind == GestureDebugFrameKind.NoHand)
        {
            return Brushes.DimGray;
        }
        if (frame.Snapshot.ConfirmedMatch is not null)
        {
            return Brushes.LimeGreen;
        }
        if (frame.Recognition is not null)
        {
            return Brushes.Gold;
        }
        if (!string.IsNullOrWhiteSpace(frame.Snapshot.RejectionReason))
        {
            return Brushes.OrangeRed;
        }
        if (frame.Snapshot.CandidateTemplate is not null)
        {
            return Brushes.DarkOrange;
        }

        return Brushes.SteelBlue;
    }
}

public sealed class RecognizerWindowStrip : FrameworkElement
{
    private GestureRecognitionDebugSnapshot? _snapshot;

    internal void SetSnapshot(GestureRecognitionDebugSnapshot? snapshot)
    {
        _snapshot = snapshot;
        InvalidateVisual();
    }

    protected override void OnRender(DrawingContext drawingContext)
    {
        base.OnRender(drawingContext);
        drawingContext.DrawRectangle(new SolidColorBrush(Color.FromRgb(15, 17, 21)), null, new Rect(0, 0, ActualWidth, ActualHeight));
        var frames = _snapshot?.WindowFrames ?? [];
        if (frames.Count == 0)
        {
            return;
        }

        var maxTime = Math.Max(1, frames.Max(x => x.TimeOffsetMilliseconds));
        var frameWidth = Math.Max(2, ActualWidth / frames.Count);
        for (var i = 0; i < frames.Count; i++)
        {
            var frame = frames[i];
            var x = (frame.TimeOffsetMilliseconds / maxTime) * Math.Max(1, ActualWidth - frameWidth);
            var height = Math.Max(4, Math.Clamp(frame.Confidence, 0, 1) * Math.Max(1, ActualHeight - 4));
            var brush = frame.IsAcceptedMatch ? Brushes.LimeGreen : frame.IsUsable ? Brushes.DeepSkyBlue : Brushes.DimGray;
            drawingContext.DrawRectangle(
                brush,
                null,
                new Rect(x, ActualHeight - height, Math.Max(1, frameWidth - 1), height));
        }
    }
}

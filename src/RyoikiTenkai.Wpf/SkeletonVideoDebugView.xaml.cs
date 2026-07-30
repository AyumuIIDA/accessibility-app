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
        Viewport.SetFrame(selected, CreateGhostTrail(selectedIndex), _frames);
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
        var fingerPose = sample?.Landmarks is { Count: >= 21 } landmarks
            ? GestureFeatureExtractor.AnalyzeFingerPose(landmarks)
            : null;
        var fingerText = fingerPose is null
            ? string.Empty
            : $"\nfingers={GestureFeatureExtractor.FormatFingerMask(fingerPose.StateMask)} straight={FormatStraightness(fingerPose.Straightness)}";
        var featureText = sample?.Landmarks is { Count: >= 21 }
            ? "\n" + FormatFrameFeatures(sample)
            : string.Empty;
        FrameFactsText.Text = sample is null
            ? $"#{selectedIndex + 1} seq={frame.SequenceNumber} no-hand event\n{frame.Timestamp:HH:mm:ss.fff}"
            : $"#{selectedIndex + 1} seq={frame.SequenceNumber} {frame.Timestamp:HH:mm:ss.fff}\nconfidence={sample.Confidence:0.000} handedness={sample.Handedness:0.000} hands={frame.FrameSet?.Hands.Count ?? 1} source={frame.Source}{fingerText}{featureText}";

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
        if (snapshot.CandidateTemplate?.Topology is { } topology)
        {
            ScoresList.Items.Add(
                $"topology change={topology.TopologyChangeScore:0.000} travel={topology.PalmTravel:0.000} angle={topology.PalmOrientationRangeRadians * 180 / MathF.PI:0.0}deg handed={topology.HandednessRange:0.000}");
            ScoresList.Items.Add(
                $"palm turn={topology.PalmTurnScore:0.000} areaRange={topology.SignedPalmAreaRange:0.000} crossings={topology.SignedPalmAreaSignChanges} compressionDrop={topology.PalmCompressionDrop:0.000} depthMax={topology.PalmDepthRangeMax:0.000} fingerTransitions={topology.FingerStateTransitionCount} curlRange={topology.FingerStraightnessRangeMax:0.000}");
            ScoresList.Items.Add(
                $"hand side mean={topology.HandednessMean:0.000} sizeRange={topology.HandScaleRatioRange:0.000} sizeDelta={topology.HandScaleRatioDelta:0.000} bboxAreaDelta={topology.BoundingBoxAreaRatioDelta:0.000} translation=({topology.TranslationDeltaX:0.000},{topology.TranslationDeltaY:0.000}) dist={topology.TranslationDistance:0.000}");
        }
        if (snapshot.DtwWarpRatio is not null)
        {
            ScoresList.Items.Add($"dtw={snapshot.DtwScore:0.000} warp={snapshot.DtwWarpRatio:0.00}");
        }
        if (snapshot.ScoreBreakdown is { } breakdown)
        {
            ScoresList.Items.Add(
                $"score parts joint={breakdown.JointScore:0.000} bone={breakdown.BoneScore:0.000} curl={breakdown.CurlScore:0.000} finger={breakdown.FingerStateScore:0.000} spacing={breakdown.SpacingScore:0.000} motion={breakdown.MotionScore:0.000} palm={breakdown.PalmTurnScore:0.000} depth={breakdown.DepthScore:0.000} hand={breakdown.HandednessScore:0.000} size={breakdown.SizeScore:0.000} trans={breakdown.TranslationScore:0.000}");
        }

        foreach (var score in snapshot.Scores
            .OrderByDescending(x => x.IsBest)
            .ThenByDescending(x => x.Confidence)
            .Take(20))
        {
            var marker = score.IsBest ? "* " : "  ";
            var template = score.TemplateIndex < 0 ? "-" : score.TemplateIndex.ToString();
            var parts = score.Breakdown is null
                ? string.Empty
                : $" parts j{score.Breakdown.JointScore:0.00}/b{score.Breakdown.BoneScore:0.00}/c{score.Breakdown.CurlScore:0.00}/f{score.Breakdown.FingerStateScore:0.00}/s{score.Breakdown.SpacingScore:0.00}/m{score.Breakdown.MotionScore:0.00}/p{score.Breakdown.PalmTurnScore:0.00}/z{score.Breakdown.DepthScore:0.00}/h{score.Breakdown.HandednessScore:0.00}/sz{score.Breakdown.SizeScore:0.00}/t{score.Breakdown.TranslationScore:0.00}";
            ScoresList.Items.Add(
                $"{marker}{score.DisplayName} unified t{template} conf={score.Confidence:0.00} score={FormatScore(score.Score)}{parts} {score.Reason}");
        }
    }

    private static string FormatStraightness(IReadOnlyList<float> straightness)
    {
        return string.Join(",", straightness.Select(x => x.ToString("0.00")));
    }

    private static string FormatFrameFeatures(GestureFrameSample sample)
    {
        var track = GestureFeatureExtractor.BuildFeatureTrack([sample]);
        var feature = track.Frames.FirstOrDefault();
        if (feature is null)
        {
            return "features unavailable";
        }

        return
            $"palmArea={feature.SignedPalmArea:0.000} compression={feature.PalmCompression:0.000} " +
            $"zRange={feature.PalmDepthRange:0.000} bboxAspect={feature.BoundingBoxAspect:0.00} " +
            $"scale={feature.HandScale:0.0} scaleRatio={feature.HandScaleRatio:0.000} bboxAreaRatio={feature.BoundingBoxAreaRatio:0.000} " +
            $"translation=({feature.TranslationX:0.000},{feature.TranslationY:0.000})";
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
    private IReadOnlyList<GestureDebugFrame> _sessionFrames = [];
    private IReadOnlyList<GestureSkeletonFrame> _candidateFitFrames = [];
    private IReadOnlyList<GestureSkeletonFrame> _templateFitFrames = [];
    private IReadOnlyList<GestureDtwPoint> _fitPath = [];

    internal void SetFrame(
        GestureDebugFrame? frame,
        IReadOnlyList<GestureDebugFrame> ghostTrail,
        IReadOnlyList<GestureDebugFrame> sessionFrames)
    {
        _frame = frame;
        _ghostTrail = ghostTrail;
        _sessionFrames = sessionFrames;
        _candidateFitFrames = frame?.Snapshot.CandidateTemplate?.SkeletonFrames ?? [];
        _templateFitFrames = frame?.Snapshot.BestTemplate?.SkeletonFrames ?? [];
        _fitPath = frame?.Snapshot.DtwPath ?? [];
        InvalidateVisual();
    }

    protected override void OnRender(DrawingContext drawingContext)
    {
        base.OnRender(drawingContext);
        var bounds = new Rect(0, 0, ActualWidth, ActualHeight);
        drawingContext.DrawRectangle(new SolidColorBrush(Color.FromRgb(5, 6, 8)), null, bounds);
        var selectedHands = HandsForFrame(_frame).ToList();
        if (selectedHands.Count == 0)
        {
            DrawCenteredText(drawingContext, _frame?.NoHandReason ?? "No skeleton frame selected", bounds);
            return;
        }

        var allLandmarks = _sessionFrames
            .SelectMany(HandsForFrame)
            .SelectMany(x => x.Landmarks)
            .Concat(selectedHands.SelectMany(x => x.Landmarks))
            .ToList();
        var mapper = CreateMapper(allLandmarks, bounds);
        for (var i = 0; i < _ghostTrail.Count; i++)
        {
            foreach (var ghost in HandsForFrame(_ghostTrail[i]))
            {
                DrawSkeleton(drawingContext, ghost.Landmarks, mapper, 0.08 + (i / (double)Math.Max(1, _ghostTrail.Count)) * 0.22,
                    new SolidColorBrush(Color.FromRgb(74, 132, 205)), 1.4);
            }
        }

        for (var i = 0; i < selectedHands.Count; i++)
        {
            var brush = i == 0 ? Brushes.Gold : Brushes.DeepSkyBlue;
            DrawSkeleton(drawingContext, selectedHands[i].Landmarks, mapper, 1.0, brush, 3.0);
        }

        DrawTemplateFitInset(drawingContext, bounds);
    }

    private static IEnumerable<GestureFrameSample> HandsForFrame(GestureDebugFrame? frame)
    {
        if (frame?.FrameSet?.Hands is { Count: > 0 } hands)
        {
            return hands.Where(x => x.Landmarks.Count >= 21);
        }

        return frame?.Sample?.Landmarks is { Count: >= 21 } ? [frame.Sample] : [];
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
        var useCameraSpace = LooksLikeNormalizedCameraSpace(landmarks);
        var minX = useCameraSpace ? 0 : landmarks.Min(x => x.X);
        var maxX = useCameraSpace ? 1 : landmarks.Max(x => x.X);
        var minY = useCameraSpace ? 0 : landmarks.Min(x => x.Y);
        var maxY = useCameraSpace ? 1 : landmarks.Max(x => x.Y);
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
            left + drawnWidth - ((point.X - minX) * scale),
            top + ((point.Y - minY) * scale));
    }

    private static bool LooksLikeNormalizedCameraSpace(IReadOnlyList<HandLandmark> landmarks)
    {
        return landmarks.Count > 0
            && landmarks.All(point =>
                point.X >= -0.05f && point.X <= 1.05f
                && point.Y >= -0.05f && point.Y <= 1.05f);
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

    private void DrawTemplateFitInset(DrawingContext drawingContext, Rect bounds)
    {
        if (_candidateFitFrames.Count == 0 || _templateFitFrames.Count == 0)
        {
            return;
        }

        var width = Math.Min(360, bounds.Width * 0.34);
        var height = Math.Min(280, bounds.Height * 0.34);
        if (width < 180 || height < 140)
        {
            return;
        }

        var inset = new Rect(bounds.Right - width - 18, bounds.Top + 18, width, height);
        drawingContext.DrawRoundedRectangle(
            new SolidColorBrush(Color.FromArgb(210, 10, 12, 16)),
            new Pen(new SolidColorBrush(Color.FromRgb(58, 68, 84)), 1),
            inset,
            4,
            4);

        var title = new FormattedText(
            "Template fit",
            Thread.CurrentThread.CurrentCulture,
            FlowDirection.LeftToRight,
            new Typeface("Segoe UI"),
            13,
            Brushes.White,
            VisualTreeHelper.GetDpi(Application.Current.MainWindow).PixelsPerDip);
        drawingContext.DrawText(title, new Point(inset.Left + 10, inset.Top + 8));

        var plot = new Rect(inset.Left + 10, inset.Top + 34, inset.Width - 20, inset.Height - 44);
        var allPoints = _candidateFitFrames
            .SelectMany(x => x.Landmarks)
            .Concat(_templateFitFrames.SelectMany(x => x.Landmarks))
            .ToList();
        var mapper = CreateSkeletonMapper(allPoints, plot);

        foreach (var frame in SampleFitFrames(_templateFitFrames, 8))
        {
            DrawSkeletonFrame(drawingContext, frame, mapper, new SolidColorBrush(Color.FromRgb(156, 105, 255)), 0.20, 1.2);
        }

        foreach (var frame in SampleFitFrames(_candidateFitFrames, 8))
        {
            DrawSkeletonFrame(drawingContext, frame, mapper, new SolidColorBrush(Color.FromRgb(60, 150, 240)), 0.22, 1.2);
        }

        var candidateIndex = Math.Max(0, _candidateFitFrames.Count - 1);
        var templateIndex = FindMatchedTemplateIndex(candidateIndex);
        DrawSkeletonFrame(drawingContext, _templateFitFrames[Math.Clamp(templateIndex, 0, _templateFitFrames.Count - 1)], mapper, Brushes.LimeGreen, 0.90, 2.6);
        DrawSkeletonFrame(drawingContext, _candidateFitFrames[candidateIndex], mapper, Brushes.Gold, 1.0, 2.8);
    }

    private int FindMatchedTemplateIndex(int candidateIndex)
    {
        if (_fitPath.Count == 0)
        {
            return Math.Min(candidateIndex, _templateFitFrames.Count - 1);
        }

        return _fitPath
            .OrderBy(x => Math.Abs(x.CandidateIndex - candidateIndex))
            .ThenBy(x => x.TemplateIndex)
            .First()
            .TemplateIndex;
    }

    private static IEnumerable<GestureSkeletonFrame> SampleFitFrames(IReadOnlyList<GestureSkeletonFrame> frames, int maxFrames)
    {
        if (frames.Count <= maxFrames)
        {
            return frames;
        }

        return Enumerable.Range(0, maxFrames)
            .Select(index => frames[(int)Math.Round(index * (frames.Count - 1) / (double)Math.Max(1, maxFrames - 1))]);
    }

    private static Func<GestureSkeletonPoint, Point> CreateSkeletonMapper(IReadOnlyList<GestureSkeletonPoint> points, Rect bounds)
    {
        var minX = points.Min(x => x.X);
        var maxX = points.Max(x => x.X);
        var minY = points.Min(x => x.Y);
        var maxY = points.Max(x => x.Y);
        var spanX = Math.Max(0.001f, maxX - minX);
        var spanY = Math.Max(0.001f, maxY - minY);
        var pad = Math.Min(bounds.Width, bounds.Height) * 0.08;
        var scale = Math.Min(
            Math.Max(1, bounds.Width - (pad * 2)) / spanX,
            Math.Max(1, bounds.Height - (pad * 2)) / spanY);
        var drawnWidth = spanX * scale;
        var drawnHeight = spanY * scale;
        var left = bounds.Left + (bounds.Width - drawnWidth) / 2;
        var top = bounds.Top + (bounds.Height - drawnHeight) / 2;
        return point => new Point(
            left + drawnWidth - ((point.X - minX) * scale),
            top + ((point.Y - minY) * scale));
    }

    private static void DrawSkeletonFrame(
        DrawingContext drawingContext,
        GestureSkeletonFrame frame,
        Func<GestureSkeletonPoint, Point> map,
        Brush brush,
        double opacity,
        double thickness)
    {
        if (frame.Landmarks.Count < 21)
        {
            return;
        }

        var lineBrush = brush.CloneCurrentValue();
        lineBrush.Opacity = opacity;
        var pen = new Pen(lineBrush, thickness)
        {
            StartLineCap = PenLineCap.Round,
            EndLineCap = PenLineCap.Round
        };
        foreach (var (start, end) in Connections)
        {
            drawingContext.DrawLine(pen, map(frame.Landmarks[start]), map(frame.Landmarks[end]));
        }

        drawingContext.PushOpacity(opacity);
        foreach (var landmark in frame.Landmarks.Take(21))
        {
            drawingContext.DrawEllipse(brush, null, map(landmark), 2.2, 2.2);
        }

        drawingContext.Pop();
    }
}

public sealed class SkeletonTimeline : FrameworkElement
{
    private IReadOnlyList<GestureDebugFrame> _frames = [];
    private HashSet<long> _acceptedMatchSequenceNumbers = [];
    private IReadOnlyList<TimelineMatchSegment> _matchSegments = [];
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
        _matchSegments = BuildMatchSegments(frames);
        _acceptedMatchSequenceNumbers = _matchSegments
            .SelectMany(x => Enumerable.Range(x.StartIndex, x.EndIndex - x.StartIndex + 1))
            .Where(x => x >= 0 && x < frames.Count)
            .Select(x => frames[x].SequenceNumber)
            .ToHashSet();
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

        var segmentTop = 5d;
        var segmentHeight = 22d;
        var rowTop = 34d;
        var rowHeight = Math.Max(16, bounds.Height - rowTop - 8);
        var frameWidth = Math.Max(1, _pixelsPerFrame);
        DrawMatchSegments(drawingContext, bounds, frameWidth, segmentTop, segmentHeight);
        for (var i = 0; i < _frames.Count; i++)
        {
            var rect = new Rect(i * frameWidth, rowTop, Math.Max(1, frameWidth - 0.5), rowHeight);
            drawingContext.DrawRectangle(FrameBrush(_frames[i], _acceptedMatchSequenceNumbers), null, rect);
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

    private static IReadOnlyList<TimelineMatchSegment> BuildMatchSegments(IReadOnlyList<GestureDebugFrame> frames)
    {
        var result = new List<TimelineMatchSegment>();
        var handFrames = frames
            .Select((frame, index) => (frame, index))
            .Where(x => x.frame.Kind == GestureDebugFrameKind.Hand)
            .OrderBy(x => x.frame.Timestamp)
            .ToList();
        foreach (var confirmed in frames
            .Select((frame, index) => (frame, index))
            .Where(x => x.frame.Snapshot.ConfirmedMatch is not null))
        {
            var acceptedWindowFrames = confirmed.frame.Snapshot.WindowFrames?.Where(x => x.IsAcceptedMatch).ToList() ?? [];
            if (acceptedWindowFrames.Count == 0)
            {
                result.Add(new TimelineMatchSegment(
                    confirmed.index,
                    confirmed.index,
                    confirmed.frame.Snapshot.ConfirmedMatch!.DisplayName,
                    confirmed.frame.Snapshot.ConfirmedMatch!.Confidence,
                    confirmed.frame.Timestamp));
                continue;
            }

            var oldestAccepted = acceptedWindowFrames.Max(x => x.AgeMilliseconds);
            var newestAccepted = acceptedWindowFrames.Min(x => x.AgeMilliseconds);
            var rangeStart = confirmed.frame.Timestamp - TimeSpan.FromMilliseconds(oldestAccepted + 90);
            var rangeEnd = confirmed.frame.Timestamp - TimeSpan.FromMilliseconds(Math.Max(0, newestAccepted - 90));
            var matchedFrames = handFrames
                .Where(x => x.frame.Timestamp >= rangeStart && x.frame.Timestamp <= rangeEnd)
                .ToList();
            if (matchedFrames.Count == 0)
            {
                foreach (var windowFrame in acceptedWindowFrames)
                {
                    var timestamp = confirmed.frame.Timestamp - TimeSpan.FromMilliseconds(windowFrame.AgeMilliseconds);
                    var nearest = handFrames
                        .OrderBy(x => Math.Abs((x.frame.Timestamp - timestamp).TotalMilliseconds))
                        .FirstOrDefault();
                    if (nearest.frame is not null && Math.Abs((nearest.frame.Timestamp - timestamp).TotalMilliseconds) <= 90)
                    {
                        matchedFrames.Add(nearest);
                    }
                }
            }

            if (matchedFrames.Count == 0)
            {
                result.Add(new TimelineMatchSegment(
                    confirmed.index,
                    confirmed.index,
                    confirmed.frame.Snapshot.ConfirmedMatch!.DisplayName,
                    confirmed.frame.Snapshot.ConfirmedMatch!.Confidence,
                    confirmed.frame.Timestamp));
                continue;
            }

            result.Add(new TimelineMatchSegment(
                matchedFrames.Min(x => x.index),
                matchedFrames.Max(x => x.index),
                confirmed.frame.Snapshot.ConfirmedMatch!.DisplayName,
                confirmed.frame.Snapshot.ConfirmedMatch!.Confidence,
                confirmed.frame.Timestamp));
        }

        return MergeAdjacentSegments(result);
    }

    private static IReadOnlyList<TimelineMatchSegment> MergeAdjacentSegments(IReadOnlyList<TimelineMatchSegment> segments)
    {
        var result = new List<TimelineMatchSegment>();
        foreach (var segment in segments.OrderBy(x => x.StartIndex).ThenBy(x => x.EndIndex))
        {
            var previous = result.LastOrDefault();
            if (previous is not null
                && StringComparer.OrdinalIgnoreCase.Equals(previous.DisplayName, segment.DisplayName)
                && segment.StartIndex <= previous.EndIndex + 3)
            {
                result[^1] = previous with
                {
                    EndIndex = Math.Max(previous.EndIndex, segment.EndIndex),
                    Confidence = Math.Max(previous.Confidence, segment.Confidence),
                    ConfirmedAt = segment.ConfirmedAt
                };
                continue;
            }

            result.Add(segment);
        }

        return result;
    }

    private void DrawMatchSegments(
        DrawingContext drawingContext,
        Rect bounds,
        double frameWidth,
        double top,
        double height)
    {
        foreach (var segment in _matchSegments)
        {
            var x = segment.StartIndex * frameWidth;
            var width = Math.Max(frameWidth, ((segment.EndIndex - segment.StartIndex) + 1) * frameWidth);
            var rect = new Rect(x, top, width, height);
            drawingContext.DrawRoundedRectangle(
                new SolidColorBrush(Color.FromArgb(185, 28, 180, 70)),
                new Pen(new SolidColorBrush(Color.FromRgb(115, 255, 145)), 1),
                rect,
                3,
                3);

            if (width < 44)
            {
                continue;
            }

            var text = $"{segment.DisplayName} {segment.Confidence:0.00}";
            var formatted = new FormattedText(
                text,
                Thread.CurrentThread.CurrentCulture,
                FlowDirection.LeftToRight,
                new Typeface("Segoe UI Semibold"),
                12,
                Brushes.White,
                VisualTreeHelper.GetDpi(Application.Current.MainWindow).PixelsPerDip)
            {
                MaxTextWidth = Math.Max(1, width - 8),
                Trimming = TextTrimming.CharacterEllipsis
            };
            drawingContext.DrawText(formatted, new Point(rect.Left + 4, rect.Top + 3));
        }

        if (_matchSegments.Count == 0)
        {
            var formatted = new FormattedText(
                "no confirmed gestures",
                Thread.CurrentThread.CurrentCulture,
                FlowDirection.LeftToRight,
                new Typeface("Segoe UI"),
                12,
                new SolidColorBrush(Color.FromRgb(138, 148, 164)),
                VisualTreeHelper.GetDpi(Application.Current.MainWindow).PixelsPerDip);
            drawingContext.DrawText(formatted, new Point(6, top + 3));
        }
    }

    private static HashSet<long> BuildAcceptedMatchSet(IReadOnlyList<GestureDebugFrame> frames)
    {
        var result = new HashSet<long>();
        var handFrames = frames
            .Where(x => x.Kind == GestureDebugFrameKind.Hand)
            .OrderBy(x => x.Timestamp)
            .ToList();
        foreach (var confirmed in frames.Where(x => x.Snapshot.ConfirmedMatch is not null))
        {
            var acceptedWindowFrames = confirmed.Snapshot.WindowFrames?.Where(x => x.IsAcceptedMatch).ToList() ?? [];
            if (acceptedWindowFrames.Count > 0)
            {
                var oldestAccepted = acceptedWindowFrames.Max(x => x.AgeMilliseconds);
                var newestAccepted = acceptedWindowFrames.Min(x => x.AgeMilliseconds);
                var rangeStart = confirmed.Timestamp - TimeSpan.FromMilliseconds(oldestAccepted + 90);
                var rangeEnd = confirmed.Timestamp - TimeSpan.FromMilliseconds(Math.Max(0, newestAccepted - 90));
                foreach (var frame in handFrames.Where(x => x.Timestamp >= rangeStart && x.Timestamp <= rangeEnd))
                {
                    result.Add(frame.SequenceNumber);
                }
            }

            foreach (var windowFrame in acceptedWindowFrames)
            {
                var timestamp = confirmed.Timestamp - TimeSpan.FromMilliseconds(windowFrame.AgeMilliseconds);
                var nearest = handFrames
                    .OrderBy(x => Math.Abs((x.Timestamp - timestamp).TotalMilliseconds))
                    .FirstOrDefault();
                if (nearest is not null && Math.Abs((nearest.Timestamp - timestamp).TotalMilliseconds) <= 90)
                {
                    result.Add(nearest.SequenceNumber);
                }
            }
        }

        return result;
    }

    private static Brush FrameBrush(GestureDebugFrame frame, IReadOnlySet<long> acceptedMatchSequenceNumbers)
    {
        if (frame.Kind == GestureDebugFrameKind.NoHand)
        {
            return Brushes.DimGray;
        }
        if (acceptedMatchSequenceNumbers.Contains(frame.SequenceNumber))
        {
            return Brushes.LimeGreen;
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

    private sealed record TimelineMatchSegment(
        int StartIndex,
        int EndIndex,
        string DisplayName,
        float Confidence,
        DateTimeOffset ConfirmedAt);
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

using RyoikiTenkai.Core;

namespace RyoikiTenkai.Vision;

internal sealed class WindowGestureRecognizer
{
    private const int MinimumTemplateCount = 3;
    private const int ConsecutiveMatchThreshold = 2;
    private const float CustomConfidenceThreshold = 0.72f;
    private const float DynamicWarpRatioLimit = 2.6f;
    private static readonly TimeSpan WindowDuration = TimeSpan.FromSeconds(2);
    private static readonly TimeSpan Cooldown = TimeSpan.FromMilliseconds(1500);
    private static readonly TimeSpan MinimumCandidateDuration = TimeSpan.FromMilliseconds(GestureTemplateFactory.MinimumDurationMilliseconds);

    private readonly GestureWindowBuffer _window = new(WindowDuration);
    private List<GestureDefinition> _definitions = [];
    private GestureRecognitionDebugSnapshot _latestDebugSnapshot = EmptyDebugSnapshot("Waiting for hand");
    private string? _candidateGestureId;
    private int _candidateCount;
    private DateTimeOffset _lastTriggeredAt = DateTimeOffset.MinValue;

    public void SetDefinitions(IEnumerable<GestureDefinition> definitions)
    {
        _definitions = definitions.ToList();
    }

    public void Reset(string triggerState = "Waiting for hand")
    {
        _window.Reset();
        _candidateGestureId = null;
        _candidateCount = 0;
        _latestDebugSnapshot = EmptyDebugSnapshot(triggerState);
    }

    public GestureRecognitionDebugSnapshot GetDebugSnapshot()
    {
        return _latestDebugSnapshot;
    }

    public GestureRecognitionResult? Recognize(GestureFrameSample sample)
    {
        _window.Add(sample);

        var candidateTemplate = CreateCandidateTemplate(sample.Timestamp, out var candidateFailureReason);
        var customResult = RecognizeCustom(sample.Timestamp, candidateTemplate, out var scores, out var bestTemplate);
        var result = customResult;

        if (result is null)
        {
            _candidateGestureId = null;
            _candidateCount = 0;
            _latestDebugSnapshot = CreateDebugSnapshot(
                sample.Timestamp,
                candidateTemplate,
                candidateFailureReason,
                scores,
                best: null,
                bestTemplate,
                triggerState: "No match",
                confirmedMatch: null);
            return null;
        }

        if (StringComparer.OrdinalIgnoreCase.Equals(_candidateGestureId, result.GestureId))
        {
            _candidateCount++;
        }
        else
        {
            _candidateGestureId = result.GestureId;
            _candidateCount = 1;
        }

        var cooldownRemaining = Cooldown - (sample.Timestamp - _lastTriggeredAt);
        var triggerState = _candidateCount < ConsecutiveMatchThreshold
            ? $"Waiting for stable match {_candidateCount}/{ConsecutiveMatchThreshold}"
            : cooldownRemaining > TimeSpan.Zero
                ? $"Cooldown {cooldownRemaining.TotalMilliseconds:0} ms"
                : "Ready to trigger";
        _latestDebugSnapshot = CreateDebugSnapshot(
            sample.Timestamp,
            candidateTemplate,
            candidateFailureReason,
            scores,
            result,
            bestTemplate,
            triggerState,
            confirmedMatch: null);

        if (_candidateCount < ConsecutiveMatchThreshold || cooldownRemaining > TimeSpan.Zero)
        {
            return null;
        }

        _lastTriggeredAt = sample.Timestamp;
        _candidateCount = 0;
        _latestDebugSnapshot = CreateDebugSnapshot(
            sample.Timestamp,
            candidateTemplate,
            candidateFailureReason,
            scores,
            result,
            bestTemplate,
            "Confirmed match",
            new GestureConfirmedMatch(result.GestureId, result.DisplayName, result.Confidence, sample.Timestamp));
        return result;
    }

    private GestureTemplate? CreateCandidateTemplate(DateTimeOffset now, out string failureReason)
    {
        var samples = _window.Samples;
        var window = _window.CreateSnapshot();
        if (window.UsableFrameCount < GestureTemplateFactory.MinimumUsableSampleCount)
        {
            failureReason =
                $"Waiting for {GestureTemplateFactory.MinimumUsableSampleCount} usable frames at {GestureTemplateFactory.MinimumSampleRateFps:0}+ fps; have {window.UsableFrameCount} usable / {window.FrameCount} total at {window.EffectiveFps:0.0} fps.";
            return null;
        }

        if (now - samples[0].Timestamp < MinimumCandidateDuration)
        {
            failureReason = $"Waiting for full 2-second candidate window; have {window.DurationMilliseconds:0} ms.";
            return null;
        }

        var creation = GestureTemplateFactory.TryCreate(samples);
        failureReason = creation.Template is null ? creation.FailureReason : string.Empty;
        return creation.Template;
    }

    private GestureRecognitionResult? RecognizeCustom(
        DateTimeOffset now,
        GestureTemplate? candidateTemplate,
        out List<GestureTemplateScore> scores,
        out GestureTemplate? bestTemplate)
    {
        scores = [];
        bestTemplate = null;
        GestureRecognitionResult? best = null;
        var bestScore = float.MaxValue;
        var bestScoreIndex = -1;
        var bestGestureId = string.Empty;

        if (candidateTemplate is null)
        {
            foreach (var definition in _definitions)
            {
                scores.Add(CreateInactiveScore(definition, "Waiting for candidate window"));
            }

            return null;
        }

        var candidateKind = candidateTemplate.Kind;
        if (candidateKind == GestureKind.Dynamic
            && candidateTemplate.MotionSummary?.MotionScore < GestureFeatureExtractor.StaticMotionThreshold)
        {
            candidateKind = GestureKind.Static;
        }

        foreach (var definition in _definitions)
        {
            if (definition.Templates.Count < MinimumTemplateCount)
            {
                scores.Add(CreateInactiveScore(definition, $"Needs {MinimumTemplateCount} examples"));
                continue;
            }

            for (var i = 0; i < definition.Templates.Count; i++)
            {
                var template = GestureTemplateFactory.NormalizeLegacyTemplate(definition.Templates[i], definition.Type);
                if (template.Kind != candidateKind)
                {
                    scores.Add(new GestureTemplateScore(
                        GestureId: definition.Id,
                        DisplayName: definition.DisplayName,
                        TemplateIndex: i + 1,
                        Score: float.MaxValue,
                        Confidence: 0,
                        IsEligible: false,
                        Reason: $"Skipped {template.Kind} template on {candidateKind} path",
                        IsBest: false,
                        Kind: template.Kind));
                    continue;
                }

                var comparison = candidateKind == GestureKind.Static
                    ? CompareStatic(candidateTemplate, template)
                    : CompareDynamic(candidateTemplate, template);
                var score = comparison.Score;
                var confidence = ScoreToConfidence(score);
                var isEligible = confidence >= CustomConfidenceThreshold && comparison.IsEligible;

                scores.Add(new GestureTemplateScore(
                    GestureId: definition.Id,
                    DisplayName: definition.DisplayName,
                    TemplateIndex: i + 1,
                    Score: score,
                    Confidence: confidence,
                    IsEligible: isEligible,
                    Reason: isEligible ? "Eligible" : comparison.Reason,
                    IsBest: false,
                    Kind: template.Kind,
                    WarpRatio: comparison.WarpRatio));

                if (!isEligible || score >= bestScore)
                {
                    continue;
                }

                bestScore = score;
                bestScoreIndex = i + 1;
                bestGestureId = definition.Id;
                bestTemplate = template;
                best = new GestureRecognitionResult(
                    definition.Id,
                    definition.DisplayName,
                    confidence,
                    candidateKind == GestureKind.Static ? "custom-static" : "custom-dynamic",
                    TimeSpan.FromMilliseconds(candidateTemplate.DurationMilliseconds));
            }
        }

        if (best is null)
        {
            return null;
        }

        scores = scores
            .Select(x => x with
            {
                IsBest = x.GestureId == bestGestureId && x.TemplateIndex == bestScoreIndex
            })
            .OrderByDescending(x => x.IsBest)
            .ThenByDescending(x => x.Confidence)
            .ToList();
        return best;
    }

    private static GestureTemplateComparison CompareStatic(GestureTemplate candidate, GestureTemplate template)
    {
        var candidateFrames = candidate.FeatureFrames is { Count: > 0 }
            ? candidate.FeatureFrames
            : GestureTemplateFactory.NormalizeLegacyTemplate(candidate, "template").FeatureFrames ?? [];
        var templateFrames = template.FeatureFrames is { Count: > 0 }
            ? template.FeatureFrames
            : GestureTemplateFactory.NormalizeLegacyTemplate(template, "template").FeatureFrames ?? [];
        if (candidateFrames.Count == 0 || templateFrames.Count == 0)
        {
            return new GestureTemplateComparison(float.MaxValue, false, "Missing static feature frames", null, []);
        }

        var score = FeatureFrameDistance(candidateFrames[^1], templateFrames[^1], includeCenter: false);
        return new GestureTemplateComparison(
            score,
            true,
            score <= ConfidenceToScore(CustomConfidenceThreshold) ? "Eligible" : "Below static threshold",
            null,
            []);
    }

    private static GestureTemplateComparison CompareDynamic(GestureTemplate candidate, GestureTemplate template)
    {
        var candidateFrames = candidate.FeatureFrames is { Count: > 0 } frames
            ? frames
            : GestureTemplateFactory.NormalizeLegacyTemplate(candidate, "template").FeatureFrames ?? [];
        var templateFrames = template.FeatureFrames is { Count: > 0 } templateFeatureFrames
            ? templateFeatureFrames
            : GestureTemplateFactory.NormalizeLegacyTemplate(template, "template").FeatureFrames ?? [];
        if (candidateFrames.Count < 3 || templateFrames.Count < 3)
        {
            return new GestureTemplateComparison(float.MaxValue, false, "No active segment", null, []);
        }

        var dtw = BoundedDtw(candidateFrames, templateFrames, bandRadius: Math.Max(4, candidateFrames.Count / 5));
        var warpRatio = dtw.Path.Count / (float)Math.Max(candidateFrames.Count, templateFrames.Count);
        if (warpRatio > DynamicWarpRatioLimit)
        {
            return new GestureTemplateComparison(dtw.Score, false, $"Excessive DTW warp {warpRatio:0.00}", warpRatio, dtw.Path);
        }

        return new GestureTemplateComparison(
            dtw.Score,
            true,
            dtw.Score <= ConfidenceToScore(CustomConfidenceThreshold) ? "Eligible" : "Below dynamic threshold",
            warpRatio,
            dtw.Path);
    }

    internal static float Score(IReadOnlyList<GestureTemplateSample> candidate, IReadOnlyList<GestureTemplateSample> template)
    {
        var count = Math.Min(candidate.Count, template.Count);
        if (count == 0)
        {
            return float.MaxValue;
        }

        var sum = 0f;
        var terms = 0;
        for (var i = 0; i < count; i++)
        {
            var centerDx = candidate[i].CenterX - template[i].CenterX;
            var centerDy = candidate[i].CenterY - template[i].CenterY;
            sum += MathF.Sqrt((centerDx * centerDx) + (centerDy * centerDy)) * 6f;
            terms++;

            var valueCount = Math.Min(candidate[i].Values.Length, template[i].Values.Length);
            for (var j = 0; j < valueCount; j += 2)
            {
                var dx = candidate[i].Values[j] - template[i].Values[j];
                var dy = candidate[i].Values[j + 1] - template[i].Values[j + 1];
                sum += MathF.Sqrt((dx * dx) + (dy * dy));
                terms++;
            }
        }

        return terms == 0 ? float.MaxValue : sum / terms;
    }

    internal static float ScoreToConfidence(float score)
    {
        return Math.Clamp(1f - (score / 0.9f), 0, 1);
    }

    internal static float FeatureScore(IReadOnlyList<GestureFeatureFrame> candidate, IReadOnlyList<GestureFeatureFrame> template)
    {
        var count = Math.Min(candidate.Count, template.Count);
        if (count == 0)
        {
            return float.MaxValue;
        }

        var sum = 0f;
        for (var i = 0; i < count; i++)
        {
            sum += FeatureFrameDistance(candidate[i], template[i], includeCenter: true);
        }

        return sum / count;
    }

    internal static DtwResult BoundedDtw(
        IReadOnlyList<GestureFeatureFrame> candidate,
        IReadOnlyList<GestureFeatureFrame> template,
        int bandRadius)
    {
        var n = candidate.Count;
        var m = template.Count;
        if (n == 0 || m == 0)
        {
            return new DtwResult(float.MaxValue, []);
        }

        var radius = Math.Max(bandRadius, Math.Abs(n - m));
        var costs = new float[n + 1, m + 1];
        for (var i = 0; i <= n; i++)
        {
            for (var j = 0; j <= m; j++)
            {
                costs[i, j] = float.PositiveInfinity;
            }
        }

        costs[0, 0] = 0;
        for (var i = 1; i <= n; i++)
        {
            var start = Math.Max(1, i - radius);
            var end = Math.Min(m, i + radius);
            for (var j = start; j <= end; j++)
            {
                var distance = FeatureFrameDistance(candidate[i - 1], template[j - 1], includeCenter: true);
                var previous = Math.Min(costs[i - 1, j], Math.Min(costs[i, j - 1], costs[i - 1, j - 1]));
                costs[i, j] = distance + previous;
            }
        }

        if (!float.IsFinite(costs[n, m]))
        {
            return new DtwResult(float.MaxValue, []);
        }

        var path = new List<GestureDtwPoint>();
        var x = n;
        var y = m;
        while (x > 0 && y > 0)
        {
            path.Add(new GestureDtwPoint(x - 1, y - 1));
            var diagonal = costs[x - 1, y - 1];
            var up = costs[x - 1, y];
            var left = costs[x, y - 1];
            if (diagonal <= up && diagonal <= left)
            {
                x--;
                y--;
            }
            else if (up <= left)
            {
                x--;
            }
            else
            {
                y--;
            }
        }

        path.Reverse();
        return new DtwResult(costs[n, m] / Math.Max(1, path.Count), path);
    }

    private static float FeatureFrameDistance(GestureFeatureFrame candidate, GestureFeatureFrame template, bool includeCenter)
    {
        var sum = 0f;
        var terms = 0;
        if (includeCenter)
        {
            var centerDx = candidate.CenterX - template.CenterX;
            var centerDy = candidate.CenterY - template.CenterY;
            sum += MathF.Sqrt((centerDx * centerDx) + (centerDy * centerDy)) * 10f;
            terms++;
        }

        var valueCount = Math.Min(candidate.Values.Length, template.Values.Length);
        for (var i = 0; i + 1 < valueCount; i += 2)
        {
            var dx = candidate.Values[i] - template.Values[i];
            var dy = candidate.Values[i + 1] - template.Values[i + 1];
            sum += MathF.Sqrt((dx * dx) + (dy * dy));
            terms++;
        }

        return terms == 0 ? float.MaxValue : sum / terms;
    }

    private static float ConfidenceToScore(float confidence)
    {
        return (1f - Math.Clamp(confidence, 0, 1)) * 0.9f;
    }

    private GestureRecognitionDebugSnapshot CreateDebugSnapshot(
        DateTimeOffset timestamp,
        GestureTemplate? candidateTemplate,
        string candidateFailureReason,
        IReadOnlyList<GestureTemplateScore> scores,
        GestureRecognitionResult? best,
        GestureTemplate? bestTemplate,
        string triggerState,
        GestureConfirmedMatch? confirmedMatch)
    {
        var window = _window.CreateSnapshot();
        var windowFrames = CreateDebugWindowFrames(timestamp, confirmedMatch is not null);
        return new GestureRecognitionDebugSnapshot(
            timestamp,
            window.FrameCount,
            window.UsableFrameCount,
            Math.Max(0, window.DurationMilliseconds),
            window.EffectiveFps,
            window.UsableEffectiveFps,
            candidateTemplate,
            candidateFailureReason,
            scores,
            best?.GestureId,
            best?.DisplayName,
            best?.Confidence,
            bestTemplate,
            CustomConfidenceThreshold,
            triggerState,
            confirmedMatch,
            candidateTemplate?.Kind == GestureKind.Static ? GestureRecognitionPath.Static :
                candidateTemplate?.Kind == GestureKind.Dynamic ? GestureRecognitionPath.Dynamic : GestureRecognitionPath.Unknown,
            candidateTemplate?.MotionSummary?.MotionScore ?? 0,
            candidateTemplate?.ActiveSegment?.StartMilliseconds,
            candidateTemplate?.ActiveSegment?.EndMilliseconds,
            scores.Where(x => x.Kind == GestureKind.Static && float.IsFinite(x.Score)).OrderBy(x => x.Score).FirstOrDefault()?.Score,
            scores.Where(x => x.Kind == GestureKind.Dynamic && float.IsFinite(x.Score)).OrderBy(x => x.Score).FirstOrDefault()?.Score,
            scores.Where(x => x.IsBest).Select(x => x.WarpRatio).FirstOrDefault(),
            string.IsNullOrWhiteSpace(candidateFailureReason)
                ? scores.FirstOrDefault(x => x.TemplateIndex > 0 && !x.IsEligible)?.Reason ?? string.Empty
                : candidateFailureReason,
            scores.Any(x => x.IsBest) ? FindBestDtwPath(candidateTemplate, bestTemplate) : [],
            windowFrames);
    }

    private List<GestureDebugWindowFrame> CreateDebugWindowFrames(DateTimeOffset now, bool acceptedMatch)
    {
        if (_window.Samples.Count == 0)
        {
            return [];
        }

        var firstTimestamp = _window.Samples[0].Timestamp;
        var frames = new List<GestureDebugWindowFrame>(_window.Samples.Count);
        for (var i = 0; i < _window.Samples.Count; i++)
        {
            var sample = _window.Samples[i];
            var isUsable = sample.Landmarks.Count >= 21 && sample.Confidence >= 0.35f;
            frames.Add(new GestureDebugWindowFrame(
                Index: i,
                TimeOffsetMilliseconds: Math.Max(0, (sample.Timestamp - firstTimestamp).TotalMilliseconds),
                AgeMilliseconds: Math.Max(0, (now - sample.Timestamp).TotalMilliseconds),
                Confidence: sample.Confidence,
                IsUsable: isUsable,
                IsAcceptedMatch: acceptedMatch && isUsable));
        }

        return frames;
    }

    private static IReadOnlyList<GestureDtwPoint> FindBestDtwPath(GestureTemplate? candidateTemplate, GestureTemplate? bestTemplate)
    {
        if (candidateTemplate?.Kind != GestureKind.Dynamic
            || bestTemplate?.Kind != GestureKind.Dynamic
            || candidateTemplate.FeatureFrames is not { Count: > 0 } candidate
            || bestTemplate.FeatureFrames is not { Count: > 0 } template)
        {
            return [];
        }

        return BoundedDtw(candidate, template, bandRadius: Math.Max(4, candidate.Count / 5)).Path;
    }

    private static GestureTemplateScore CreateInactiveScore(GestureDefinition definition, string reason)
    {
        return new GestureTemplateScore(
            GestureId: definition.Id,
            DisplayName: definition.DisplayName,
            TemplateIndex: -1,
            Score: float.MaxValue,
            Confidence: 0,
            IsEligible: false,
            Reason: reason,
            IsBest: false);
    }

    private static GestureRecognitionDebugSnapshot EmptyDebugSnapshot(string triggerState)
    {
        return new GestureRecognitionDebugSnapshot(
            DateTimeOffset.MinValue,
            BufferFrameCount: 0,
            UsableFrameCount: 0,
            BufferDurationMilliseconds: 0,
            BufferEffectiveFps: 0,
            UsableEffectiveFps: 0,
            CandidateTemplate: null,
            CandidateFailureReason: string.Empty,
            Scores: [],
            BestGestureId: null,
            BestDisplayName: null,
            BestConfidence: null,
            BestTemplate: null,
            MatchThreshold: CustomConfidenceThreshold,
            triggerState,
            ConfirmedMatch: null,
            WindowFrames: []);
    }

    internal sealed record DtwResult(float Score, IReadOnlyList<GestureDtwPoint> Path);

    private sealed record GestureTemplateComparison(
        float Score,
        bool IsEligible,
        string Reason,
        float? WarpRatio,
        IReadOnlyList<GestureDtwPoint> Path);
}

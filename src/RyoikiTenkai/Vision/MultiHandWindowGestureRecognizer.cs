using RyoikiTenkai.Core;

namespace RyoikiTenkai.Vision;

internal sealed class MultiHandWindowGestureRecognizer
{
    private const int MinimumTemplateCount = 3;
    private const int ConsecutiveMatchThreshold = 2;
    private const int MinimumCandidateUsableFrames = 25;
    private const double MinimumCandidateDurationMilliseconds = 1400;
    private const float ConfidenceThreshold = 0.80f;
    private const float ReverseDtwMargin = 0.94f;
    private static readonly TimeSpan WindowDuration = TimeSpan.FromMilliseconds(2600);
    private static readonly TimeSpan Cooldown = TimeSpan.FromMilliseconds(1500);
    private static readonly int[] CandidateDurationsMilliseconds = [1400, 1700, 2100, 2500];

    private readonly GestureFrameSetWindowBuffer _window = new(WindowDuration);
    private List<GestureDefinition> _definitions = [];
    private GestureRecognitionDebugSnapshot _latestDebugSnapshot = EmptyDebugSnapshot("Waiting for two hands");
    private string? _candidateGestureId;
    private int _candidateCount;
    private DateTimeOffset _lastTriggeredAt = DateTimeOffset.MinValue;
    private DateTimeOffset _lastTriggeredSegmentStart = DateTimeOffset.MinValue;
    private DateTimeOffset _lastTriggeredSegmentEnd = DateTimeOffset.MinValue;

    public void SetDefinitions(IEnumerable<GestureDefinition> definitions)
    {
        _definitions = definitions.ToList();
    }

    public void Reset(string triggerState = "Waiting for two hands")
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

    public GestureRecognitionResult? Recognize(GestureFrameSetSample sample)
    {
        _window.Add(sample);
        var match = FindBestMatch(sample.Timestamp, out var scores, out var candidateFailureReason);
        if (match is null)
        {
            _candidateGestureId = null;
            _candidateCount = 0;
            _latestDebugSnapshot = CreateDebugSnapshot(sample.Timestamp, null, candidateFailureReason, scores, "No two-hand match");
            return null;
        }

        if (StringComparer.OrdinalIgnoreCase.Equals(_candidateGestureId, match.Result.GestureId))
        {
            _candidateCount++;
        }
        else
        {
            _candidateGestureId = match.Result.GestureId;
            _candidateCount = 1;
        }

        var cooldownRemaining = Cooldown - (sample.Timestamp - _lastTriggeredAt);
        var duplicateSegment = IsDuplicateSegment(match);
        var triggerState = _candidateCount < ConsecutiveMatchThreshold
            ? $"Waiting for stable two-hand match {_candidateCount}/{ConsecutiveMatchThreshold}"
            : duplicateSegment
                ? "Duplicate two-hand segment suppressed"
                : cooldownRemaining > TimeSpan.Zero
                    ? $"Cooldown {cooldownRemaining.TotalMilliseconds:0} ms"
                    : "Ready to trigger";
        _latestDebugSnapshot = CreateDebugSnapshot(sample.Timestamp, match, candidateFailureReason, scores, triggerState);
        if (_candidateCount < ConsecutiveMatchThreshold || cooldownRemaining > TimeSpan.Zero || duplicateSegment)
        {
            return null;
        }

        _lastTriggeredAt = sample.Timestamp;
        _lastTriggeredSegmentStart = match.CandidateStart;
        _lastTriggeredSegmentEnd = match.CandidateEnd;
        _candidateCount = 0;
        _latestDebugSnapshot = CreateDebugSnapshot(sample.Timestamp, match, candidateFailureReason, scores, "Confirmed two-hand match", new GestureConfirmedMatch(match.Result.GestureId, match.Result.DisplayName, match.Result.Confidence, sample.Timestamp));
        return match.Result;
    }

    private bool IsDuplicateSegment(GestureMatch match)
    {
        if (_lastTriggeredSegmentEnd == DateTimeOffset.MinValue)
        {
            return false;
        }

        return StringComparer.OrdinalIgnoreCase.Equals(_candidateGestureId, match.Result.GestureId)
            && match.CandidateStart <= _lastTriggeredSegmentEnd
            && match.CandidateEnd >= _lastTriggeredSegmentStart;
    }

    private GestureMatch? FindBestMatch(DateTimeOffset now, out List<GestureTemplateScore> scores, out string candidateFailureReason)
    {
        scores = [];
        candidateFailureReason = string.Empty;
        var definitions = _definitions
            .Where(definition => definition.Templates.Count(MultiHandGestureFeatureExtractor.IsDistinctTwoHandTemplate) >= MinimumTemplateCount)
            .ToList();
        if (definitions.Count == 0)
        {
            candidateFailureReason = "No two-hand gesture has 3 accepted examples.";
            return null;
        }

        var candidates = CreateCandidateWindows(now).ToList();
        if (candidates.Count == 0)
        {
            var window = _window.CreateSnapshot();
            candidateFailureReason = $"Waiting for usable two-hand rolling windows; have {window.UsableFrameCount} usable / {window.FrameCount} total at {window.UsableEffectiveFps:0.0} usable fps.";
            return null;
        }

        GestureMatch? best = null;
        var bestScore = float.MaxValue;
        var bestScoreIndex = -1;
        foreach (var definition in definitions)
        {
            var twoHandTemplates = definition.Templates
                .Select((template, index) => (template, index))
                .Where(x => MultiHandGestureFeatureExtractor.IsDistinctTwoHandTemplate(x.template)
                    && x.template.FeatureFrames is { Count: >= 3 })
                .ToList();
            foreach (var (template, templateIndex) in twoHandTemplates)
            {
                GestureTemplateScore? scoreRecord = null;
                GestureTemplateComparison? bestComparison = null;
                GestureCandidateWindow? bestCandidate = null;
                foreach (var candidate in candidates)
                {
                    var reason = RejectionReason(candidate.Template.MultiHandSummary, template.MultiHandSummary);
                    if (!string.IsNullOrWhiteSpace(reason))
                    {
                        scoreRecord = new GestureTemplateScore(definition.Id, definition.DisplayName, templateIndex + 1, float.MaxValue, 0, false, reason, false);
                        continue;
                    }

                    var comparison = Compare(candidate.Template.FeatureFrames ?? [], template.FeatureFrames ?? []);
                    if (!comparison.IsEligible)
                    {
                        scoreRecord = new GestureTemplateScore(definition.Id, definition.DisplayName, templateIndex + 1, comparison.Score, ScoreToConfidence(comparison.Score), false, comparison.Reason, false);
                        continue;
                    }

                    if (comparison.Score < (bestComparison?.Score ?? float.MaxValue))
                    {
                        bestComparison = comparison;
                        bestCandidate = candidate;
                    }
                }

                if (bestComparison is null || bestCandidate is null)
                {
                    scores.Add(scoreRecord ?? new GestureTemplateScore(definition.Id, definition.DisplayName, templateIndex + 1, float.MaxValue, 0, false, "No two-hand candidate window", false));
                    continue;
                }

                var confidence = ScoreToConfidence(bestComparison.Score);
                scores.Add(new GestureTemplateScore(definition.Id, definition.DisplayName, templateIndex + 1, bestComparison.Score, confidence, true, "Eligible two-hand", false, GestureKind.Dynamic, bestComparison.WarpRatio));
                if (bestComparison.Score < bestScore)
                {
                    bestScore = bestComparison.Score;
                    bestScoreIndex = templateIndex + 1;
                    best = new GestureMatch(
                        new GestureRecognitionResult(definition.Id, definition.DisplayName, confidence, "custom-two-hand", TimeSpan.FromMilliseconds(bestCandidate.Template.DurationMilliseconds)),
                        bestCandidate.Template,
                        template,
                        bestCandidate.Start,
                        bestCandidate.End,
                        bestComparison);
                }
            }
        }

        if (best is null)
        {
            return null;
        }

        scores = scores
            .Select(x => x with { IsBest = x.GestureId == best.Result.GestureId && x.TemplateIndex == bestScoreIndex })
            .OrderByDescending(x => x.IsBest)
            .ThenByDescending(x => x.Confidence)
            .ToList();
        return best;
    }

    private IEnumerable<GestureCandidateWindow> CreateCandidateWindows(DateTimeOffset now)
    {
        foreach (var durationMilliseconds in CandidateDurationsMilliseconds)
        {
            var start = now - TimeSpan.FromMilliseconds(durationMilliseconds);
            var windowSamples = _window.Samples
                .Where(x => x.Timestamp >= start && x.Timestamp <= now)
                .ToList();
            var duration = windowSamples.Count < 2
                ? 0
                : Math.Max(0, (windowSamples[^1].Timestamp - windowSamples[0].Timestamp).TotalMilliseconds);
            if (duration < MinimumCandidateDurationMilliseconds)
            {
                continue;
            }

            if (MultiHandGestureFeatureExtractor.CountUsableTwoHandFrames(windowSamples) < MinimumCandidateUsableFrames)
            {
                continue;
            }

            var sequence = MultiHandGestureFeatureExtractor.Extract(windowSamples);
            var featureFrames = GestureFeatureExtractor.BuildUnifiedSequence(sequence);
            if (featureFrames.Count < 3)
            {
                continue;
            }

            var template = new GestureTemplate(
                Samples: [],
                SourceFrameCount: windowSamples.Count,
                DurationMilliseconds: duration,
                AverageConfidence: (float)windowSamples.Average(x => x.Hands.Take(2).Average(hand => hand.Confidence)),
                Kind: GestureKind.Dynamic,
                FeatureFrames: featureFrames,
                MotionSummary: sequence.Motion,
                SourceSkeletonFrameCount: windowSamples.Count * 2,
                HandCount: 2,
                MultiHandSummary: MultiHandGestureFeatureExtractor.Summarize(sequence.Frames));
            yield return new GestureCandidateWindow(windowSamples[0].Timestamp, windowSamples[^1].Timestamp, template);
        }
    }

    private static string RejectionReason(GestureMultiHandFeatureSummary? candidate, GestureMultiHandFeatureSummary? template)
    {
        if (candidate is null || template is null)
        {
            return string.Empty;
        }

        if (template.RelativeTranslationDistance >= GestureTemplateFactory.MinimumTranslationDistance
            && candidate.RelativeTranslationDistance < template.RelativeTranslationDistance * 0.45f)
        {
            return $"Two-hand relative translation too small {candidate.RelativeTranslationDistance:0.000} < template {template.RelativeTranslationDistance:0.000}";
        }

        if (template.RelativeDistanceRange >= GestureTemplateFactory.MinimumHandScaleRatioRange
            && candidate.RelativeDistanceRange < template.RelativeDistanceRange * 0.55f)
        {
            return $"Two-hand distance change too small {candidate.RelativeDistanceRange:0.000} < template {template.RelativeDistanceRange:0.000}";
        }

        if (MathF.Abs(template.RelativeDistanceDelta) >= GestureTemplateFactory.MinimumHandScaleRatioRange
            && MathF.Sign(candidate.RelativeDistanceDelta) != MathF.Sign(template.RelativeDistanceDelta))
        {
            return $"Two-hand distance moved opposite direction candidate {candidate.RelativeDistanceDelta:0.000} template {template.RelativeDistanceDelta:0.000}";
        }

        return string.Empty;
    }

    private static GestureTemplateComparison Compare(IReadOnlyList<GestureFeatureFrame> candidate, IReadOnlyList<GestureFeatureFrame> template)
    {
        var bandRadius = Math.Max(5, candidate.Count / 4);
        var dtw = BoundedDtw(candidate, template, bandRadius);
        var reversedDtw = BoundedDtw(candidate, ReverseFrames(template), bandRadius);
        if (reversedDtw.Score <= dtw.Score * ReverseDtwMargin)
        {
            return new GestureTemplateComparison(
                dtw.Score,
                false,
                $"Reversed two-hand time direction fits better forward={dtw.Score:0.000} reverse={reversedDtw.Score:0.000}",
                null);
        }

        var warpRatio = dtw.PathLength / (float)Math.Max(candidate.Count, template.Count);
        var threshold = ConfidenceToScore(ConfidenceThreshold);
        return new GestureTemplateComparison(
            dtw.Score,
            dtw.Score <= threshold,
            dtw.Score <= threshold ? "Eligible" : "Below two-hand threshold",
            warpRatio);
    }

    private static List<GestureFeatureFrame> ReverseFrames(IReadOnlyList<GestureFeatureFrame> frames)
    {
        var result = new List<GestureFeatureFrame>(frames.Count);
        for (var i = frames.Count - 1; i >= 0; i--)
        {
            result.Add(frames[i]);
        }

        return result;
    }

    private static DtwResult BoundedDtw(IReadOnlyList<GestureFeatureFrame> candidate, IReadOnlyList<GestureFeatureFrame> template, int bandRadius)
    {
        var n = candidate.Count;
        var m = template.Count;
        if (n == 0 || m == 0)
        {
            return new DtwResult(float.MaxValue, 0);
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
                var distance = FrameDistance(candidate[i - 1], template[j - 1]);
                var previous = Math.Min(costs[i - 1, j], Math.Min(costs[i, j - 1], costs[i - 1, j - 1]));
                costs[i, j] = distance + previous;
            }
        }

        var pathLength = 0;
        var x = n;
        var y = m;
        while (x > 0 && y > 0)
        {
            pathLength++;
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

        return new DtwResult(costs[n, m] / Math.Max(1, pathLength), pathLength);
    }

    private static float FrameDistance(GestureFeatureFrame candidate, GestureFeatureFrame template)
    {
        var length = Math.Min(candidate.Values.Length, template.Values.Length);
        if (length == 0)
        {
            return float.MaxValue;
        }

        var sum = 0f;
        for (var i = 0; i < length; i++)
        {
            sum += MathF.Abs(candidate.Values[i] - template.Values[i]);
        }

        return sum / length;
    }

    private static float ScoreToConfidence(float score)
    {
        return Math.Clamp(1f - (score / 0.9f), 0, 1);
    }

    private static float ConfidenceToScore(float confidence)
    {
        return (1f - Math.Clamp(confidence, 0, 1)) * 0.9f;
    }

    private GestureRecognitionDebugSnapshot CreateDebugSnapshot(
        DateTimeOffset timestamp,
        GestureMatch? match,
        string candidateFailureReason,
        IReadOnlyList<GestureTemplateScore> scores,
        string triggerState,
        GestureConfirmedMatch? confirmedMatch = null)
    {
        var window = _window.CreateSnapshot();
        var windowFrames = CreateDebugWindowFrames(match, timestamp, confirmedMatch is not null);
        return new GestureRecognitionDebugSnapshot(
            timestamp,
            window.FrameCount,
            window.UsableFrameCount,
            window.DurationMilliseconds,
            window.EffectiveFps,
            window.UsableEffectiveFps,
            match?.CandidateTemplate,
            candidateFailureReason,
            scores,
            match?.Result.GestureId,
            match?.Result.DisplayName,
            match?.Result.Confidence,
            match?.BestTemplate,
            ConfidenceThreshold,
            triggerState,
            confirmedMatch,
            GestureRecognitionPath.UnifiedSequence,
            match?.CandidateTemplate.MotionSummary?.MotionScore ?? 0,
            match is null ? null : Math.Max(0, (match.CandidateStart - _window.Samples[0].Timestamp).TotalMilliseconds),
            match is null ? null : Math.Max(0, (match.CandidateEnd - _window.Samples[0].Timestamp).TotalMilliseconds),
            DtwScore: match?.Comparison.Score,
            DtwWarpRatio: match?.Comparison.WarpRatio,
            RejectionReason: string.IsNullOrWhiteSpace(candidateFailureReason)
                ? scores.FirstOrDefault(x => x.TemplateIndex > 0 && !x.IsEligible)?.Reason ?? string.Empty
                : candidateFailureReason,
            WindowFrames: windowFrames);
    }

    private List<GestureDebugWindowFrame> CreateDebugWindowFrames(GestureMatch? match, DateTimeOffset now, bool acceptedMatch)
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
            var isUsable = sample.Hands.Count >= 2
                && sample.Hands.Take(2).All(hand => hand.Landmarks.Count >= 21 && hand.Confidence >= 0.35f);
            var confidence = sample.Hands.Count == 0 ? 0 : sample.Hands.Take(2).Average(hand => hand.Confidence);
            var inMatchedWindow = match is not null
                && sample.Timestamp >= match.CandidateStart
                && sample.Timestamp <= match.CandidateEnd;
            frames.Add(new GestureDebugWindowFrame(
                Index: i,
                TimeOffsetMilliseconds: Math.Max(0, (sample.Timestamp - firstTimestamp).TotalMilliseconds),
                AgeMilliseconds: Math.Max(0, (now - sample.Timestamp).TotalMilliseconds),
                Confidence: confidence,
                IsUsable: isUsable,
                IsAcceptedMatch: acceptedMatch && isUsable && inMatchedWindow));
        }

        return frames;
    }

    private static GestureRecognitionDebugSnapshot EmptyDebugSnapshot(string triggerState)
    {
        return new GestureRecognitionDebugSnapshot(
            DateTimeOffset.MinValue,
            0,
            0,
            0,
            0,
            0,
            null,
            string.Empty,
            [],
            null,
            null,
            null,
            null,
            ConfidenceThreshold,
            triggerState,
            null,
            GestureRecognitionPath.UnifiedSequence,
            WindowFrames: []);
    }

    private sealed record GestureTemplateComparison(float Score, bool IsEligible, string Reason, float? WarpRatio);

    private sealed record DtwResult(float Score, int PathLength);

    private sealed record GestureCandidateWindow(DateTimeOffset Start, DateTimeOffset End, GestureTemplate Template);

    private sealed record GestureMatch(
        GestureRecognitionResult Result,
        GestureTemplate CandidateTemplate,
        GestureTemplate BestTemplate,
        DateTimeOffset CandidateStart,
        DateTimeOffset CandidateEnd,
        GestureTemplateComparison Comparison);
}

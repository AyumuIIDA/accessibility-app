using RyoikiTenkai.Core;

namespace RyoikiTenkai.Vision;

internal sealed class WindowGestureRecognizer
{
    private const int MinimumTemplateCount = 3;
    private const int ConsecutiveMatchThreshold = 2;
    private const int MinimumCandidateUsableFrames = 25;
    private const double MinimumCandidateDurationMilliseconds = 1400;
    private const float ConfidenceThreshold = 0.82f;
    private const float WarpRatioLimit = 2.8f;
    private static readonly TimeSpan WindowDuration = TimeSpan.FromMilliseconds(2600);
    private static readonly TimeSpan Cooldown = TimeSpan.FromMilliseconds(1500);
    private static readonly int[] CandidateDurationsMilliseconds = [1400, 1700, 2100, 2500];

    private readonly GestureWindowBuffer _window = new(WindowDuration);
    private List<GestureDefinition> _definitions = [];
    private GestureRecognitionDebugSnapshot _latestDebugSnapshot = EmptyDebugSnapshot("Waiting for hand");
    private string? _candidateGestureId;
    private int _candidateCount;
    private DateTimeOffset _lastTriggeredAt = DateTimeOffset.MinValue;
    private DateTimeOffset _lastTriggeredSegmentStart = DateTimeOffset.MinValue;
    private DateTimeOffset _lastTriggeredSegmentEnd = DateTimeOffset.MinValue;

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

        var match = FindBestMatch(sample.Timestamp, out var scores, out var candidateFailureReason);
        if (match is null)
        {
            _candidateGestureId = null;
            _candidateCount = 0;
            _latestDebugSnapshot = CreateDebugSnapshot(
                sample.Timestamp,
                match,
                candidateFailureReason,
                scores,
                "No match",
                confirmedMatch: null);
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
            ? $"Waiting for stable match {_candidateCount}/{ConsecutiveMatchThreshold}"
            : duplicateSegment
                ? "Duplicate segment suppressed"
                : cooldownRemaining > TimeSpan.Zero
                    ? $"Cooldown {cooldownRemaining.TotalMilliseconds:0} ms"
                    : "Ready to trigger";
        _latestDebugSnapshot = CreateDebugSnapshot(
            sample.Timestamp,
            match,
            candidateFailureReason,
            scores,
            triggerState,
            confirmedMatch: null);

        if (_candidateCount < ConsecutiveMatchThreshold || cooldownRemaining > TimeSpan.Zero || duplicateSegment)
        {
            return null;
        }

        _lastTriggeredAt = sample.Timestamp;
        _lastTriggeredSegmentStart = match.CandidateStart;
        _lastTriggeredSegmentEnd = match.CandidateEnd;
        _candidateCount = 0;
        _latestDebugSnapshot = CreateDebugSnapshot(
            sample.Timestamp,
            match,
            candidateFailureReason,
            scores,
            "Confirmed match",
            new GestureConfirmedMatch(match.Result.GestureId, match.Result.DisplayName, match.Result.Confidence, sample.Timestamp));
        return match.Result;
    }

    private GestureMatch? FindBestMatch(
        DateTimeOffset now,
        out List<GestureTemplateScore> scores,
        out string candidateFailureReason)
    {
        scores = [];
        candidateFailureReason = string.Empty;
        var definitions = _definitions.Where(x => x.Templates.Count >= MinimumTemplateCount).ToList();
        foreach (var inactive in _definitions.Where(x => x.Templates.Count < MinimumTemplateCount))
        {
            scores.Add(CreateInactiveScore(inactive, $"Needs {MinimumTemplateCount} examples"));
        }

        if (definitions.Count == 0)
        {
            candidateFailureReason = _definitions.Count == 0
                ? "No gesture recordings are active."
                : $"No gesture has {MinimumTemplateCount} accepted examples.";
            return null;
        }

        var candidates = CreateCandidateWindows(now).ToList();
        if (candidates.Count == 0)
        {
            var window = _window.CreateSnapshot();
            candidateFailureReason =
                $"Waiting for usable rolling windows; have {window.UsableFrameCount} usable / {window.FrameCount} total at {window.UsableEffectiveFps:0.0} usable fps.";
            return null;
        }

        GestureMatch? best = null;
        var bestScore = float.MaxValue;
        var bestScoreIndex = -1;
        foreach (var definition in definitions)
        {
            for (var templateIndex = 0; templateIndex < definition.Templates.Count; templateIndex++)
            {
                var template = GestureTemplateFactory.NormalizeLegacyTemplate(definition.Templates[templateIndex], definition.Type);
                var templateFrames = template.FeatureFrames is { Count: >= 3 } frames
                    ? frames
                    : GestureFeatureExtractor.BuildUnifiedSequence(new GestureFeatureSequence(
                        GestureTemplateFactory.NormalizeLegacyTemplate(template, definition.Type).FeatureFrames ?? [],
                        template.MotionSummary ?? new GestureMotionSummary(0, 0, 0, template.DurationMilliseconds, template.SourceFrameCount),
                        null));
                if (templateFrames.Count < 3)
                {
                    scores.Add(new GestureTemplateScore(
                        definition.Id,
                        definition.DisplayName,
                        templateIndex + 1,
                        float.MaxValue,
                        0,
                        false,
                        "Template has too few feature frames",
                        false,
                        GestureKind.Dynamic));
                    continue;
                }

                GestureMatch? templateBest = null;
                GestureTemplateComparison? templateBestComparison = null;
                foreach (var candidate in candidates)
                {
                    var topologyReason = TopologyRejectionReason(candidate.Template.Topology, template.Topology);
                    if (!string.IsNullOrWhiteSpace(topologyReason))
                    {
                        continue;
                    }

                    var comparison = CompareUnified(candidate.Template.FeatureFrames ?? [], templateFrames);
                    var confidence = ScoreToConfidence(comparison.Score);
                    if (!comparison.IsEligible || confidence < ConfidenceThreshold)
                    {
                        continue;
                    }

                    if (comparison.Score >= (templateBestComparison?.Score ?? float.MaxValue))
                    {
                        continue;
                    }

                    templateBestComparison = comparison;
                    templateBest = new GestureMatch(
                        new GestureRecognitionResult(
                            definition.Id,
                            definition.DisplayName,
                            confidence,
                            "custom-unified",
                            TimeSpan.FromMilliseconds(candidate.Template.DurationMilliseconds)),
                        candidate.Template,
                        template,
                        candidate.Start,
                        candidate.End,
                        comparison);
                }

                if (templateBestComparison is null)
                {
                    var nearest = candidates
                        .Select(candidate =>
                        {
                            var topologyReason = TopologyRejectionReason(candidate.Template.Topology, template.Topology);
                            if (!string.IsNullOrWhiteSpace(topologyReason))
                            {
                                return new GestureTemplateComparison(float.MaxValue, false, topologyReason, null, [], null);
                            }

                            return CompareUnified(candidate.Template.FeatureFrames ?? [], templateFrames);
                        })
                        .OrderBy(x => x.Score)
                        .FirstOrDefault();
                    scores.Add(new GestureTemplateScore(
                        definition.Id,
                        definition.DisplayName,
                        templateIndex + 1,
                        nearest?.Score ?? float.MaxValue,
                        nearest is null ? 0 : ScoreToConfidence(nearest.Score),
                        false,
                        nearest?.Reason ?? "No candidate window",
                        false,
                        GestureKind.Dynamic,
                        nearest?.WarpRatio,
                        nearest?.Breakdown));
                    continue;
                }

                scores.Add(new GestureTemplateScore(
                    definition.Id,
                    definition.DisplayName,
                    templateIndex + 1,
                    templateBestComparison.Score,
                    templateBest!.Result.Confidence,
                    true,
                    "Eligible",
                    false,
                    GestureKind.Dynamic,
                    templateBestComparison.WarpRatio,
                    templateBestComparison.Breakdown));

                if (templateBestComparison.Score >= bestScore)
                {
                    continue;
                }

                bestScore = templateBestComparison.Score;
                bestScoreIndex = templateIndex + 1;
                best = templateBest;
            }
        }

        if (best is null)
        {
            return null;
        }

        scores = scores
            .Select(x => x with
            {
                IsBest = x.GestureId == best.Result.GestureId && x.TemplateIndex == bestScoreIndex
            })
            .OrderByDescending(x => x.IsBest)
            .ThenByDescending(x => x.Confidence)
            .ToList();
        return best;
    }

    private IEnumerable<GestureCandidateWindow> CreateCandidateWindows(DateTimeOffset now)
    {
        var samples = _window.Samples;
        if (samples.Count == 0)
        {
            yield break;
        }

        foreach (var durationMilliseconds in CandidateDurationsMilliseconds)
        {
            var start = now - TimeSpan.FromMilliseconds(durationMilliseconds);
            var windowSamples = samples
                .Where(x => x.Timestamp >= start && x.Timestamp <= now)
                .ToList();
            var duration = windowSamples.Count < 2
                ? 0
                : Math.Max(0, (windowSamples[^1].Timestamp - windowSamples[0].Timestamp).TotalMilliseconds);
            if (duration < MinimumCandidateDurationMilliseconds)
            {
                continue;
            }

            if (windowSamples.Count(x => x.Landmarks.Count >= 21 && x.Confidence >= 0.35f) < MinimumCandidateUsableFrames)
            {
                continue;
            }

            var sequence = GestureFeatureExtractor.Extract(windowSamples);
            var featureFrames = GestureFeatureExtractor.BuildUnifiedSequence(sequence);
            if (featureFrames.Count < 3)
            {
                continue;
            }

            var template = new GestureTemplate(
                Samples: GestureTemplateFactory.Resample(GestureTemplateFactory.Normalize(windowSamples), GestureFeatureExtractor.UnifiedSequenceLength),
                SourceFrameCount: windowSamples.Count,
                DurationMilliseconds: duration,
                AverageConfidence: (float)windowSamples.Average(x => x.Confidence),
                SkeletonFrames: GestureTemplateFactory.NormalizeSkeleton(windowSamples),
                Kind: GestureKind.Dynamic,
                FeatureFrames: featureFrames,
                MotionSummary: sequence.Motion,
                SourceSkeletonFrameCount: windowSamples.Count,
                ActiveSegment: sequence.ActiveSegment,
                Topology: GestureFeatureExtractor.SummarizeTopology(sequence.Frames),
                FeatureTrack: GestureFeatureExtractor.BuildFeatureTrack(windowSamples));
            yield return new GestureCandidateWindow(windowSamples[0].Timestamp, windowSamples[^1].Timestamp, template);
        }
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
            for (var j = 0; j + 1 < valueCount; j += 2)
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

    private static float ConfidenceToScore(float confidence)
    {
        return (1f - Math.Clamp(confidence, 0, 1)) * 0.9f;
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
            sum += FeatureFrameDistance(candidate[i], template[i]).TotalScore;
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
            return new DtwResult(float.MaxValue, [], new GestureScoreBreakdown(0, 0, 0, 0, 0, 0, 0, 0, float.MaxValue));
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
                var distance = FeatureFrameDistance(candidate[i - 1], template[j - 1]).TotalScore;
                var previous = Math.Min(costs[i - 1, j], Math.Min(costs[i, j - 1], costs[i - 1, j - 1]));
                costs[i, j] = distance + previous;
            }
        }

        if (!float.IsFinite(costs[n, m]))
        {
            return new DtwResult(float.MaxValue, [], new GestureScoreBreakdown(0, 0, 0, 0, 0, 0, 0, 0, float.MaxValue));
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
        var breakdown = AverageBreakdown(candidate, template, path);
        return new DtwResult(costs[n, m] / Math.Max(1, path.Count), path, breakdown);
    }

    private static GestureTemplateComparison CompareUnified(
        IReadOnlyList<GestureFeatureFrame> candidateFrames,
        IReadOnlyList<GestureFeatureFrame> templateFrames)
    {
        if (candidateFrames.Count < 3 || templateFrames.Count < 3)
        {
            return new GestureTemplateComparison(float.MaxValue, false, "Too few feature frames", null, [], null);
        }

        if (HasOppositeMotionDirection(candidateFrames, templateFrames))
        {
            return new GestureTemplateComparison(float.MaxValue, false, "Opposite motion direction", null, [], null);
        }

        var dtw = BoundedDtw(candidateFrames, templateFrames, bandRadius: Math.Max(5, candidateFrames.Count / 4));
        var warpRatio = dtw.Path.Count / (float)Math.Max(candidateFrames.Count, templateFrames.Count);
        if (warpRatio > WarpRatioLimit)
        {
            return new GestureTemplateComparison(
                dtw.Score,
                false,
                $"Excessive DTW warp {warpRatio:0.00}",
                warpRatio,
                dtw.Path,
                dtw.Breakdown);
        }

        var thresholdScore = ConfidenceToScore(ConfidenceThreshold);
        return new GestureTemplateComparison(
            dtw.Score,
            dtw.Score <= thresholdScore,
            dtw.Score <= thresholdScore ? "Eligible" : "Below unified threshold",
            warpRatio,
            dtw.Path,
            dtw.Breakdown);
    }

    private static bool HasOppositeMotionDirection(
        IReadOnlyList<GestureFeatureFrame> candidateFrames,
        IReadOnlyList<GestureFeatureFrame> templateFrames)
    {
        var candidateDx = candidateFrames[^1].CenterX - candidateFrames[0].CenterX;
        var candidateDy = candidateFrames[^1].CenterY - candidateFrames[0].CenterY;
        var templateDx = templateFrames[^1].CenterX - templateFrames[0].CenterX;
        var templateDy = templateFrames[^1].CenterY - templateFrames[0].CenterY;
        var candidateMagnitude = MathF.Sqrt((candidateDx * candidateDx) + (candidateDy * candidateDy));
        var templateMagnitude = MathF.Sqrt((templateDx * templateDx) + (templateDy * templateDy));
        if (candidateMagnitude < 0.18f || templateMagnitude < 0.18f)
        {
            return false;
        }

        var dot = ((candidateDx * templateDx) + (candidateDy * templateDy)) / Math.Max(0.001f, candidateMagnitude * templateMagnitude);
        return dot < -0.25f;
    }

    private static string TopologyRejectionReason(
        GestureTopologySummary? candidate,
        GestureTopologySummary? template)
    {
        if (candidate is null || template is null || template.TopologyChangeScore < GestureTemplateFactory.MinimumTopologyChangeScore)
        {
            return string.Empty;
        }

        if (template.PalmTurnScore >= GestureTemplateFactory.MinimumPalmTurnScore)
        {
            if (candidate.StartFingerStateMask != template.StartFingerStateMask
                || candidate.EndFingerStateMask != template.EndFingerStateMask)
            {
                return
                    $"Finger state mismatch candidate {GestureFeatureExtractor.FormatFingerMask(candidate.StartFingerStateMask)}->{GestureFeatureExtractor.FormatFingerMask(candidate.EndFingerStateMask)} " +
                    $"template {GestureFeatureExtractor.FormatFingerMask(template.StartFingerStateMask)}->{GestureFeatureExtractor.FormatFingerMask(template.EndFingerStateMask)}";
            }

            if (candidate.PalmTurnScore < template.PalmTurnScore * 0.55f)
            {
                return $"Palm turn too weak {candidate.PalmTurnScore:0.000} < template {template.PalmTurnScore:0.000}";
            }

            if (template.SignedPalmAreaSignChanges > 0 && candidate.SignedPalmAreaSignChanges == 0)
            {
                return "Palm area did not cross sides";
            }

            if (template.PalmCompressionDrop >= GestureTemplateFactory.MinimumPalmCompressionDrop
                && candidate.PalmCompressionDrop < template.PalmCompressionDrop * 0.55f)
            {
                return $"Palm compression too small {candidate.PalmCompressionDrop:0.000} < template {template.PalmCompressionDrop:0.000}";
            }
        }

        if (candidate.TopologyChangeScore < template.TopologyChangeScore * 0.55f)
        {
            return $"Topology change too small {candidate.TopologyChangeScore:0.000} < template {template.TopologyChangeScore:0.000}";
        }

        if (template.PalmTravel >= GestureTemplateFactory.MinimumPalmTravel
            && candidate.PalmTravel < template.PalmTravel * 0.45f)
        {
            return $"Palm travel too small {candidate.PalmTravel:0.000} < template {template.PalmTravel:0.000}";
        }

        if (template.PalmOrientationRangeRadians >= GestureTemplateFactory.MinimumPalmOrientationRangeRadians
            && candidate.PalmOrientationRangeRadians < template.PalmOrientationRangeRadians * 0.50f)
        {
            return $"Palm angle change too small {candidate.PalmOrientationRangeRadians * 180 / MathF.PI:0.0} deg < template {template.PalmOrientationRangeRadians * 180 / MathF.PI:0.0} deg";
        }

        if (template.HandednessRange >= GestureTemplateFactory.MinimumHandednessRange
            && candidate.HandednessRange < template.HandednessRange * 0.50f)
        {
            return $"Handedness change too small {candidate.HandednessRange:0.000} < template {template.HandednessRange:0.000}";
        }

        if (template.FingerStateTransitionCount > 0 && candidate.FingerStateTransitionCount == 0)
        {
            return "Finger topology did not change";
        }

        return string.Empty;
    }

    private static GestureScoreBreakdown FeatureFrameDistance(GestureFeatureFrame candidate, GestureFeatureFrame template)
    {
        const int jointOffset = 0;
        const int jointLength = 21 * 2;
        const int curlOffset = jointOffset + jointLength;
        const int curlLength = 5;
        const int spacingOffset = curlOffset + curlLength;
        const int spacingLength = 4;
        const int fingerStateOffset = spacingOffset + spacingLength;
        const int fingerStateLength = 5;

        var joint = PairDistance(candidate.Values, template.Values, jointOffset, jointLength);
        var bone = BoneDistance(candidate.Values, template.Values);
        var curl = ScalarDistance(candidate.Values, template.Values, curlOffset, curlLength);
        var fingerState = FingerStateDistance(candidate, template, fingerStateOffset, fingerStateLength);
        var spacing = ScalarDistance(candidate.Values, template.Values, spacingOffset, spacingLength);
        var palmTurn = PalmTurnDistance(candidate, template);
        var depth = MathF.Abs(GestureFeatureExtractor.ReadPalmDepthRange(candidate) - GestureFeatureExtractor.ReadPalmDepthRange(template));
        var motionDx = candidate.CenterX - template.CenterX;
        var motionDy = candidate.CenterY - template.CenterY;
        var velocity = candidate.PalmVelocity - template.PalmVelocity;
        var motion = MathF.Sqrt((motionDx * motionDx) + (motionDy * motionDy)) + (MathF.Abs(velocity) * 0.12f);
        var total = (joint * 0.22f)
            + (bone * 0.14f)
            + (curl * 0.08f)
            + (fingerState * 0.22f)
            + (spacing * 0.04f)
            + (motion * 0.12f)
            + (palmTurn * 0.14f)
            + (depth * 0.04f);
        return new GestureScoreBreakdown(joint, bone, curl, fingerState, spacing, motion, palmTurn, depth, total);
    }

    private static GestureScoreBreakdown AverageBreakdown(
        IReadOnlyList<GestureFeatureFrame> candidate,
        IReadOnlyList<GestureFeatureFrame> template,
        IReadOnlyList<GestureDtwPoint> path)
    {
        if (path.Count == 0)
        {
            return new GestureScoreBreakdown(0, 0, 0, 0, 0, 0, 0, 0, float.MaxValue);
        }

        var joint = 0f;
        var bone = 0f;
        var curl = 0f;
        var fingerState = 0f;
        var spacing = 0f;
        var motion = 0f;
        var palmTurn = 0f;
        var depth = 0f;
        var total = 0f;
        foreach (var point in path)
        {
            var breakdown = FeatureFrameDistance(candidate[point.CandidateIndex], template[point.TemplateIndex]);
            joint += breakdown.JointScore;
            bone += breakdown.BoneScore;
            curl += breakdown.CurlScore;
            fingerState += breakdown.FingerStateScore;
            spacing += breakdown.SpacingScore;
            motion += breakdown.MotionScore;
            palmTurn += breakdown.PalmTurnScore;
            depth += breakdown.DepthScore;
            total += breakdown.TotalScore;
        }

        var count = path.Count;
        return new GestureScoreBreakdown(joint / count, bone / count, curl / count, fingerState / count, spacing / count, motion / count, palmTurn / count, depth / count, total / count);
    }

    private static float PalmTurnDistance(GestureFeatureFrame candidate, GestureFeatureFrame template)
    {
        var area = MathF.Abs(GestureFeatureExtractor.ReadSignedPalmArea(candidate) - GestureFeatureExtractor.ReadSignedPalmArea(template)) * 4f;
        var compression = MathF.Abs(GestureFeatureExtractor.ReadPalmCompression(candidate) - GestureFeatureExtractor.ReadPalmCompression(template));
        return (area * 0.55f) + (compression * 0.45f);
    }

    private static float PairDistance(float[] candidate, float[] template, int offset, int length)
    {
        var end = Math.Min(Math.Min(candidate.Length, template.Length), offset + length);
        var sum = 0f;
        var terms = 0;
        for (var i = offset; i + 1 < end; i += 2)
        {
            var dx = candidate[i] - template[i];
            var dy = candidate[i + 1] - template[i + 1];
            sum += MathF.Sqrt((dx * dx) + (dy * dy));
            terms++;
        }

        return terms == 0 ? 0 : sum / terms;
    }

    private static float ScalarDistance(float[] candidate, float[] template, int offset, int length)
    {
        var end = Math.Min(Math.Min(candidate.Length, template.Length), offset + length);
        var sum = 0f;
        var terms = 0;
        for (var i = offset; i < end; i++)
        {
            sum += MathF.Abs(candidate[i] - template[i]);
            terms++;
        }

        return terms == 0 ? 0 : sum / terms;
    }

    private static float FingerStateDistance(
        GestureFeatureFrame candidate,
        GestureFeatureFrame template,
        int offset,
        int length)
    {
        if (candidate.FingerStateMask != 0 || template.FingerStateMask != 0)
        {
            var mismatches = 0;
            for (var i = 0; i < 5; i++)
            {
                var candidateOpen = (candidate.FingerStateMask & (1 << i)) != 0;
                var templateOpen = (template.FingerStateMask & (1 << i)) != 0;
                if (candidateOpen != templateOpen)
                {
                    mismatches++;
                }
            }

            return mismatches / 5f;
        }

        var straightnessDistance = ScalarDistance(candidate.Values, template.Values, offset, length);
        var candidateMask = GestureFeatureExtractor.ReadFingerStateMask(candidate.Values);
        var templateMask = GestureFeatureExtractor.ReadFingerStateMask(template.Values);
        var maskMismatches = 0;
        for (var i = 0; i < 5; i++)
        {
            if (((candidateMask ^ templateMask) & (1 << i)) != 0)
            {
                maskMismatches++;
            }
        }

        return Math.Max(straightnessDistance, maskMismatches / 5f);
    }

    private static float BoneDistance(float[] candidate, float[] template)
    {
        ReadOnlySpan<(int Start, int End)> connections =
        [
            (0, 1), (1, 2), (2, 3), (3, 4),
            (0, 5), (5, 6), (6, 7), (7, 8),
            (5, 9), (9, 10), (10, 11), (11, 12),
            (9, 13), (13, 14), (14, 15), (15, 16),
            (13, 17), (17, 18), (18, 19), (19, 20),
            (0, 17)
        ];

        var sum = 0f;
        var terms = 0;
        foreach (var (start, end) in connections)
        {
            var a = start * 2;
            var b = end * 2;
            if (b + 1 >= candidate.Length || b + 1 >= template.Length)
            {
                continue;
            }

            var cdx = candidate[b] - candidate[a];
            var cdy = candidate[b + 1] - candidate[a + 1];
            var tdx = template[b] - template[a];
            var tdy = template[b + 1] - template[a + 1];
            sum += MathF.Sqrt(((cdx - tdx) * (cdx - tdx)) + ((cdy - tdy) * (cdy - tdy)));
            terms++;
        }

        return terms == 0 ? 0 : sum / terms;
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

    private GestureRecognitionDebugSnapshot CreateDebugSnapshot(
        DateTimeOffset timestamp,
        GestureMatch? match,
        string candidateFailureReason,
        IReadOnlyList<GestureTemplateScore> scores,
        string triggerState,
        GestureConfirmedMatch? confirmedMatch)
    {
        var window = _window.CreateSnapshot();
        var windowFrames = CreateDebugWindowFrames(match, timestamp, confirmedMatch is not null);
        return new GestureRecognitionDebugSnapshot(
            timestamp,
            window.FrameCount,
            window.UsableFrameCount,
            Math.Max(0, window.DurationMilliseconds),
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
            null,
            match?.Comparison.Score,
            match?.Comparison.WarpRatio,
            string.IsNullOrWhiteSpace(candidateFailureReason)
                ? scores.FirstOrDefault(x => x.TemplateIndex > 0 && !x.IsEligible)?.Reason ?? string.Empty
                : candidateFailureReason,
            match?.Comparison.Path ?? [],
            windowFrames,
            match?.Comparison.Breakdown);
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
            var isUsable = sample.Landmarks.Count >= 21 && sample.Confidence >= 0.35f;
            var inMatchedWindow = match is not null
                && sample.Timestamp >= match.CandidateStart
                && sample.Timestamp <= match.CandidateEnd;
            frames.Add(new GestureDebugWindowFrame(
                Index: i,
                TimeOffsetMilliseconds: Math.Max(0, (sample.Timestamp - firstTimestamp).TotalMilliseconds),
                AgeMilliseconds: Math.Max(0, (now - sample.Timestamp).TotalMilliseconds),
                Confidence: sample.Confidence,
                IsUsable: isUsable,
                IsAcceptedMatch: acceptedMatch && isUsable && inMatchedWindow));
        }

        return frames;
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
            MatchThreshold: ConfidenceThreshold,
            triggerState,
            ConfirmedMatch: null,
            DetectedPath: GestureRecognitionPath.UnifiedSequence,
            WindowFrames: []);
    }

    internal sealed record DtwResult(float Score, IReadOnlyList<GestureDtwPoint> Path, GestureScoreBreakdown Breakdown);

    private sealed record GestureTemplateComparison(
        float Score,
        bool IsEligible,
        string Reason,
        float? WarpRatio,
        IReadOnlyList<GestureDtwPoint> Path,
        GestureScoreBreakdown? Breakdown);

    private sealed record GestureCandidateWindow(DateTimeOffset Start, DateTimeOffset End, GestureTemplate Template);

    private sealed record GestureMatch(
        GestureRecognitionResult Result,
        GestureTemplate CandidateTemplate,
        GestureTemplate BestTemplate,
        DateTimeOffset CandidateStart,
        DateTimeOffset CandidateEnd,
        GestureTemplateComparison Comparison);
}

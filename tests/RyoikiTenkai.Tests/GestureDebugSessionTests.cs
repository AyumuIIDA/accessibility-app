using RyoikiTenkai.Core;
using RyoikiTenkai.Vision;
using Xunit;

namespace RyoikiTenkai.Tests;

public sealed class GestureDebugSessionTests
{
    [Fact]
    public void AddHandFrame_LinksSampleAndSnapshot()
    {
        var session = new GestureDebugSession(TimeSpan.FromMinutes(5), 100);
        var sample = GestureTemplateFactoryTests.CreateHoldSamples(count: 1)[0];
        var snapshot = CreateSnapshot(sample.Timestamp, triggerState: "No match");

        var frame = session.AddHandFrame(
            sample,
            snapshot,
            recognition: null,
            hasBoundAction: false,
            source: "native",
            elapsedText: "12 ms");

        Assert.Equal(1, frame.SequenceNumber);
        Assert.Equal(GestureDebugFrameKind.Hand, frame.Kind);
        Assert.Same(sample, frame.Sample);
        Assert.Same(snapshot, frame.Snapshot);
        Assert.NotNull(frame.NormalizedSkeleton);
        Assert.Equal(21, frame.NormalizedSkeleton.Landmarks.Count);
        Assert.Equal("no confirmed gesture", frame.ActionState);
    }

    [Fact]
    public void AddNoHandFrame_RecordsResetReason()
    {
        var session = new GestureDebugSession(TimeSpan.FromMinutes(5), 100);
        var timestamp = new DateTimeOffset(2026, 7, 17, 0, 0, 0, TimeSpan.Zero);
        var snapshot = CreateSnapshot(timestamp, triggerState: "No hand for 500 ms");

        var frame = session.AddNoHandFrame(timestamp, "native hand unavailable", snapshot);

        Assert.Equal(GestureDebugFrameKind.NoHand, frame.Kind);
        Assert.Null(frame.Sample);
        Assert.Equal("native hand unavailable", frame.NoHandReason);
        Assert.Equal("recognizer reset", frame.ActionState);
    }

    [Fact]
    public void AddHandFrame_TrimsOldestFramesByCount()
    {
        var session = new GestureDebugSession(TimeSpan.FromMinutes(5), maxFrameCount: 3);
        var samples = GestureTemplateFactoryTests.CreateHoldSamples(count: 5);

        foreach (var sample in samples)
        {
            session.AddHandFrame(sample, CreateSnapshot(sample.Timestamp), null, false, "native", "1 ms");
        }

        Assert.Equal(3, session.Frames.Count);
        Assert.Equal(2, session.DroppedFrameCount);
        Assert.Equal(3, session.Frames[0].SequenceNumber);
        Assert.Equal(5, session.Latest?.SequenceNumber);
    }

    [Fact]
    public void ExportJsonLines_ExcludesImageBuffersAndIncludesScores()
    {
        var session = new GestureDebugSession(TimeSpan.FromMinutes(5), 100);
        var sample = GestureTemplateFactoryTests.CreateHoldSamples(count: 1)[0];
        session.AddHandFrame(
            sample,
            CreateSnapshot(sample.Timestamp, scores:
            [
                new GestureTemplateScore("hold", "Hold", 1, 0.2f, 0.78f, true, "Eligible", true)
            ]),
            new GestureRecognitionResult("hold", "Hold", 0.78f, "custom-unified", TimeSpan.FromSeconds(2)),
            hasBoundAction: true,
            source: "native",
            elapsedText: "8 ms");

        var path = Path.Combine(Path.GetTempPath(), $"ryoiki-debug-{Guid.NewGuid():N}.jsonl");
        try
        {
            session.ExportJsonLines(path);

            var json = File.ReadAllText(path);
            Assert.Contains("\"Landmarks\"", json);
            Assert.Contains("\"Scores\"", json);
            Assert.Contains("\"binding ready\"", json);
            Assert.DoesNotContain("Bgra", json, StringComparison.OrdinalIgnoreCase);
        }
        finally
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
    }

    [Fact]
    public void AppendJsonLine_WritesOneLinePerDebugFrame()
    {
        var session = new GestureDebugSession(TimeSpan.FromMinutes(5), 100);
        var samples = GestureTemplateFactoryTests.CreateHoldSamples(count: 2);
        var first = session.AddHandFrame(samples[0], CreateSnapshot(samples[0].Timestamp), null, false, "native", "1 ms");
        var second = session.AddHandFrame(samples[1], CreateSnapshot(samples[1].Timestamp), null, false, "native", "2 ms");
        var path = Path.Combine(Path.GetTempPath(), $"ryoiki-debug-live-{Guid.NewGuid():N}.jsonl");

        try
        {
            GestureDebugSession.AppendJsonLine(path, first);
            GestureDebugSession.AppendJsonLine(path, second);

            var lines = File.ReadAllLines(path);
            Assert.Equal(2, lines.Length);
            Assert.Contains("\"SequenceNumber\":1", lines[0]);
            Assert.Contains("\"SequenceNumber\":2", lines[1]);
            Assert.Contains("\"Landmarks\"", lines[0]);
        }
        finally
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
    }

    private static GestureRecognitionDebugSnapshot CreateSnapshot(
        DateTimeOffset timestamp,
        string triggerState = "No match",
        IReadOnlyList<GestureTemplateScore>? scores = null)
    {
        return new GestureRecognitionDebugSnapshot(
            Timestamp: timestamp,
            BufferFrameCount: 1,
            UsableFrameCount: 1,
            BufferDurationMilliseconds: 0,
            BufferEffectiveFps: 0,
            UsableEffectiveFps: 0,
            CandidateTemplate: null,
            CandidateFailureReason: "Waiting for candidate window",
            Scores: scores ?? [],
            BestGestureId: null,
            BestDisplayName: null,
            BestConfidence: null,
            BestTemplate: null,
            MatchThreshold: 0.72f,
            triggerState,
            ConfirmedMatch: null,
            WindowFrames:
            [
                new GestureDebugWindowFrame(0, 0, 0, 0.95f, true, false)
            ]);
    }
}

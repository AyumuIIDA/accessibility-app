using RyoikiTenkai.Core;
using RyoikiTenkai.Storage;
using RyoikiTenkai.Vision;
using Xunit;

namespace RyoikiTenkai.Tests;

public sealed class GestureDefinitionStoreTests
{
    [Fact]
    public void SaveAndLoad_RoundTripsCustomGestureTemplates()
    {
        var path = Path.Combine(Path.GetTempPath(), $"gestures-{Guid.NewGuid():N}.json");
        try
        {
            var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateWaveSamples());
            Assert.NotNull(template);

            var store = new GestureDefinitionStore(path);
            store.Save([
                new GestureDefinition(
                    Id: "wave",
                    DisplayName: "Wave",
                    Type: "template",
                    Templates: [template],
                    CreatedAt: DateTimeOffset.UtcNow,
                    Recordings:
                    [
                        new GestureRecording(
                            Id: "take-1",
                            TakeIndex: 1,
                            CapturedAt: DateTimeOffset.UtcNow,
                            DurationMilliseconds: 1980,
                            AverageConfidence: 0.95f,
                            Quality: new GestureRecordingQuality(true, 60, 60, 60, 30, 30, "Accepted"),
                            Frames:
                            [
                                new GestureRecordingFrame(
                                    TimeOffsetMilliseconds: 0,
                                    Confidence: 0.95f,
                                    Handedness: 0,
                                    BoundingBox: null,
                                    Landmarks: GestureTemplateFactoryTests.CreateHand(100, 240, 100)
                                        .Select(point => new GestureSkeletonPoint(point.X, point.Y, point.Z))
                                        .ToList())
                            ])
                    ])
            ]);

            var loaded = store.Load();

            Assert.Single(loaded);
            Assert.Equal("wave", loaded[0].Id);
            Assert.Single(loaded[0].Templates);
            Assert.Equal(template.Samples.Count, loaded[0].Templates[0].Samples.Count);
            Assert.Single(loaded[0].Recordings!);
            Assert.Equal(21, loaded[0].Recordings![0].Frames[0].Landmarks.Count);
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
    public void Load_MigratesV2RecordingSkeletonsIntoPalmTurnFeatureTracks()
    {
        var path = Path.Combine(Path.GetTempPath(), $"gestures-{Guid.NewGuid():N}.json");
        try
        {
            var samples = GestureTemplateFactoryTests.CreatePalmTurnSamples();
            var legacyTemplate = GestureTemplateFactory.Create(samples);
            Assert.NotNull(legacyTemplate);
            legacyTemplate = legacyTemplate with { FeatureTrack = null };

            var recording = CreateRecording("take-1", samples);
            var store = new GestureDefinitionStore(path);
            store.Save([
                new GestureDefinition(
                    Id: "palm_flip",
                    DisplayName: "palm flip",
                    Type: "template",
                    Templates: [legacyTemplate],
                    CreatedAt: DateTimeOffset.UtcNow,
                    Recordings: [recording],
                    SchemaVersion: 2)
            ]);

            var loaded = store.Load();

            var gesture = Assert.Single(loaded);
            Assert.Equal(3, gesture.SchemaVersion);
            var template = Assert.Single(gesture.Templates);
            Assert.NotNull(template.FeatureTrack);
            Assert.NotNull(template.Topology);
            Assert.True(template.Topology.PalmTurnScore >= GestureTemplateFactory.MinimumPalmTurnScore);
            Assert.True(template.Topology.SignedPalmAreaSignChanges > 0);
            Assert.NotNull(gesture.Recordings![0].FeatureTrack);
            Assert.All(gesture.Recordings[0].Frames, frame => Assert.NotNull(frame.Features));
        }
        finally
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
    }

    private static GestureRecording CreateRecording(string id, IReadOnlyList<GestureFrameSample> samples)
    {
        var first = samples[0].Timestamp;
        var duration = (samples[^1].Timestamp - first).TotalMilliseconds;
        return new GestureRecording(
            id,
            1,
            first,
            duration,
            (float)samples.Average(x => x.Confidence),
            new GestureRecordingQuality(true, samples.Count, samples.Count, samples.Count, samples.Count * 1000.0 / duration, samples.Count * 1000.0 / duration, "Accepted"),
            samples.Select(sample => new GestureRecordingFrame(
                (sample.Timestamp - first).TotalMilliseconds,
                sample.Confidence,
                sample.Handedness,
                sample.BoundingBox is null
                    ? null
                    : new GestureRecordingBox(
                        sample.BoundingBox.X1,
                        sample.BoundingBox.Y1,
                        sample.BoundingBox.X2,
                        sample.BoundingBox.Y2),
                sample.Landmarks
                    .Take(21)
                    .Select(point => new GestureSkeletonPoint(point.X, point.Y, point.Z))
                    .ToList()))
                .ToList());
    }
}

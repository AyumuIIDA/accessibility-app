using System.Text.Json;
using System.Text.Json.Serialization;
using RyoikiTenkai.Core;
using RyoikiTenkai.Vision;

namespace RyoikiTenkai.Storage;

internal sealed class GestureDefinitionStore
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        Converters = { new JsonStringEnumConverter() }
    };

    private readonly string _path;

    public GestureDefinitionStore(string path)
    {
        _path = path;
    }

    public List<GestureDefinition> Load()
    {
        if (!File.Exists(_path))
        {
            return [];
        }

        var json = File.ReadAllText(_path);
        var gestures = JsonSerializer.Deserialize<List<GestureDefinition>>(json, JsonOptions) ?? [];
        return gestures
            .Select(Migrate)
            .ToList();
    }

    public void Save(List<GestureDefinition> gestures)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
        File.WriteAllText(_path, JsonSerializer.Serialize(gestures, JsonOptions));
    }

    private static GestureDefinition Migrate(GestureDefinition gesture)
    {
        var recordings = gesture.Recordings?.Select(EnrichRecording).ToList();
        var templates = gesture.Templates
            .Select(template => GestureTemplateFactory.NormalizeLegacyTemplate(template, gesture.Type))
            .ToList();

        if (gesture.SchemaVersion < 3 && recordings is { Count: > 0 })
        {
            var migratedTemplates = recordings
                .Where(x => x.Quality.Accepted)
                .Select(recording =>
                {
                    var samples = RecordingToSamples(recording);
                    var template = GestureTemplateFactory.Create(samples);
                    return template is null ? null : template with { SourceRecordingId = recording.Id };
                })
                .Where(x => x is not null)
                .Select(x => x!)
                .ToList();
            if (migratedTemplates.Count > 0)
            {
                templates = migratedTemplates;
            }
        }

        return gesture with
        {
            Templates = templates,
            Recordings = recordings,
            SchemaVersion = Math.Max(gesture.SchemaVersion, 4)
        };
    }

    private static GestureRecording EnrichRecording(GestureRecording recording)
    {
        if (recording.FeatureTrack is not null && recording.Frames.All(x => x.Features is not null))
        {
            return recording;
        }

        var samples = RecordingToSamples(recording);
        var track = GestureFeatureExtractor.BuildFeatureTrack(samples);
        var featureByTime = track.Frames
            .GroupBy(x => Math.Round(x.TimeOffsetMilliseconds, 3))
            .ToDictionary(x => x.Key, x => x.First());
        var frames = recording.Frames
            .Select(frame => frame.Features is not null
                ? frame
                : frame with
                {
                    Features = featureByTime.TryGetValue(Math.Round(frame.TimeOffsetMilliseconds, 3), out var features)
                        ? features
                        : null
                })
            .ToList();
        return recording with { Frames = frames, FeatureTrack = track };
    }

    private static List<GestureFrameSample> RecordingToSamples(GestureRecording recording)
    {
        return recording.Frames
            .Where(x => x.Landmarks.Count >= 21)
            .OrderBy(x => x.TimeOffsetMilliseconds)
            .Select(frame => new GestureFrameSample(
                recording.CapturedAt + TimeSpan.FromMilliseconds(frame.TimeOffsetMilliseconds),
                frame.Landmarks
                    .Take(21)
                    .Select(point => new HandLandmark(point.X, point.Y, point.Z))
                    .ToList(),
                frame.Confidence,
                frame.BoundingBox is null
                    ? null
                    : new HandBox(
                        frame.BoundingBox.X1,
                        frame.BoundingBox.Y1,
                        frame.BoundingBox.X2,
                        frame.BoundingBox.Y2),
                frame.Handedness))
            .ToList();
    }
}

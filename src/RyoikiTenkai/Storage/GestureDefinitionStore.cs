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
            .Select(gesture => gesture with
            {
                Templates = gesture.Templates
                    .Select(template => GestureTemplateFactory.NormalizeLegacyTemplate(template, gesture.Type))
                    .ToList()
            })
            .ToList();
    }

    public void Save(List<GestureDefinition> gestures)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
        File.WriteAllText(_path, JsonSerializer.Serialize(gestures, JsonOptions));
    }
}

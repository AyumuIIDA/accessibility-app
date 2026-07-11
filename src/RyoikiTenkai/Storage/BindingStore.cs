using System.Text.Json;
using System.Text.Json.Serialization;
using RyoikiTenkai.Core;

namespace RyoikiTenkai.Storage;

internal sealed class BindingStore
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        Converters = { new JsonStringEnumConverter() }
    };

    private readonly string _path;

    public BindingStore(string path)
    {
        _path = path;
    }

    public List<GestureBinding> Load()
    {
        if (!File.Exists(_path))
        {
            return [];
        }

        var json = File.ReadAllText(_path);
        return JsonSerializer.Deserialize<List<GestureBinding>>(json, JsonOptions) ?? [];
    }

    public void Save(List<GestureBinding> bindings)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
        File.WriteAllText(_path, JsonSerializer.Serialize(bindings, JsonOptions));
    }
}

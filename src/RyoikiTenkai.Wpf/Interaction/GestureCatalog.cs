using RyoikiTenkai.Core;
using RyoikiTenkai.Wpf.Native;

namespace RyoikiTenkai.Wpf.Interaction;

internal sealed record SavedGestureItem(uint Id, string Name, bool Enabled, uint TakeCount)
{
    public override string ToString() =>
        $"{Name}  ·  {TakeCount} takes  ·  {(Enabled ? "Enabled" : "Disabled")}";
}

/// <summary>
/// Managed view over the native gesture repository. The main window owns one
/// instance so definitions and command bindings stay loaded while the action
/// window is closed; the action window only renders and mutates through it.
/// </summary>
internal sealed class GestureCatalog
{
    private static readonly string[] SupportedActionTypes =
        ["app.launch", "keyboard.typeText", "keyboard.hotkey", "handoff.grab", "handoff.release"];

    private readonly NativeVisionHost _host;
    private readonly MutableGestureBindingResolver _resolver;
    private readonly Dictionary<uint, string> _names = [];
    private readonly Dictionary<uint, GestureCommandBinding> _bindings = [];

    public GestureCatalog(NativeVisionHost host, MutableGestureBindingResolver resolver)
    {
        _host = host;
        _resolver = resolver;
    }

    public IReadOnlyList<SavedGestureItem> Items { get; private set; } = [];

    /// <summary>Outcome of the last repository read or command, for display.</summary>
    public string Status { get; private set; } = "Loading saved gestures…";

    public event EventHandler? Refreshed;

    public string NameOf(uint definitionId, string fallback) =>
        _names.GetValueOrDefault(definitionId, fallback);

    public bool TryGetName(uint definitionId, out string name) =>
        _names.TryGetValue(definitionId, out name!);

    public void NoteName(uint definitionId, string name) => _names[definitionId] = name;

    public GestureCommandBinding? BindingFor(uint definitionId) =>
        _bindings.GetValueOrDefault(definitionId);

    public bool PrepareDefinition(uint definitionId, string name) =>
        TryOperation(() => _host.SetGestureDefinitionMetadata(definitionId, name, true),
            "Ready to record.", "The gesture name could not be prepared for saving.");

    public bool SaveMetadata(uint definitionId, string name, bool enabled) =>
        TryOperation(() => _host.SetGestureDefinitionMetadata(definitionId, name, enabled),
            "Gesture updated.", "The gesture could not be updated.");

    public bool DeleteDefinition(uint definitionId)
    {
        if (!TryOperation(() => _host.DeleteGestureDefinition(definitionId),
            "Gesture deleted.", "The gesture could not be deleted.")) return false;
        _names.Remove(definitionId);
        return true;
    }

    public bool SaveBinding(uint definitionId, string actionType, string parameter, bool enabled)
    {
        if (!TryOperation(() => _host.UpsertGestureBinding(definitionId, actionType, parameter, enabled),
            "Action binding saved.", "The action binding could not be saved.")) return false;
        RefreshBindings();
        return true;
    }

    public bool RemoveBinding(uint definitionId)
    {
        if (!TryOperation(() => _host.DeleteGestureBinding(definitionId),
            "Action binding removed.", "The action binding could not be removed.")) return false;
        RefreshBindings();
        return true;
    }

    public bool Reload() => TryOperation(_host.ReloadGestureDefinitions,
        "Saved gestures reloaded.", "Saved gestures could not be reloaded.");

    /// <summary>
    /// Native repository commands are applied on the capture thread, so the
    /// snapshot is only re-read after the runtime has had a chance to fail.
    /// </summary>
    public async Task RefreshAfterCommandAsync()
    {
        await Task.Delay(500);
        var error = _host.GetLastErrorMessage();
        if (error.StartsWith("Gesture repository:", StringComparison.Ordinal))
        {
            Status = error;
            Refreshed?.Invoke(this, EventArgs.Empty);
            return;
        }
        Refresh();
    }

    public void Refresh()
    {
        try
        {
            if (!_host.TryListGestureDefinitions(out var definitions))
            {
                Status = "Saved gestures could not be read.";
                Refreshed?.Invoke(this, EventArgs.Empty);
                return;
            }
            var items = new List<SavedGestureItem>();
            _names.Clear();
            var count = (int)Math.Min(definitions.Count, (uint)NativeVisionInterop.MaxGestureDefinitions);
            for (var index = 0; index < count; ++index)
            {
                var metadata = definitions.GetItem(index);
                var item = new SavedGestureItem(metadata.DefinitionId, metadata.GetName(),
                    metadata.Enabled != 0, metadata.TakeCount);
                items.Add(item);
                _names[item.Id] = item.Name;
            }
            Items = items;
            RefreshBindings();
            var error = definitions.GetError();
            Status = !string.IsNullOrWhiteSpace(error)
                ? error
                : count == 0 ? "No saved gestures yet."
                : $"{count} saved gesture{(count == 1 ? string.Empty : "s")}.";
        }
        catch (Exception exception) when (exception is EntryPointNotFoundException
            or DllNotFoundException or BadImageFormatException)
        {
            Status = "Native gesture storage is unavailable: " + exception.Message;
        }
        Refreshed?.Invoke(this, EventArgs.Empty);
    }

    internal static string ParameterNameFor(string actionType) => actionType switch
    {
        "app.launch" => "path",
        "keyboard.typeText" => "text",
        "keyboard.hotkey" => "hotkey",
        _ => string.Empty
    };

    /// <summary>Handoff actions carry no parameter; every other action requires one.</summary>
    internal static bool RequiresParameter(string actionType) =>
        ParameterNameFor(actionType).Length != 0;

    private bool TryOperation(Func<bool> operation, string success, string failure)
    {
        if (!_host.IsStarted)
        {
            Status = "Start the camera to change saved gestures.";
            return false;
        }
        try
        {
            if (!operation())
            {
                Status = failure;
                return false;
            }
            Status = success;
            return true;
        }
        catch (Exception exception) when (exception is EntryPointNotFoundException
            or DllNotFoundException or BadImageFormatException)
        {
            Status = "Native gesture storage is unavailable: " + exception.Message;
            return false;
        }
    }

    private void RefreshBindings()
    {
        _bindings.Clear();
        if (!_host.TryListGestureBindings(out var bindings))
        {
            _resolver.Replace([]);
            return;
        }
        var count = (int)Math.Min(bindings.Count, (uint)NativeVisionInterop.MaxGestureDefinitions);
        for (var index = 0; index < count; ++index)
        {
            var native = bindings.GetItem(index);
            var actionType = native.GetActionType();
            if (!SupportedActionTypes.Contains(actionType)) continue;
            var parameterName = ParameterNameFor(actionType);
            _bindings[native.DefinitionId] = new GestureCommandBinding(native.DefinitionId,
                NameOf(native.DefinitionId, $"Gesture {native.DefinitionId}"),
                native.Enabled != 0,
                new ActionSpec(actionType, parameterName.Length == 0
                    ? [] : new Dictionary<string, string> { [parameterName] = native.GetActionParameter() }));
        }
        _resolver.Replace(_bindings.Values);
    }
}

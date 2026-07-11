using RyoikiTenkai.Actions;
using RyoikiTenkai.Storage;

namespace RyoikiTenkai.Core;

internal sealed class GestureDispatcher
{
    private readonly BindingStore _store;
    private readonly ActionExecutor _executor;

    public GestureDispatcher(BindingStore store, ActionExecutor executor)
    {
        _store = store;
        _executor = executor;
    }

    public void Dispatch(GestureEvent gesture)
    {
        Console.WriteLine($"Gesture detected: {gesture.GestureId} (source={gesture.Source}, confidence={gesture.Confidence:0.00})");

        var binding = _store.Load()
            .FirstOrDefault(x => StringComparer.OrdinalIgnoreCase.Equals(x.GestureId, gesture.GestureId));

        if (binding is null)
        {
            Console.WriteLine("No binding found.");
            return;
        }

        Console.WriteLine($"Executing: {binding.DisplayName}");
        _executor.Execute(binding.Action);
    }
}

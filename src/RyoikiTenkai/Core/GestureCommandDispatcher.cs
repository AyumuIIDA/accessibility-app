using RyoikiTenkai.Actions;

namespace RyoikiTenkai.Core;

internal sealed class GestureCommandDispatcher
{
    private readonly IGestureBindingResolver _bindings;
    private readonly IActionExecutor _executor;

    public GestureCommandDispatcher(IGestureBindingResolver bindings, IActionExecutor executor)
    {
        _bindings = bindings;
        _executor = executor;
    }

    public async Task<GestureDispatchResult> DispatchAsync(
        ConfirmedGestureEvent gesture,
        CancellationToken cancellationToken)
    {
        var binding = _bindings.Resolve(gesture.GestureId);
        if (binding is null)
            return new(false, gesture.GestureId, "No binding is configured.");
        if (!binding.Enabled)
            return new(false, gesture.GestureId, "The binding is disabled.");

        await _executor.ExecuteAsync(binding.Action, cancellationToken).ConfigureAwait(false);
        return new(true, gesture.GestureId, binding.DisplayName);
    }
}

internal sealed record GestureDispatchResult(bool Executed, uint GestureId, string Message);

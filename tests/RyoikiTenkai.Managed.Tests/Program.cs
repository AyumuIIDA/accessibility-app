using RyoikiTenkai.Actions;
using RyoikiTenkai.Core;

var resolver = new MutableGestureBindingResolver();
var executor = new RecordingExecutor();
var dispatcher = new GestureCommandDispatcher(resolver, executor);
var missing = await dispatcher.DispatchAsync(Event(7), CancellationToken.None);
Require(!missing.Executed && executor.Actions.Count == 0, "Unbound event executed an action.");

resolver.Replace([
    new GestureCommandBinding(7, "Demo", true,
        new ActionSpec("keyboard.hotkey", new() { ["hotkey"] = "CTRL+K" })),
    new GestureCommandBinding(8, "Disabled", false,
        new ActionSpec("app.launch", new() { ["path"] = "notepad.exe" }))]);
var disabled = await dispatcher.DispatchAsync(Event(8), CancellationToken.None);
Require(!disabled.Executed && executor.Actions.Count == 0, "Disabled binding executed.");
var executed = await dispatcher.DispatchAsync(Event(7), CancellationToken.None);
Require(executed.Executed && executor.Actions.Count == 1, "Enabled binding was not dispatched.");
Require(executor.Actions[0].Type == "keyboard.hotkey", "Command payload changed in dispatch.");
Require(HandoffService.SafeFileName("..\\..\\payload.png") == "payload.png",
    "Handoff filename traversal was not removed.");
Require(HandoffService.SafeFileName("") == "handoff.bin",
    "Empty handoff filename did not receive a safe default.");

// LAN handoff has no manual controls, so the gesture action path is the only
// way in and must fail loudly instead of silently doing nothing.
var unwired = new ActionExecutor();
await RequireThrowsAsync<InvalidOperationException>(
    () => unwired.ExecuteAsync(new ActionSpec("handoff.grab", new()), CancellationToken.None),
    "handoff.grab without a handoff service did not fail.");
await RequireThrowsAsync<InvalidOperationException>(
    () => unwired.ExecuteAsync(new ActionSpec("handoff.release", new()), CancellationToken.None),
    "handoff.release without a handoff service did not fail.");

// Port 0 keeps the status assertions off the real discovery port.
await using (var handoff = new HandoffService(new StubHandoffPayloadProvider(), _ => { }, offerPort: 0))
{
    Require(handoff.GetStatus().State == HandoffState.Idle,
        "A fresh handoff service did not report Idle.");
    await RequireThrowsAsync<InvalidOperationException>(
        () => handoff.ReleaseHereAsync(CancellationToken.None),
        "Releasing without an offer did not fail.");
    Require(handoff.GetStatus().State == HandoffState.Failed,
        "A failed release was not visible in the handoff status.");
}

Console.WriteLine("Managed gesture command tests passed.");

static ConfirmedGestureEvent Event(uint id) => new(1, id, .9F, .8F, 10, 20);
static void Require(bool value, string message) { if (!value) throw new InvalidOperationException(message); }

static async Task RequireThrowsAsync<TException>(Func<Task> action, string message)
    where TException : Exception
{
    try { await action(); }
    catch (TException) { return; }
    throw new InvalidOperationException(message);
}

sealed class StubHandoffPayloadProvider : IHandoffPayloadProvider
{
    public Task<HandoffPayload?> CaptureScreenshotAsync(CancellationToken cancellationToken) =>
        Task.FromResult<HandoffPayload?>(null);
    public Task<string> SaveReceivedFileAsync(string fileName, ReadOnlyMemory<byte> data,
        CancellationToken cancellationToken) => Task.FromResult(fileName);
    public Task OpenReceivedFileAsync(string path, CancellationToken cancellationToken) =>
        Task.CompletedTask;
}

sealed class RecordingExecutor : IActionExecutor
{
    public List<ActionSpec> Actions { get; } = [];
    public Task ExecuteAsync(ActionSpec action, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        Actions.Add(action);
        return Task.CompletedTask;
    }
}

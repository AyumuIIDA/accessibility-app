using System.Diagnostics;
using RyoikiTenkai.Core;

namespace RyoikiTenkai.Actions;

internal sealed class ActionExecutor
{
    private readonly HandoffService? _handoffService;

    public ActionExecutor(HandoffService? handoffService = null)
    {
        _handoffService = handoffService;
    }

    public void Execute(ActionSpec action)
    {
        ExecuteSynchronous(action);
    }

    public async Task ExecuteAsync(ActionSpec action, CancellationToken cancellationToken)
    {
        switch (action.Type)
        {
            case "app.launch":
                await Task.Run(() => Launch(action), cancellationToken).ConfigureAwait(false);
                break;
            case "keyboard.typeText":
                await Task.Run(() => TypeText(action), cancellationToken).ConfigureAwait(false);
                break;
            case "keyboard.hotkey":
                await Task.Run(() => Hotkey(action), cancellationToken).ConfigureAwait(false);
                break;
            case "handoff.grabScreenshot":
                await GrabScreenshotAsync(cancellationToken);
                break;
            case "handoff.releaseHere":
                await ReleaseHereAsync(cancellationToken);
                break;
            default:
                await Task.Run(() => ExecuteSynchronous(action), cancellationToken).ConfigureAwait(false);
                break;
        }
    }

    private void ExecuteSynchronous(ActionSpec action)
    {
        switch (action.Type)
        {
            case "app.launch":
                Launch(action);
                break;
            case "keyboard.typeText":
                TypeText(action);
                break;
            case "keyboard.hotkey":
                Hotkey(action);
                break;
            default:
                Console.WriteLine($"Unsupported action type: {action.Type}");
                break;
        }
    }

    private static void Launch(ActionSpec action)
    {
        if (!action.Params.TryGetValue("path", out var path) || string.IsNullOrWhiteSpace(path))
        {
            Console.WriteLine("Missing action param: path");
            return;
        }

        Process.Start(new ProcessStartInfo
        {
            FileName = path,
            UseShellExecute = true
        });
    }

    private static void TypeText(ActionSpec action)
    {
        if (!action.Params.TryGetValue("text", out var text))
        {
            Console.WriteLine("Missing action param: text");
            return;
        }

        if (!OperatingSystem.IsWindows())
        {
            Console.WriteLine("keyboard.typeText is Windows-only.");
            return;
        }

        KeyboardInput.SendUnicodeText(text);
    }

    private static void Hotkey(ActionSpec action)
    {
        if (!action.Params.TryGetValue("hotkey", out var hotkey))
        {
            Console.WriteLine("Missing action param: hotkey");
            return;
        }

        if (!OperatingSystem.IsWindows())
        {
            Console.WriteLine("keyboard.hotkey is Windows-only.");
            return;
        }

        if (!KeyboardInput.TrySendHotkey(hotkey, out var error))
        {
            Console.WriteLine(error);
        }
    }

    private async Task GrabScreenshotAsync(CancellationToken cancellationToken)
    {
        if (_handoffService is null)
        {
            Console.WriteLine("handoff.grabScreenshot requires a handoff service.");
            return;
        }

        await _handoffService.GrabScreenshotAsync(cancellationToken).ConfigureAwait(false);
    }

    private async Task ReleaseHereAsync(CancellationToken cancellationToken)
    {
        if (_handoffService is null)
        {
            Console.WriteLine("handoff.releaseHere requires a handoff service.");
            return;
        }

        await _handoffService.ReleaseHereAsync(cancellationToken).ConfigureAwait(false);
    }
}

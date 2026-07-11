using System.Diagnostics;
using RyoikiTenkai.Core;

namespace RyoikiTenkai.Actions;

internal sealed class ActionExecutor
{
    public void Execute(ActionSpec action)
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
}

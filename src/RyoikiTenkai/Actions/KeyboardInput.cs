using System.Runtime.InteropServices;

namespace RyoikiTenkai.Actions;

internal static class KeyboardInput
{
    private const uint INPUT_KEYBOARD = 1;
    private const uint KEYEVENTF_KEYUP = 0x0002;
    private const uint KEYEVENTF_UNICODE = 0x0004;

    private static readonly Dictionary<string, ushort> NamedKeys = new(StringComparer.OrdinalIgnoreCase)
    {
        ["enter"] = 0x0D,
        ["return"] = 0x0D,
        ["esc"] = 0x1B,
        ["escape"] = 0x1B,
        ["tab"] = 0x09,
        ["space"] = 0x20,
        ["backspace"] = 0x08,
        ["delete"] = 0x2E,
        ["del"] = 0x2E,
        ["home"] = 0x24,
        ["end"] = 0x23,
        ["pageup"] = 0x21,
        ["pagedown"] = 0x22,
        ["left"] = 0x25,
        ["up"] = 0x26,
        ["right"] = 0x27,
        ["down"] = 0x28,
        ["ctrl"] = 0x11,
        ["control"] = 0x11,
        ["shift"] = 0x10,
        ["alt"] = 0x12,
        ["win"] = 0x5B,
        ["windows"] = 0x5B
    };

    public static void SendUnicodeText(string text)
    {
        foreach (var ch in text)
        {
            var inputs = new INPUT[2];
            inputs[0].type = INPUT_KEYBOARD;
            inputs[0].u.ki.wScan = ch;
            inputs[0].u.ki.dwFlags = KEYEVENTF_UNICODE;

            inputs[1].type = INPUT_KEYBOARD;
            inputs[1].u.ki.wScan = ch;
            inputs[1].u.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

            var sent = SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<INPUT>());
            if (sent != inputs.Length)
            {
                Console.WriteLine($"SendInput failed for character: {ch}");
                return;
            }
        }
    }

    public static bool TrySendHotkey(string hotkey, out string error)
    {
        var keys = hotkey
            .Split('+', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries)
            .Select(ParseVirtualKey)
            .ToArray();

        if (keys.Any(x => x is null))
        {
            error = $"Unsupported hotkey: {hotkey}";
            return false;
        }

        var virtualKeys = keys.Select(x => x!.Value).ToArray();
        foreach (var key in virtualKeys)
        {
            SendVirtualKey(key, keyUp: false);
        }

        for (var i = virtualKeys.Length - 1; i >= 0; i--)
        {
            SendVirtualKey(virtualKeys[i], keyUp: true);
        }

        error = string.Empty;
        return true;
    }

    private static ushort? ParseVirtualKey(string key)
    {
        if (NamedKeys.TryGetValue(key, out var namedKey))
        {
            return namedKey;
        }

        if (key.Length == 1)
        {
            var ch = char.ToUpperInvariant(key[0]);
            if (ch is >= 'A' and <= 'Z')
            {
                return ch;
            }

            if (ch is >= '0' and <= '9')
            {
                return ch;
            }
        }

        if (key.Length is 2 or 3 && key[0] is 'f' or 'F' && int.TryParse(key[1..], out var functionKey) && functionKey is >= 1 and <= 24)
        {
            return (ushort)(0x70 + functionKey - 1);
        }

        return null;
    }

    private static void SendVirtualKey(ushort virtualKey, bool keyUp)
    {
        var input = new INPUT
        {
            type = INPUT_KEYBOARD
        };
        input.u.ki.wVk = virtualKey;
        input.u.ki.dwFlags = keyUp ? KEYEVENTF_KEYUP : 0;

        var sent = SendInput(1, [input], Marshal.SizeOf<INPUT>());
        if (sent != 1)
        {
            Console.WriteLine($"SendInput failed for virtual key: 0x{virtualKey:X2}");
        }
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint cInputs, INPUT[] pInputs, int cbSize);

    [StructLayout(LayoutKind.Sequential)]
    private struct INPUT
    {
        public uint type;
        public INPUTUNION u;
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct INPUTUNION
    {
        [FieldOffset(0)]
        public KEYBDINPUT ki;

        [FieldOffset(0)]
        public MOUSEINPUT mi;

        [FieldOffset(0)]
        public HARDWAREINPUT hi;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct KEYBDINPUT
    {
        public ushort wVk;
        public ushort wScan;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MOUSEINPUT
    {
        public int dx;
        public int dy;
        public uint mouseData;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct HARDWAREINPUT
    {
        public uint uMsg;
        public ushort wParamL;
        public ushort wParamH;
    }
}

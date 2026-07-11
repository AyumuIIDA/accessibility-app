using RyoikiTenkai.Actions;
using RyoikiTenkai.Core;
using RyoikiTenkai.GestureSources;
using RyoikiTenkai.Storage;
using RyoikiTenkai.Vision;

namespace RyoikiTenkai.App;

internal sealed class RyoikiTenkaiApp
{
    private readonly BindingStore _store;
    private readonly ActionExecutor _executor = new();
    private readonly GestureDispatcher _dispatcher;

    public RyoikiTenkaiApp(BindingStore store)
    {
        _store = store;
        _dispatcher = new GestureDispatcher(_store, _executor);
    }

    public void Run()
    {
        EnsureSeedData();

        while (true)
        {
            Console.WriteLine();
            Console.WriteLine("RyoikiTenkai minimal vertical slice");
            Console.WriteLine("1. List gesture bindings");
            Console.WriteLine("2. Register app launch binding");
            Console.WriteLine("3. Register text input binding");
            Console.WriteLine("4. Register hotkey binding");
            Console.WriteLine("5. Trigger gesture");
            Console.WriteLine("6. Run fake vision gesture source");
            Console.WriteLine("7. Run camera gesture source");
            Console.WriteLine("8. Run MediaPipe Hands camera source");
            Console.WriteLine("9. Reset sample binding");
            Console.WriteLine("0. Exit");
            Console.Write("> ");

            switch (Console.ReadLine()?.Trim())
            {
                case "1":
                    ListBindings();
                    break;
                case "2":
                    RegisterLaunchBinding();
                    break;
                case "3":
                    RegisterTextBinding();
                    break;
                case "4":
                    RegisterHotkeyBinding();
                    break;
                case "5":
                    TriggerGesture();
                    break;
                case "6":
                    RunFakeVisionSource();
                    break;
                case "7":
                    RunCameraSource();
                    break;
                case "8":
                    RunMediaPipeHandsSource();
                    break;
                case "9":
                    ResetSampleBinding();
                    break;
                case "0":
                    return;
                default:
                    Console.WriteLine("Unknown command.");
                    break;
            }
        }
    }

    private void EnsureSeedData()
    {
        if (_store.Load().Count > 0)
        {
            return;
        }

        _store.Save([
            new GestureBinding(
                GestureId: "open_palm",
                DisplayName: "Open palm -> launch Notepad",
                Action: new ActionSpec(
                    Type: "app.launch",
                    Params: new Dictionary<string, string>
                    {
                        ["path"] = "notepad.exe"
                    }))
        ]);
    }

    private void ListBindings()
    {
        var bindings = _store.Load();
        if (bindings.Count == 0)
        {
            Console.WriteLine("No bindings registered.");
            return;
        }

        foreach (var binding in bindings)
        {
            Console.WriteLine($"{binding.GestureId}: {binding.DisplayName} => {binding.Action.Type}");
            foreach (var param in binding.Action.Params)
            {
                Console.WriteLine($"  {param.Key}: {param.Value}");
            }
        }
    }

    private void RegisterLaunchBinding()
    {
        Console.Write("Gesture id (example: fist): ");
        var gestureId = ReadRequired();

        Console.Write("Display name: ");
        var displayName = ReadRequired();

        Console.Write("Executable or file path (example: notepad.exe): ");
        var path = ReadRequired();

        SaveBinding(new GestureBinding(
            GestureId: gestureId,
            DisplayName: displayName,
            Action: new ActionSpec(
                Type: "app.launch",
                Params: new Dictionary<string, string> { ["path"] = path })));
    }

    private void RegisterTextBinding()
    {
        Console.Write("Gesture id (example: two_fingers): ");
        var gestureId = ReadRequired();

        Console.Write("Display name: ");
        var displayName = ReadRequired();

        Console.Write("Text to type into the active window: ");
        var text = ReadRequired();

        SaveBinding(new GestureBinding(
            GestureId: gestureId,
            DisplayName: displayName,
            Action: new ActionSpec(
                Type: "keyboard.typeText",
                Params: new Dictionary<string, string> { ["text"] = text })));
    }

    private void RegisterHotkeyBinding()
    {
        Console.Write("Gesture id (example: pinch): ");
        var gestureId = ReadRequired();

        Console.Write("Display name: ");
        var displayName = ReadRequired();

        Console.Write("Hotkey (examples: ctrl+s, alt+f4, win+r): ");
        var hotkey = ReadRequired();

        SaveBinding(new GestureBinding(
            GestureId: gestureId,
            DisplayName: displayName,
            Action: new ActionSpec(
                Type: "keyboard.hotkey",
                Params: new Dictionary<string, string> { ["hotkey"] = hotkey })));
    }

    private void SaveBinding(GestureBinding binding)
    {
        var bindings = _store.Load();
        bindings.RemoveAll(x => StringComparer.OrdinalIgnoreCase.Equals(x.GestureId, binding.GestureId));
        bindings.Add(binding);
        _store.Save(bindings);
        Console.WriteLine("Saved.");
    }

    private void TriggerGesture()
    {
        var source = new ConsoleGestureSource(ReadRequired);
        source.GestureDetected += _dispatcher.Dispatch;
        source.Start();
    }

    private void RunFakeVisionSource()
    {
        Console.Write("Gesture id to emit (example: open_palm): ");
        var gestureId = ReadRequired();

        Console.Write("How many times? ");
        var countText = ReadRequired();
        var count = int.TryParse(countText, out var parsedCount) ? Math.Max(1, parsedCount) : 1;

        Console.Write("Interval milliseconds: ");
        var intervalText = ReadRequired();
        var intervalMs = int.TryParse(intervalText, out var parsedInterval) ? Math.Max(1, parsedInterval) : 1000;

        var source = new FakeVisionGestureSource(gestureId, count, TimeSpan.FromMilliseconds(intervalMs));
        source.GestureDetected += _dispatcher.Dispatch;
        source.Start();
    }

    private void RunCameraSource()
    {
        Console.Write("Gesture id to emit on camera-model detection (example: open_palm): ");
        var gestureId = ReadRequired();

        Console.Write("How many frames? ");
        var countText = ReadRequired();
        var frameCount = int.TryParse(countText, out var parsedCount) ? Math.Max(1, parsedCount) : 10;

        Console.Write("Interval milliseconds: ");
        var intervalText = ReadRequired();
        var intervalMs = int.TryParse(intervalText, out var parsedInterval) ? Math.Max(1, parsedInterval) : 500;

        var source = new CameraGestureSource(
            model: new FrameBrightnessGestureModel(gestureId),
            frameCount: frameCount,
            interval: TimeSpan.FromMilliseconds(intervalMs));

        source.GestureDetected += _dispatcher.Dispatch;
        source.Start();
    }

    private void RunMediaPipeHandsSource()
    {
        Console.Write("How many frames? ");
        var countText = ReadRequired();
        var frameCount = int.TryParse(countText, out var parsedCount) ? Math.Max(1, parsedCount) : 10;

        Console.Write("Interval milliseconds: ");
        var intervalText = ReadRequired();
        var intervalMs = int.TryParse(intervalText, out var parsedInterval) ? Math.Max(1, parsedInterval) : 500;

        var modelDirectory = Path.Combine(AppContext.BaseDirectory, "models");
        var handModel = new MediaPipeHandsModel(new MediaPipeHandsModelOptions(
            PalmDetectorPath: Path.Combine(modelDirectory, "palm_detection.onnx"),
            HandLandmarkPath: Path.Combine(modelDirectory, "hand_landmark.onnx")));
        var gestureModel = new MediaPipeLandmarkGestureModel(handModel);

        var source = new CameraGestureSource(
            model: gestureModel,
            frameCount: frameCount,
            interval: TimeSpan.FromMilliseconds(intervalMs));

        source.GestureDetected += _dispatcher.Dispatch;

        try
        {
            source.Start();
        }
        catch (Exception ex)
        {
            Console.WriteLine(ex.Message);
        }
    }

    private void ResetSampleBinding()
    {
        _store.Save([]);
        EnsureSeedData();
        Console.WriteLine("Reset to sample binding: open_palm -> notepad.exe");
    }

    private static string ReadRequired()
    {
        while (true)
        {
            var value = Console.ReadLine()?.Trim();
            if (!string.IsNullOrWhiteSpace(value))
            {
                return value;
            }

            Console.Write("> ");
        }
    }
}

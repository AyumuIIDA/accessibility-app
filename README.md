# RyoikiTenkai

RyoikiTenkai is a Windows/.NET prototype that maps hand gestures to local PC actions.

Current vertical slice:

```text
camera
  -> MediaPipe-style palm detector ONNX
  -> MediaPipe-style hand landmark ONNX
  -> gesture classifier
  -> WPF UI overlay
  -> Windows action execution
```

## Requirements

- Windows 11
- .NET SDK 10
- A webcam
- PowerShell
- Internet access for first-time NuGet/model download

Check your .NET install:

```powershell
dotnet --info
```

The projects currently target:

```text
net10.0-windows10.0.26100.0
```

## Repository Layout

```text
doc/
  Design notes and product planning.

src/RyoikiTenkai/
  Core console app and shared runtime code.
  Actions/       Windows action execution.
  App/           Console menu app.
  Core/          Gesture/action records and dispatcher.
  GestureSources/
  Storage/       bindings.json persistence.
  Vision/        Camera, ONNX, MediaPipe-style pipeline.
  models/        ONNX model files.

src/RyoikiTenkai.Wpf/
  WPF demo UI.
```

## First-Time Setup

From the repository root:

```powershell
cd C:\Users\hyena\accessibility-app
```

Restore NuGet packages:

```powershell
dotnet restore src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj
```

Build:

```powershell
dotnet build src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj
```

Run WPF UI:

```powershell
dotnet run --project src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj
```

Run console app:

```powershell
dotnet run --project src/RyoikiTenkai/RyoikiTenkai.csproj
```

## Model Files

Expected model paths:

```text
src/RyoikiTenkai/models/palm_detection.onnx
src/RyoikiTenkai/models/hand_landmark.onnx
```

Download models with PowerShell:

```powershell
New-Item -ItemType Directory -Force -Path src/RyoikiTenkai/models

Invoke-WebRequest `
  -Uri https://huggingface.co/opencv/palm_detection_mediapipe/resolve/main/palm_detection_mediapipe_2023feb.onnx `
  -OutFile src/RyoikiTenkai/models/palm_detection.onnx

Invoke-WebRequest `
  -Uri https://huggingface.co/opencv/handpose_estimation_mediapipe/resolve/main/handpose_estimation_mediapipe_2023feb.onnx `
  -OutFile src/RyoikiTenkai/models/hand_landmark.onnx
```

Verify model files:

```powershell
Get-ChildItem src/RyoikiTenkai/models
```

Expected names:

```text
palm_detection.onnx
hand_landmark.onnx
```

The WPF project copies these files to its output directory at build time:

```text
src/RyoikiTenkai.Wpf/bin/Debug/net10.0-windows10.0.26100.0/models/
```

## Common Commands

Restore:

```powershell
dotnet restore src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj
```

Build WPF:

```powershell
dotnet build src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj
```

Build without restore:

```powershell
dotnet build src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj --no-restore
```

Run WPF:

```powershell
dotnet run --project src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj
```

Run console:

```powershell
dotnet run --project src/RyoikiTenkai/RyoikiTenkai.csproj
```

Clean build outputs:

```powershell
dotnet clean src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj
```

Find running app processes:

```powershell
Get-Process | Where-Object { $_.ProcessName -like 'RyoikiTenkai*' }
```

Stop a stuck app process:

```powershell
Get-Process | Where-Object { $_.ProcessName -like 'RyoikiTenkai*' } | Stop-Process
```

## Logs And Runtime Files

WPF runtime log:

```text
src/RyoikiTenkai.Wpf/bin/Debug/net10.0-windows10.0.26100.0/ryoikitenkai.log
```

View latest log:

```powershell
Get-Content src/RyoikiTenkai.Wpf/bin/Debug/net10.0-windows10.0.26100.0/ryoikitenkai.log -Tail 100
```

Gesture/action bindings are stored next to the running app:

```text
src/RyoikiTenkai.Wpf/bin/Debug/net10.0-windows10.0.26100.0/bindings.json
src/RyoikiTenkai/bin/Debug/net10.0-windows10.0.26100.0/bindings.json
```

Delete WPF bindings and let the app recreate the default sample:

```powershell
Remove-Item src/RyoikiTenkai.Wpf/bin/Debug/net10.0-windows10.0.26100.0/bindings.json
```

## Environment Variables

No application-specific environment variable is required for the current CPU ONNX Runtime path.

Useful development variables:

```powershell
$env:DOTNET_CLI_TELEMETRY_OPTOUT = "1"
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = "1"
```

Optional local NuGet package cache location:

```powershell
$env:NUGET_PACKAGES = "$HOME\.nuget\packages"
```

Optional model directory helper for scripts:

```powershell
$env:RYOIKI_MODELS_DIR = "C:\Users\hyena\accessibility-app\src\RyoikiTenkai\models"
```

Download using `RYOIKI_MODELS_DIR`:

```powershell
New-Item -ItemType Directory -Force -Path $env:RYOIKI_MODELS_DIR

Invoke-WebRequest `
  -Uri https://huggingface.co/opencv/palm_detection_mediapipe/resolve/main/palm_detection_mediapipe_2023feb.onnx `
  -OutFile "$env:RYOIKI_MODELS_DIR\palm_detection.onnx"

Invoke-WebRequest `
  -Uri https://huggingface.co/opencv/handpose_estimation_mediapipe/resolve/main/handpose_estimation_mediapipe_2023feb.onnx `
  -OutFile "$env:RYOIKI_MODELS_DIR\hand_landmark.onnx"
```

Make environment variables persistent for the current Windows user:

```powershell
[Environment]::SetEnvironmentVariable("DOTNET_CLI_TELEMETRY_OPTOUT", "1", "User")
[Environment]::SetEnvironmentVariable("DOTNET_SKIP_FIRST_TIME_EXPERIENCE", "1", "User")
[Environment]::SetEnvironmentVariable("RYOIKI_MODELS_DIR", "C:\Users\hyena\accessibility-app\src\RyoikiTenkai\models", "User")
```

Open a new PowerShell session after setting persistent environment variables.

## Current Runtime Notes

- Inference uses `Microsoft.ML.OnnxRuntime`.
- The current execution provider is CPU.
- NPU/QNN execution is not wired yet.
- The WPF camera path uses Windows `MediaFrameReader`.
- Display frames and model frames are separated:
  - display: high-resolution camera frame
  - model: resized frame for MediaPipe-style ONNX pipeline

## Troubleshooting

If camera start fails, inspect:

```powershell
Get-Content src/RyoikiTenkai.Wpf/bin/Debug/net10.0-windows10.0.26100.0/ryoikitenkai.log -Tail 200
```

If build fails because `RyoikiTenkai.exe` is locked, stop the running process:

```powershell
Get-Process | Where-Object { $_.ProcessName -like 'RyoikiTenkai*' } | Stop-Process
dotnet build src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj
```

If models are missing, rerun the model download commands in the `Model Files` section.

If NuGet restore fails, verify internet access and run:

```powershell
dotnet nuget list source
dotnet restore src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj
```

## External Model Sources

- Palm detector: https://huggingface.co/opencv/palm_detection_mediapipe
- Hand pose model: https://huggingface.co/opencv/handpose_estimation_mediapipe
- MediaPipe Hands docs: https://mediapipe.readthedocs.io/en/latest/solutions/hands.html

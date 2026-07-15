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

src/RyoikiTenkai.Native/
  Experimental C++ native vision runtime DLL skeleton.
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
src/RyoikiTenkai.Wpf/bin/arm64/Debug/net10.0-windows10.0.26100.0/models/
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

## Native Runtime

The repository includes an experimental C++ native runtime boundary:

```text
src/RyoikiTenkai.Native/
  CMakeLists.txt
  vcpkg.json
  include/ryoiki_native.h
  src/Buffers/
  src/Geometry/
  src/Pipeline/
  src/camera_capture.cpp
  src/ryoiki_native.cpp
```

This skeleton exposes a C ABI for lifecycle and polling:

```text
ryoiki_get_abi_version
ryoiki_create
ryoiki_start
ryoiki_stop
ryoiki_resize
ryoiki_destroy
ryoiki_get_latest_metrics
ryoiki_get_latest_palm
ryoiki_get_latest_hand
ryoiki_get_last_error
```

The ABI, ownership, coordinate, timestamp, and metrics contracts are documented in
[`doc/native-vision-runtime.md`](doc/native-vision-runtime.md). The WPF host checks the
ABI version before creating a runtime and validates the version and size of every
polled structure.

The native runtime currently captures a user-facing camera through Media Foundation,
converts frames to top-down BGRA32, publishes pooled native-owned frames to separate
latest-display and capacity-one perception paths, and renders into the child HWND. The
perception worker uses OpenCV to letterbox the palm input and pack an RGB NHWC float
tensor. CPU model runners execute both ONNX models through ONNX Runtime. The
MediaPipe-like graph decodes palm anchors, creates a rotated hand ROI, projects 21
landmarks back to source coordinates, and reuses a landmark-derived ROI while tracking
confidence remains above threshold. Palm bbox/keypoints and hand landmarks are exposed
through ABI polling and rendered by the native HWND overlay. Direct2D/Direct3D
rendering, DirectML, and QNN remain future phases.

Required native build tools:

```text
Visual Studio 2022 or later
Desktop development with C++
MSVC ARM64 toolchain
Windows 11 SDK 10.0.26100+
CMake
Ninja
vcpkg (the Visual Studio bundled installation is supported)
.NET SDK restore of Microsoft.ML.OnnxRuntime 1.27.0
```

### ARM64 Developer Shell

For native C++ builds, use a Visual Studio developer shell configured for ARM64. A regular PowerShell may not have `cl.exe`, Windows SDK include paths, or MSVC library paths configured.

The most explicit way is:

```cmd
cmd /k "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" arm64
```

`vcvarsall.bat` is the Visual Studio/MSVC environment setup script. It configures:

```text
PATH
INCLUDE
LIB
LIBPATH
VCToolsInstallDir
WindowsSdkDir
WindowsSDKVersion
VSCMD_ARG_HOST_ARCH
VSCMD_ARG_TGT_ARCH
```

In the opened shell, verify that ARM64 is selected:

```cmd
where cl
echo %VSCMD_ARG_HOST_ARCH%
echo %VSCMD_ARG_TGT_ARCH%
```

Expected:

```text
...\Hostarm64\arm64\cl.exe
arm64
arm64
```

If you see `Hostx86\x86\cl.exe` or `x86`, close that shell and reopen an ARM64 developer shell. The native DLL loaded by the WPF app must match the process architecture.

### Build Native DLL

From the ARM64 developer shell:

```powershell
cd /d C:\Users\hyena\accessibility-app

dotnet restore src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj
cmake -S src/RyoikiTenkai.Native -B build/RyoikiTenkai.Native.OpenCv -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=arm64-windows-static-md
cmake --build build/RyoikiTenkai.Native.OpenCv
ctest --test-dir build/RyoikiTenkai.Native.OpenCv --output-on-failure
```

CMake resolves ONNX Runtime headers and the architecture-specific import library from
the restored NuGet cache. Override `ONNXRUNTIME_ROOT` only when using a nonstandard
package location; it must point to the `microsoft.ml.onnxruntime/1.27.0` package root.

The next WPF build automatically copies an existing native DLL into `$(OutDir)`:

```powershell
dotnet build src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj --no-restore
```

To configure, build, and copy native code through one MSBuild invocation, run this from
the ARM64 developer shell:

```powershell
dotnet build src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj `
  --no-restore -p:BuildNativeRuntime=true
```

This target fails before CMake runs when the Visual Studio target architecture is not
ARM64 or the vcpkg toolchain cannot be found. This prevents a regular PowerShell or a
stale x86 developer environment from silently producing an incompatible native DLL.

If the build directory was previously configured for x86 or another architecture, delete it before reconfiguring:

```cmd
rmdir /s /q build\RyoikiTenkai.Native.OpenCv
```

Run WPF and enable the `Native runtime` checkbox before pressing Start.

If the DLL is not present, WPF logs that the native runtime is unavailable and falls back to the managed C# camera pipeline.

If loading fails with `0x800711C7`, Windows Application Control rejected the unsigned
development DLL. Use the organization-approved development signing process or an
approved development environment; do not disable the policy as a build workaround.

## Logs And Runtime Files

WPF runtime log:

```text
src/RyoikiTenkai.Wpf/bin/arm64/Debug/net10.0-windows10.0.26100.0/ryoikitenkai.log
```

View latest log:

```powershell
Get-Content src/RyoikiTenkai.Wpf/bin/arm64/Debug/net10.0-windows10.0.26100.0/ryoikitenkai.log -Tail 100
```

Gesture/action bindings are stored next to the running app:

```text
src/RyoikiTenkai.Wpf/bin/arm64/Debug/net10.0-windows10.0.26100.0/bindings.json
src/RyoikiTenkai/bin/arm64/Debug/net10.0-windows10.0.26100.0/bindings.json
```

Delete WPF bindings and let the app recreate the default sample:

```powershell
Remove-Item src/RyoikiTenkai.Wpf/bin/arm64/Debug/net10.0-windows10.0.26100.0/bindings.json
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
Get-Content src/RyoikiTenkai.Wpf/bin/arm64/Debug/net10.0-windows10.0.26100.0/ryoikitenkai.log -Tail 200
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

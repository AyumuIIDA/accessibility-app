# RyoikiTenkai

RyoikiTenkai is a Windows/.NET prototype that maps hand gestures to local PC actions.

Current vertical slice:

```text
camera
  -> MediaPipe-style palm detector ONNX
  -> MediaPipe-style hand landmark ONNX
  -> static + time-window gesture recognizers
  -> WPF UI overlay
  -> Windows action execution
```

## Requirements

- Windows 11
- .NET SDK 10
- Visual Studio 2026 with Desktop development with C++
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
  C++ camera, perception, geometry, and DirectX rendering runtime DLL.
```

## First-Time Setup

Clone the repository, then run all commands below from its root. For example:

```powershell
Set-Location accessibility-app
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
normalizes signed stride into tightly packed top-down BGRA32, retains camera rotation
as frame metadata, and sends pooled native-owned frames through a capacity-one
perception path. Perception completion
publishes one synchronized render
packet containing the source frame and its palm/hand metadata. The child HWND composes
that packet on a dedicated render thread using a D3D11 device, DXGI flip-model swap
chain, and Direct2D device context. CPU BGRA pixels are uploaded once per accepted
frame; orientation, front-camera mirroring, letterbox fitting, and overlays are drawn
into the same back buffer before one synchronized present. No GDI or OpenCV display
path writes to the child HWND. The
perception worker uses OpenCV to letterbox the palm input and pack an RGB NHWC float
tensor. Camera orientation is fused into palm and hand tensor sampling, so capture
does not create a rotated full-resolution intermediate. CPU model runners execute
both ONNX models through ONNX Runtime. The
MediaPipe-like graph decodes palm anchors, creates a rotated hand ROI, projects 21
landmarks back to source coordinates, and reuses a landmark-derived ROI while tracking
confidence remains above threshold. Palm bbox/keypoints and hand landmarks are exposed
through ABI polling and rendered by the native HWND overlay. Direct2D/Direct3D
rendering is the sole native display path. The native model runners require ONNX
Runtime QNN/HTP by default; when QNN is unavailable or rejects the model, native mode
reports the blocking reason instead of silently falling back to CPU inference.
Camera frame memory placement options and the criteria for moving capture from CPU
buffers to DXGI surfaces are documented in
[`doc/native-frame-memory-roadmap.md`](doc/native-frame-memory-roadmap.md).

### Install Visual Studio 2026 Components

Open Visual Studio Installer, select **Visual Studio 2026**, and install the
**Desktop development with C++** workload. Ensure these individual components are
included:

```text
MSVC C++ ARM64/ARM64EC build tools
Windows 11 SDK 10.0.26100.0 or later
C++ CMake tools for Windows
vcpkg package manager
```

The verified environment uses Visual Studio 2026 (version 18), the MSVC 14.5x
toolset, CMake, Ninja, and the vcpkg installation bundled with Visual Studio. The
repository's `vcpkg.json` pins the OpenCV dependency baseline. NuGet restore supplies
the ONNX Runtime 1.27.0 native headers, import library, and DLL.

Also install the .NET 10 SDK, then verify the command-line tools:

```powershell
dotnet --version
cmake --version
ninja --version
```

### ARM64 Developer Shell

For native C++ builds, use a Visual Studio 2026 developer environment configured for
ARM64. A regular PowerShell does not normally have `cl.exe`, Windows SDK include
paths, or MSVC library paths configured.

The following commands locate any installed Visual Studio 2026 edition and configure
the current PowerShell process without assuming Community, Professional, or Enterprise:

```powershell
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * `
  -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 `
  -property installationPath

if (-not $vsPath) {
  throw "Visual Studio 2026 with the ARM64 C++ tools was not found."
}

Import-Module (Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
if ($env:VSCMD_ARG_TGT_ARCH -ne "arm64") {
  Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation `
    -Arch arm64 -HostArch arm64
}

$env:VCPKG_ROOT = Join-Path $vsPath "VC\vcpkg"
```

Alternatively, open **ARM64 Native Tools PowerShell for VS 2026** from the Start
menu and set `VCPKG_ROOT` to the bundled `VC\vcpkg` directory if needed.

The developer environment configures:

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

Verify that ARM64 is selected and the required tools are visible:

```powershell
Get-Command cl, cmake, ninja
$env:VSCMD_ARG_HOST_ARCH
$env:VSCMD_ARG_TGT_ARCH
Test-Path (Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake")
```

Expected:

```text
...\Hostarm64\arm64\cl.exe
arm64
arm64
True
```

If you see `Hostx86\x86\cl.exe` or `x86`, close that shell and reopen an ARM64 developer shell. The native DLL loaded by the WPF app must match the process architecture.

### Build Native DLL

From the repository root in that ARM64 Developer PowerShell:

```powershell
dotnet restore src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj
cmake -S src/RyoikiTenkai.Native -B build/RyoikiTenkai.Native.OpenCv -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=arm64-windows-static-md
cmake --build build/RyoikiTenkai.Native.OpenCv
ctest --test-dir build/RyoikiTenkai.Native.OpenCv --output-on-failure
```

The expected native outputs are:

```text
build/RyoikiTenkai.Native.OpenCv/RyoikiTenkai.Native.dll
build/RyoikiTenkai.Native.OpenCv/RyoikiTenkai.VisionCore.Tests.exe
build/RyoikiTenkai.Native.OpenCv/RyoikiTenkai.HandPerception.Tests.exe
```

CMake resolves ONNX Runtime headers and the architecture-specific import library from
the restored NuGet cache. Override `ONNXRUNTIME_ROOT` only when using a nonstandard
package location; it must point to the `microsoft.ml.onnxruntime/1.27.0` package root.
Set `QNN_RUNTIME_DIR` when configuring CMake if you have a Qualcomm/ONNX Runtime QNN
runtime directory containing `QnnHtp.dll` and related provider DLLs:

```powershell
cmake -S src/RyoikiTenkai.Native -B build/RyoikiTenkai.Native.OpenCv -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=arm64-windows-static-md `
  -DQNN_RUNTIME_DIR="C:\Path\To\QnnRuntime"
```

The next WPF build automatically copies existing native DLLs from the CMake output
into `$(OutDir)`, including `RyoikiTenkai.Native.dll`, `onnxruntime.dll`, and any QNN
DLLs copied by CMake:

```powershell
dotnet build src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj --no-restore
```

On machines with Smart App Control or WDAC enabled, the native development DLL must
be signed or Windows can block it with `0x800711C7` before the NPU runtime starts.
The WPF project signs the copied native DLL by default when `signtool.exe` and a
certificate named `RyoikiTenkai Local Dev Code Signing` are available. Disable this
only in an environment that can load unsigned local native DLLs:

```powershell
dotnet build src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj `
  --no-restore -p:BuildNativeRuntime=true -p:SignNativeRuntime=false
```

To configure, build, and copy native code through one MSBuild invocation, run this from
the ARM64 developer shell:

```powershell
dotnet build src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj `
  --no-restore -p:BuildNativeRuntime=true -p:QnnRuntimeDir="C:\Path\To\QnnRuntime"
```

This target fails before CMake runs when the Visual Studio target architecture is not
ARM64 or the vcpkg toolchain cannot be found. This prevents a regular PowerShell or a
stale x86 developer environment from silently producing an incompatible native DLL.

If the build directory was previously configured for x86, x64, or another vcpkg
triplet, remove only that generated directory before reconfiguring:

```powershell
$nativeBuild = Resolve-Path build/RyoikiTenkai.Native.OpenCv -ErrorAction SilentlyContinue
if ($nativeBuild -and $nativeBuild.Path.StartsWith((Resolve-Path .).Path)) {
  Remove-Item -LiteralPath $nativeBuild.Path -Recurse -Force
}
```

Run WPF and press Start. The `Native runtime` checkbox is enabled by default.

If the DLL is not present or is blocked, WPF logs that the native runtime is
unavailable and does not silently start the managed CPU inference path.

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

Custom recorded gesture templates are stored next to the WPF app:

```text
src/RyoikiTenkai.Wpf/bin/Debug/net10.0-windows10.0.26100.0/gestures.json
```

In the WPF UI, start the camera, enter a gesture name, and use `Record Example` three times. Each recording captures about 1.5 seconds of one-hand landmarks, normalizes the motion, and enables the gesture for action binding after three examples.

The WPF `Debug Lab` tab shows how gesture classification is being decided. It visualizes the normalized candidate motion path, overlays the best matching recorded templates, lists per-template score/confidence values, and shows raw normalized vector rows. The ONNX models do not expose latent embeddings in this app; the debug view shows the actual vectors and scores used by the custom gesture classifier.

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

Optional model directory helper for scripts, set from the repository root:

```powershell
$env:RYOIKI_MODELS_DIR = Join-Path (Get-Location) "src\RyoikiTenkai\models"
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
[Environment]::SetEnvironmentVariable("RYOIKI_MODELS_DIR", $env:RYOIKI_MODELS_DIR, "User")
```

Open a new PowerShell session after setting persistent environment variables.

## Current Runtime Notes

- Managed fallback inference uses `Microsoft.ML.OnnxRuntime`; native inference uses
  Windows ML ONNX Runtime with QNN/HTP registration.
- The managed WPF camera path uses CPU ONNX Runtime sessions.
- The native runtime requires QNN/HTP by default. Set
  `RYOIKI_EXECUTION_PROVIDER=cpu` only when intentionally debugging the CPU path.
- The WPF native status line and log show selected providers, fallback/blocking
  reasons, camera size/subtype, graph timing, frame age, frame/tensor byte counts,
  dropped frames, and process working-set memory.
- If QNN/HTP rejects a model, native mode fails closed instead of silently running
  neural inference on CPU.
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

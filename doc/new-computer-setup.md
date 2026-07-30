# New Computer Setup

This document is the command checklist for getting the current working version of
RyoikiTenkai running on a new Windows machine.

The expected result is:

```text
repository cloned
  -> .NET packages restored
  -> ONNX hand models present
  -> WPF project builds
  -> optional native runtime builds
  -> WPF viewer runs
```

## 1. Install Required Tools

Install these first:

- Windows 11
- .NET SDK 10
- Visual Studio 2026 with the **Desktop development with C++** workload
- A webcam
- PowerShell

In Visual Studio Installer, make sure these individual C++ components are installed:

```text
MSVC C++ ARM64/ARM64EC build tools
Windows 11 SDK 10.0.26100.0 or later
C++ CMake tools for Windows
vcpkg package manager
```

Open PowerShell and verify the basic tools:

```powershell
dotnet --info
dotnet --version
cmake --version
ninja --version
```

The projects currently target:

```text
net10.0-windows10.0.26100.0
```

## 2. Clone And Enter The Repository

Clone the repository, then run every later command from the repository root:

```powershell
git clone <repository-url> accessibility-app
Set-Location accessibility-app
```

If the repository is already present, just enter it:

```powershell
Set-Location C:\Users\<you>\Documents\accessibility-app
```

## 3. Restore .NET Packages

```powershell
dotnet restore src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj
```

## 4. Make Sure Model Files Exist

Expected files:

```text
src/RyoikiTenkai/models/palm_detection.onnx
src/RyoikiTenkai/models/hand_landmark.onnx
```

Check for them:

```powershell
Get-ChildItem src/RyoikiTenkai/models
```

If either file is missing, download both:

```powershell
New-Item -ItemType Directory -Force -Path src/RyoikiTenkai/models

Invoke-WebRequest `
  -Uri https://huggingface.co/opencv/palm_detection_mediapipe/resolve/main/palm_detection_mediapipe_2023feb.onnx `
  -OutFile src/RyoikiTenkai/models/palm_detection.onnx

Invoke-WebRequest `
  -Uri https://huggingface.co/opencv/handpose_estimation_mediapipe/resolve/main/handpose_estimation_mediapipe_2023feb.onnx `
  -OutFile src/RyoikiTenkai/models/hand_landmark.onnx
```

Verify the downloaded files:

```powershell
Get-ChildItem src/RyoikiTenkai/models
```

## 5. Build The WPF App

This build does not require the native C++ runtime. If the native DLL is missing, the
WPF app logs that native mode is unavailable.

```powershell
dotnet build src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj
```

For later builds after restore:

```powershell
dotnet build src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj --no-restore
```

## 6. Run The WPF Viewer

```powershell
dotnet run --project src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj
```

In the app, press `Start` to start the camera. The `Native runtime` checkbox is
enabled by default, but the app only uses it when a compatible native DLL was built
and copied into the WPF output directory.

## 7. Optional: Build The Native Runtime

Use this when you need the C++ camera/perception/rendering path. These commands must
run from an ARM64 Visual Studio developer PowerShell, not a plain PowerShell.

From a normal PowerShell, this configures the current shell for ARM64 Visual Studio
tools without assuming the Visual Studio edition:

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

Verify the shell:

```powershell
Get-Command cl, cmake, ninja
$env:VSCMD_ARG_HOST_ARCH
$env:VSCMD_ARG_TGT_ARCH
Test-Path (Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake")
```

Expected architecture values:

```text
arm64
arm64
True
```

Build and test the native runtime:

```powershell
dotnet restore src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj

cmake -S src/RyoikiTenkai.Native -B build/RyoikiTenkai.Native.OpenCv -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=arm64-windows-static-md

cmake --build build/RyoikiTenkai.Native.OpenCv
ctest --test-dir build/RyoikiTenkai.Native.OpenCv --output-on-failure
```

Then build the WPF app so MSBuild copies the native DLLs into the WPF output
directory:

```powershell
dotnet build src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj --no-restore
```

Expected native output:

```text
build/RyoikiTenkai.Native.OpenCv/RyoikiTenkai.Native.dll
```

If you have a Qualcomm QNN runtime directory, pass it at configure time:

```powershell
cmake -S src/RyoikiTenkai.Native -B build/RyoikiTenkai.Native.OpenCv -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=arm64-windows-static-md `
  -DQNN_RUNTIME_DIR="C:\Path\To\QnnRuntime"
```

Or build native code through MSBuild:

```powershell
dotnet build src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj `
  --no-restore -p:BuildNativeRuntime=true -p:QnnRuntimeDir="C:\Path\To\QnnRuntime"
```

## 8. Run Tests

Run managed tests:

```powershell
dotnet test tests/RyoikiTenkai.Tests/RyoikiTenkai.Tests.csproj
```

Run native tests after building the native runtime:

```powershell
ctest --test-dir build/RyoikiTenkai.Native.OpenCv --output-on-failure
```

## 9. Useful Troubleshooting Commands

View the WPF runtime log:

```powershell
Get-Content src/RyoikiTenkai.Wpf/bin/arm64/Debug/net10.0-windows10.0.26100.0/ryoikitenkai.log -Tail 200
```

Stop a running app that is locking build outputs:

```powershell
Get-Process | Where-Object { $_.ProcessName -like 'RyoikiTenkai*' } | Stop-Process
```

Clean the WPF build output:

```powershell
dotnet clean src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj
```

If native CMake was configured with the wrong architecture or vcpkg triplet, remove
only the generated native build directory and configure it again:

```powershell
$nativeBuild = Resolve-Path build/RyoikiTenkai.Native.OpenCv -ErrorAction SilentlyContinue
if ($nativeBuild -and $nativeBuild.Path.StartsWith((Resolve-Path .).Path)) {
  Remove-Item -LiteralPath $nativeBuild.Path -Recurse -Force
}
```

If the WPF log shows native loading failed with `0x800711C7`, Windows Application
Control rejected an unsigned development DLL. Use the approved local development
signing process, or build without native mode until signing is available.

## 10. Quick Command Path

For a managed-only first run:

```powershell
Set-Location C:\Users\<you>\Documents\accessibility-app
dotnet restore src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj
Get-ChildItem src/RyoikiTenkai/models
dotnet build src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj --no-restore
dotnet run --project src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj
```

For native runtime work, run the ARM64 developer shell setup in section 7 before
building native code.

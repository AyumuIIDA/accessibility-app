# Agent Engineering Guide

This repository is a Windows local-AI accessibility prototype. Multiple agents may work on the codebase concurrently. Follow this guide before making architectural or code changes.

## Project Goal

The current near-term goal is a real-time hand landmark recognition viewer:

```text
camera
  -> real-time hand perception
  -> stable 21 landmark tracking
  -> registered landmark/gesture visualization
  -> later: gesture -> PC manipulation
```

The existing gesture-to-PC-action path is a vertical slice. Do not prioritize complex action automation until hand perception quality, latency, and visualization are stable.

## Architectural Principles

### Keep the heavy vision path out of C# where practical

The target architecture is hybrid:

```text
C# / WPF:
  UI shell
  settings
  logs
  metrics display
  gesture/action registration
  app workflow

C++ native runtime:
  camera capture
  image geometry
  tensor creation
  model inference
  low-level rendering surface
  buffer ownership
```

C# may orchestrate and display metadata, but it should not permanently own high-frequency image processing, tensor packing, or native rendering paths.

### Treat MediaPipe as a perception subgraph

Do not rewrite MediaPipe concepts arbitrarily. The hand perception engine should emulate the MediaPipe Hands graph as closely as practical.

Use MediaPipe graph files as the source of truth for ROI/tracking behavior:

```text
hand_landmark_tracking_cpu.pbtxt
palm_detection_detection_to_roi.pbtxt
hand_landmark_landmarks_to_roi.pbtxt
```

Outer pipeline responsibility:

```text
frame flow
metadata flow
queue policy
metrics
hardware placement
UI/action integration
```

Inner MediaPipe-like graph responsibility:

```text
palm detection
detection -> ROI
hand landmark inference
landmarks -> next ROI
tracking fallback
confidence handling
```

Prefer this boundary:

```text
CameraStage
  -> HandPerceptionStage
       MediaPipe-like graph inside
  -> LandmarkPatternStage
  -> Overlay/Action stages
```

Avoid splitting the MediaPipe graph into external app-level stages in a way that breaks ROI loopback consistency.

### Frame + Metadata, not image mutation everywhere

Use packets that carry a frame reference plus metadata:

```text
VisionPacket
  FrameBuffer
  VisionMetadata
  PipelineMetrics
```

Visualization, gesture detection, and action decision should consume metadata, not reprocess images.

### Prefer shallow queues and latest-frame semantics

Real-time interactivity matters more than processing every frame.

Required policy:

```text
Display path: latest frame only
Perception path: capacity 1 / drop stale frames
Metadata path: latest metadata only
Action path: ordered after debounce/cooldown
```

Do not let old frames accumulate.

### Hardware-aware runtime is isolated

Device placement belongs in runtime code, not UI or graph logic.

```text
CPU:
  control flow, metadata, tracking policy, light postprocess

C++ / CPU SIMD / OpenCV:
  resize, crop, warpAffine, NMS, tensor packing

GPU:
  rendering, possible DirectML inference

NPU:
  fixed-shape neural inference via QNN/HTP when model compatibility is proven
```

The MediaPipe-like graph should call model runners through interfaces and should not know whether the model is running on CPU, DirectML, QNN GPU, or QNN HTP/NPU.

## Code Organization Direction

Prefer this layout as the project evolves:

```text
src/RyoikiTenkai/
  Vision/
    Pipeline/
    Buffers/
    HandPerception/
    HandPerception/MediaPipeGraph/
    Geometry/
    Runtime/
    Patterns/

src/RyoikiTenkai.Wpf/
  UI shell, host controls, metrics, logs

src/RyoikiTenkai.Native/
  C++ native runtime DLL
```

Do not create broad abstractions before there is a real second implementation. The first abstractions that are justified are:

```text
IHandPerceptionEngine
IGeometryProcessor
IHandModelRunner
INativeVisionRuntime
```

Avoid for now:

```text
generic plugin systems
arbitrary cloud runtime abstractions
multi-camera frameworks
general model registries
large dependency injection frameworks
```

## C# Coding Standards

Use modern C# and .NET conventions.

Required:

- Nullable reference types remain enabled.
- Async methods use the `Async` suffix.
- Long-running operations accept and honor `CancellationToken`.
- Avoid `.Result`, `.Wait()`, and blocking waits on async work.
- Avoid `async void` except WPF event handlers.
- Dispose resources with `using` / `await using`.
- Prefer immutable records or init-only properties for data contracts.
- Catch only exceptions that can be handled or enriched with useful context.
- Keep LINQ readable; split complex queries into named methods.
- Avoid repeated enumeration of expensive sequences.
- Use `PascalCase` for types, methods, and properties.
- Use `camelCase` for locals and parameters.
- Use `_camelCase` for private fields.
- Prefix interfaces with `I`.

Performance-sensitive C# rules:

- No per-frame avoidable allocation in hot paths.
- Do not use LINQ in per-frame pixel/tensor loops.
- Do not run inference or heavy image processing on the WPF UI thread.
- Do not create WPF `Line` / `Ellipse` objects per frame in final high-performance paths; use reusable visuals, `DrawingVisual`, Direct2D, or native rendering.
- C# may poll lightweight metadata from native code; avoid passing large image buffers to C# unless explicitly justified.

## C++ Coding Standards

Use modern C++ for native runtime work.

Required:

- Prefer RAII for every resource.
- Do not use owning raw pointers in normal application code.
- Use `std::unique_ptr<T>` for exclusive ownership.
- Use `std::shared_ptr<T>` only when ownership is genuinely shared.
- Use raw pointers or references only for non-owning access.
- Prefer the Rule of Zero.
- Initialize every variable.
- Use `const` for read-only parameters, locals, and member functions.
- Use references for required objects; use pointers only when null is meaningful.
- Prefer value semantics and standard-library containers.
- Avoid C-style casts.
- Use `override` for virtual overrides.
- Prefer composition over inheritance.
- Do not throw from destructors.
- Use RAII locks such as `std::lock_guard` or `std::scoped_lock`.
- Avoid detached threads without a clear lifetime strategy.

Naming:

- Types: `PascalCase`
- Functions: `camelCase`
- Variables: `camelCase`
- Private fields: trailing underscore, e.g. `frameBuffer_`
- Constants: `kPascalCase` or `kCamelCase`
- Macros: `UPPER_SNAKE_CASE`, and avoid macros unless necessary for platform interop.

Native boundary rules:

- Export a C ABI, not C++ classes.
- Use `extern "C"` and `__declspec(dllexport)`.
- Do not let C++ exceptions cross the ABI boundary.
- Return explicit status codes or boolean success values.
- C++ owns C++-allocated buffers.
- C# must not hold native pointers beyond the documented call lifetime.
- Prefer copying small metadata structs to C#.
- Do not pass image/tensor buffers to C# in the target high-performance path.

## Native Runtime ABI Guidelines

The native runtime should expose lifecycle and polling APIs first.

Preferred shape:

```cpp
extern "C" __declspec(dllexport)
RyoikiHandle* ryoiki_create(void* parentHwnd);

extern "C" __declspec(dllexport)
bool ryoiki_start(RyoikiHandle* handle);

extern "C" __declspec(dllexport)
void ryoiki_stop(RyoikiHandle* handle);

extern "C" __declspec(dllexport)
void ryoiki_destroy(RyoikiHandle* handle);

extern "C" __declspec(dllexport)
bool ryoiki_get_latest_metrics(RyoikiHandle* handle, RyoikiMetrics* outMetrics);

extern "C" __declspec(dllexport)
bool ryoiki_get_latest_hand(RyoikiHandle* handle, RyoikiHandResult* outResult);
```

Start with polling APIs. Add callbacks only when there is a clear need and delegate lifetime/threading are documented.

## Memory Ownership Rules

Every buffer must have a clear owner and memory location.

Use concepts like:

```text
MemoryLocation:
  Cpu
  Gpu
  Npu
  Shared
```

Initial implementation may use CPU memory only, but APIs should not assume C# owns the frame buffer forever.

Rules:

- Native image buffers are owned and freed by native code.
- Native tensor buffers are owned and freed by native code.
- C# receives copied metadata values by polling.
- Use buffer pools for high-frequency frame/tensor buffers.
- Avoid repeated per-frame `new byte[]`, `new float[]`, or unmanaged allocation.
- Document any ownership transfer explicitly.

## Metrics Requirements

Any performance work must expose stage timing. At minimum track:

```text
camera fps
display fps
perception fps
camera wait ms
frame copy ms
preprocess ms
palm inference ms
palm postprocess ms
roi crop/warp ms
hand inference ms
landmark postprocess ms
tracking update ms
overlay render ms
end-to-end latency ms
```

Optimization without metrics is not acceptable.

## Hardware Runtime Rules

CPU is the baseline. DirectML and QNN are optional execution paths that must be isolated behind runtime interfaces.

Rules:

- Keep CPU runner working.
- Add DirectML as a separate runner/configuration.
- Add QNN/HTP only after model compatibility and quantization requirements are understood.
- Do not hard-code an EP in the MediaPipe graph logic.
- Log selected EP and fallback behavior when possible.
- Measure whether GPU/NPU actually improves latency and CPU usage.

## Build And Verification

Useful commands:

```powershell
dotnet build src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj --no-restore
dotnet run --project src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj
```

If a process locks build outputs:

```powershell
Get-Process | Where-Object { $_.ProcessName -like 'RyoikiTenkai*' } | Stop-Process
```

Before finalizing changes:

- Build the affected project.
- Report if tests/builds were not run.
- Do not leave required dev servers or long-running processes running.
- Do not delete or overwrite user changes.

## Documentation Rules

Update documentation when changing:

- native setup requirements
- model download paths
- runtime EP behavior
- C++ ABI
- pipeline architecture
- memory ownership rules
- build commands

Use `README.md` for contributor setup and common commands. Use `doc/` for design detail.

## Review Priorities

When reviewing code, prioritize:

1. Correct ownership and lifetime across C#/C++.
2. UI thread blocking.
3. Per-frame allocation and frame queue buildup.
4. Incorrect coordinate transforms or MediaPipe graph deviations.
5. Hidden hardware/runtime fallback.
6. Missing metrics.
7. Missing cancellation or shutdown behavior.

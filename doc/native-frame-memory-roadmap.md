# Native Frame Memory Roadmap

## Purpose

The default native path now retains Media Foundation camera samples as D3D11 textures.
Rendering consumes the texture through DXGI/Direct2D, and a fused D3D11 compute shader
generates the 192x192 palm or 224x224 hand tensor. The full-resolution image is not
copied to CPU memory. Current CPU and QNN runners still require float32 CPU input, so
only the model-sized tensor is copied through a staging buffer after each dispatch.

The previous pooled CPU BGRA/OpenCV implementation remains the numerical reference.
Fixed-gradient tests compare both backends and enforce maximum and mean tensor error
bounds. DirectML GPU tensor binding is now implemented as an opt-in path without
changing MediaPipe-like graph control flow.

## Invariants

Any future capture backend must preserve these contracts:

- A `VisionPacket` owns or leases its frame until every consumer releases it.
- Display and perception use capacity-one/latest-value delivery.
- Palm and hand metadata identify the exact source `frameId`.
- Orientation stays metadata; capture never creates a rotated full-resolution copy.
- Results are stored in logical upright coordinates.
- C# receives copied metadata only and never owns native frame or tensor memory.
- Hardware placement is hidden behind capture and geometry/model-runner interfaces.

## Option A: pooled CPU BGRA

```text
Media Foundation sample
  -> signed-stride copy into pooled CPU BGRA
       -> OpenCV CPU preprocessing -> CPU ONNX Runtime
       -> one CopyFromMemory upload -> DXGI/D2D rendering
```

Keep this option while CPU ONNX Runtime is the production baseline. It has one camera
copy and one CPU-to-GPU display upload, but no GPU-to-CPU synchronization. OpenCV can
wrap the storage directly without another full-frame copy. It is also the simplest
recovery path when a GPU execution provider is unavailable.

Do not replace `std::vector` merely to change its type. If allocation or alignment is
measurably limiting, retain the same CPU ownership contract and replace the backing
storage with a fixed aligned allocation inside `FrameBuffer`/`FramePool`.

## Option B: DXGI camera surface with full-frame readback

```text
Media Foundation DXGI sample
  -> ID3D11Texture2D
       -> renderer uses texture
       -> staging texture -> Map -> OpenCV CPU preprocessing
```

This removes the display upload but adds a GPU-to-CPU copy and synchronization before
CPU perception. It is likely a regression unless camera delivery is already GPU-only
and measured readback cost is lower than the current capture copy plus upload. It must
not become the default based only on the term "zero-copy".

## Option C: DXGI surface with small-tensor readback

```text
DXGI camera texture
  -> GPU rotation + resize + letterbox -> 192x192 palm tensor/readback
  -> GPU ROI transform + crop + resize -> 224x224 hand tensor/readback
  -> CPU ONNX Runtime
  -> original texture -> renderer
```

This is the preferred intermediate experiment. Only model-sized outputs cross from
GPU to CPU. Camera orientation and ROI rotation must be composed into the sampling
transform; no upright full-frame texture is created. The implementation belongs in a
GPU `IGeometryProcessor`, not in the MediaPipe-like graph.

This option still has a synchronization boundary before each CPU inference and may be
slower for the 192x192/224x224 workload on an integrated-memory ARM64 system. Measure
it against Option A.

## Option D: GPU-resident perception

```text
DXGI camera texture
  -> GPU preprocessing
  -> DirectML model runners
  -> small upright metadata on CPU
  -> DXGI/D2D rendering
```

This is the architecture where DXGI capture has the clearest benefit. Frame pixels and
tensors remain GPU-resident, while graph control flow, ROI policy, NMS, tracking, and
ABI metadata remain CPU-owned. DirectML runners and GPU geometry are separate
implementations; the CPU path remains available and observable as a fallback.

QNN/HTP may require different buffer and quantization contracts. It must be evaluated
as another runner/backend rather than added to the renderer or graph policy.

The first QNN experiment therefore keeps the current CPU frame and OpenCV geometry
path. Only the fixed-shape neural inference is moved behind the QNN HTP ORT session.
This isolates NPU compatibility and latency from a simultaneous camera-memory rewrite.
The runtime requires QDQ models and disables CPU EP fallback so measurements cannot
silently include unsupported CPU nodes. Shared-memory tensors, QNN context caching,
and GPU-resident capture remain later, separately measured changes.

## Required interfaces before migration

Do not expose `ID3D11Texture2D` through the C ABI. Native packet storage may evolve to
a tagged lease such as:

```text
FrameStorage
  CpuBgra: data, stride, size
  D3d11Texture: native-owned texture lease, plane/format metadata
```

The renderer consumes either storage type. CPU and GPU geometry processors declare
the memory locations they accept and produce. The camera backend owns Media Foundation
samples and DXGI texture lifetime until a consumer-owned copy or GPU fence makes reuse
safe.

`FrameBuffer` now owns either pooled CPU BGRA, a retained D3D11 texture/sample lease,
or both. Neither representation crosses the C ABI.

## Measurement gate

Record the following for Options A and C on the target ARM64 device before choosing a
new default:

- camera wait and capture copy time
- CPU-to-GPU upload or GPU-to-CPU readback time
- palm and hand preprocessing time
- inference time and synchronization wait
- overlay render and `Present` time
- end-to-end capture-to-present latency
- camera, perception, and display FPS
- dropped frame counts
- CPU, GPU, memory bandwidth, and power use over a sustained run

Use the same camera mode, model files, hand motion sequence, window size, build type,
and warm-up period. A backend is accepted only if it improves sustained latency or
power without reducing tracking stability. Average FPS alone is insufficient; record
at least p50/p95 stage latency and dropped frames.

## Recommended sequence

1. Completed: unify DXGI rendering and measure CPU upload/present costs.
2. Completed: retain Media Foundation D3D11 camera textures without full CPU readback.
3. Completed: fuse orientation, ROI affine sampling, RGB conversion, normalization,
   and NHWC packing in `D3d11HandGeometryProcessor`.
4. Completed: compare GPU tensors with the CPU OpenCV reference on fixed input.
5. Completed: generate the GPU tensor on D3D12 and bind it to DirectML without staging readback.
6. Completed: establish CPU, QNN HTP, and DirectML inference latency baselines.
7. Next: collect controlled sustained accuracy, utilization, and power measurements.

The first DirectML baseline is now implemented as a separate ORT distribution variant.
On the target ARM64 device it improved the observed fixed runtime path relative to the
CPU EP baseline while retaining the staging tensor boundary. The remaining work in
step 5 is specifically D3D12 tensor generation and DirectML I/O binding, not EP
registration or model compatibility.

On 2026-07-18, the target ARM64 Qualcomm driver rejected shared tensor resources with
`E_INVALIDARG`: a D3D11 UAV buffer opened by D3D12, a shared D3D12 buffer opened by
D3D11, and a shared `R32_FLOAT` D3D11 UAV texture. The production design must not rely
on cross-API tensor-buffer sharing on this device. Camera and rendering may remain on
D3D11, but tensor generation and DirectML inference should share one D3D12 device and
command queue. The active path keeps the small staging readback until that D3D12
preprocess implementation is complete.

The Media Foundation camera texture was also not directly shareable: the measured
1920x1080 BGRA texture reported `miscFlags=0`, and opening it from D3D12 failed with
`E_INVALIDARG`. Copying it on-GPU into a D3D11.1 shared BGRA texture did work, and the
result opened successfully from D3D12. A 20-sample diagnostic run measured copy
completion at p50 0.014 ms and p95 1.839 ms. This is promising on the target unified
memory device, but the final decision still requires a sustained D3D12 preprocess run
against the existing small-tensor staging baseline.

The sustained comparison found that this shared-copy candidate is not correct enough
to adopt. With a custom D3D12 preprocess and GPU-bound DirectML input it reached E2E
p50 4.527 ms and p95 5.357 ms, versus p50 4.770 ms and p95 6.312 ms for the existing
small-tensor staging path. However, the GPU path ran palm detection every sampled
frame and never entered the hand-ROI tracking path, while the staging path tracked the
hand normally. The Qualcomm driver requires `SHARED_KEYEDMUTEX` for the shareable BGRA
texture configuration; removing it makes resource creation fail, while D3D12 cannot
use the D3D11 keyed-mutex ownership protocol as implemented here. The apparent latency
gain therefore came with invalid camera/tensor contents and is rejected.

The staging path remains the production default. The next zero-copy investigation is
to create the capture-facing D3D11 device through D3D11On12 on top of the same D3D12
device/queue used by DirectML, then validate Media Foundation compatibility and use
the documented wrapped/underlying-resource ownership APIs. This must pass fixed-input
tensor comparison and real-camera hand tracking before another performance decision.

The first D3D11On12 capture probe succeeded on 2026-07-18. A D3D12 direct queue was
created first, a D3D11On12 device was registered with Media Foundation, and the actual
1920x1080 BGRA camera texture was unwrapped to an `ID3D12Resource`. Diagnostic D3D12
readback sampled an 8x8 grid with 64/64 nonzero pixels (hash
`0xd0c7ae0128a981db`), and `ReturnUnderlyingResource` succeeded. This validates the
camera-allocation direction without a full-frame copy.

The first integrated path is available behind `RYOIKI_DIRECTML_GPU_TENSOR=1`. That
option creates Media Foundation's D3D11 device through D3D11On12, reuses the same
D3D12 device with a dedicated compute queue for tensor generation and DirectML, and removes the
full-frame shared-texture copy. Each palm or hand preprocess performs:

```text
UnwrapUnderlyingResource(camera)
  -> D3D12 resize/orientation/ROI shader
  -> model-sized GPU tensor
  -> camera back to COMMON
  -> signal queue fence
  -> ReturnUnderlyingResource(camera, fence)
  -> DirectML inference from the independent tensor buffer
```

The camera lease ends after tensor generation, not after inference, because DirectML
does not retain the camera resource. For asynchronous display, capture submits one
GPU-local copy into a pooled display texture. The original camera texture is owned by
the D3D12 preprocess path; the copy is owned by the D3D11/D2D renderer. This avoids
sharing one mutable resource across inference and Present without introducing a CPU
readback.

The D3D12 device is shared, but queues are separated: D3D11On12 capture/render uses a
direct queue and D3D12 preprocess/DirectML uses a compute queue. The camera resource
is unwrapped directly onto the compute queue and returned with its compute fence.
Using one direct queue for both paths, or manually cross-signalling both queues,
produced an approximately two-second Present stall on the target Qualcomm driver and
was rejected.

`NativeRenderStage` retains latest frame and latest perception metadata independently.
Either update produces a latest-value render packet, so display does not wait for the
current inference. A real-camera run reached about 29.7 camera FPS and 30.0 display
FPS. Observed E2E was 15.4 ms, including 14.8 ms in `Present(1)` vsync wait. Fixed-input
DirectML tests and the QNN baseline also pass. This remains opt-in until longer
stability and latency/power comparisons are complete.

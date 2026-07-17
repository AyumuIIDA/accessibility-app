# Native Frame Memory Roadmap

## Purpose

The renderer migration deliberately does not change camera or perception memory.
Today Media Foundation copies a top-down BGRA frame into a pooled CPU `FrameBuffer`.
OpenCV creates the 192x192 palm tensor and 224x224 hand ROI tensor from that storage,
CPU ONNX Runtime performs inference, and the renderer uploads the same retained frame
once to Direct2D. Expose and resize redraws reuse that upload; device or camera-bitmap
recreation invalidates it.

This is the baseline to measure before changing capture memory. DXGI capture is not an
automatic optimization while perception still requires full-frame CPU access.

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

Do not add this variant until there is a real second backend. The current
`FrameBuffer` CPU contract remains intentionally concrete.

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

1. Stabilize and measure the current CPU capture plus unified DXGI renderer.
2. Add upload time and `Present` wait as separate metrics if render time is material.
3. Prototype a GPU `IGeometryProcessor` for fused 192x192 and 224x224 sampling.
4. Compare CPU capture against DXGI capture with small-tensor readback.
5. Add a DirectML runner only after GPU preprocessing coordinates match CPU tests.
6. Adopt DXGI capture as the default only when the complete perception/display path
   benefits; otherwise retain pooled CPU BGRA.

# Hardware Runtime Findings

## Scope

This document records the ARM64 Qualcomm device findings through 2026-07-18. Values
are local measurements, not portable performance guarantees. CPU, DirectML, and QNN
remain model-runner implementations behind the MediaPipe-like hand graph.

## Current Data Flow

```text
Media Foundation camera
  -> D3D11On12 camera texture
       |-> GPU-local copy -> pooled display texture -> D3D11/D2D -> Present
       `-> unwrap on D3D12 compute queue
             -> fused orientation/resize/ROI shader
             -> 192x192 or 224x224 float tensor
             -> DirectML inference
             -> CPU metadata/postprocess/tracking
```

The display and perception branches use latest-value delivery. `NativeRenderStage`
stores the newest frame and newest perception result independently. Tensor generation
returns the camera resource with a compute fence before DirectML consumes the separate
tensor buffer. C# receives metrics and copied metadata only.

The display copy is intentional. Concurrent rendering and D3D12 unwrap of the same
camera texture caused resource-ownership stalls. The copy stays inside GPU memory and
was previously measured at p50 0.014 ms and p95 1.839 ms. It is smaller than the
3-5 ms inference work that it allows rendering to overlap.

## Queue And Synchronization Findings

- Camera capture and D3D11/D2D presentation use the D3D11On12 direct queue.
- D3D12 tensor generation and DirectML use a separate compute queue on the same device.
- `UnwrapUnderlyingResource` receives the compute queue directly.
- `ReturnUnderlyingResource` receives the preprocess completion fence.
- A single shared direct queue produced an approximately two-second `Present` stall.
- Manual direct-to-compute and compute-to-direct waits could form a cycle and were rejected.
- A keyed-mutex shared-camera candidate produced invalid perception behavior and was rejected.

These are correctness constraints on the tested Qualcomm driver, not optional tuning.

## Inference Results

Sustained camera measurements gave these median neural inference times:

| Stage | CPU float32 | DirectML GPU float32 | QNN HTP quantized |
| --- | ---: | ---: | ---: |
| Palm | 7.41 ms | about 3.3 ms | 1.53 ms |
| Hand landmark | 7.78 ms | about 3.2-3.3 ms | 1.20 ms |

QNN HTP showed the best pure inference latency: 4.8x CPU for palm and 6.5x CPU
for hand landmark. DirectML was roughly twice as fast as the CPU baseline and avoids
quantization, while also allowing camera preprocessing and inference tensors to stay
on the GPU.

The QNN result is not yet the accuracy winner. The current quantized landmark model
showed material 3D and bone-geometry errors documented in
`qnn-hand-model-quantization.md`. NPU promotion requires improved calibration and
fixed-sequence graph replay, not latency alone.

## Presentation Results

The asynchronous DirectML path reached approximately 29.7 camera FPS and 30.0 display
FPS in the latest real-camera run. The sampled capture-to-present value was 15.4 ms,
of which 14.8 ms was `Present(1)` wait.

`Present(1, 0)` synchronizes presentation to the next display refresh. On a 60 Hz
display this can add up to about 16.7 ms while preventing tearing. Earlier serialized
runs reported 3.7-6.3 ms because inference often completed close to the next vertical
blank; that number does not prove a lower sensor-to-display latency. Presentation wait
and inference time must therefore be evaluated separately.

The next display experiment is a flip-discard frame-latency waitable swap chain with
maximum frame latency one. `Present(0)` is a secondary comparison because lower wait
may introduce tearing and irregular pacing.

## Architecture Decision

Keep the asynchronous packet architecture:

- Preview must not wait for current-frame inference.
- Palm fallback or an EP stall must not accumulate old display frames.
- Rendering can be disabled independently in background mode.
- CPU, GPU, and NPU runners retain the same graph and metadata contracts.

Use DirectML GPU as the current full-precision GPU-resident experiment. Keep CPU as
the numerical and compatibility baseline. Continue QNN HTP work because it has the
best inference latency and is likely the background-power target, but promote it only
after quantized accuracy and sustained power are validated.

## Remaining Measurements

1. Fixed-sequence CPU, DirectML, and QNN accuracy using identical tensors.
2. Per-frame GPU timestamps for preprocess and inference rather than CPU wall time only.
3. Metadata age (`displayFrameId - perceptionFrameId`) and its p50/p95 distribution.
4. Waitable-swap-chain presentation latency and frame pacing.
5. Sustained CPU/GPU/NPU utilization, memory bandwidth, temperature, and package power.
6. Startup session initialization before camera start to remove warm-up frame drops.

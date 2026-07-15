# Native Vision Runtime Contract

This document defines the version 4 contract between the WPF host and
`RyoikiTenkai.Native.dll`. The public declarations are in
`src/RyoikiTenkai.Native/include/ryoiki_native.h`.

## Native pipeline layers

Dependencies point toward data and processing layers, while the runtime composes
them:

```text
CameraCapture
  -> Buffers/FramePool
  -> Pipeline/LatestFrameSlot -> HWND display
  -> Pipeline/PerceptionMailbox (capacity 1)
       -> HandPerception/MediaPipeGraph/HandPerceptionGraph
            -> PalmDetectionGraph -> IPalmDetectionRunner
            -> PalmDetectionToRoi -> rotated hand ROI
            -> HandLandmarkGraph -> IHandLandmarkRunner
            -> HandLandmarksToRoi -> next-frame ROI loopback
            -> CPU ONNX Runtime runners
```

- `Buffers` owns reusable image/tensor storage and memory-location metadata. It does
  not schedule work or interpret hand coordinates.
- `Pipeline` owns delivery policy only. Display retains the latest frame and
  perception overwrites stale pending work.
- `Geometry` owns OpenCV resize, letterbox, rotated ROI warp, coordinate transforms,
  and tensor packing. It does not select an execution provider or decode model output.
- `ModelRunners` owns model loading, model-specific tensor contracts, and raw tensor
  inference only. It does not decode detections or implement tracking policy. Model
  input/output names, shapes, and element types are validated when a runner is created;
  contract incompatibility is a fatal initialization error rather than a per-frame
  fallback.
- `MediaPipeGraph` owns anchor decode, confidence handling, NMS, palm-to-ROI,
  landmark projection, ROI loopback, and palm fallback. It depends on runner
  interfaces and does not select hardware providers.
- `CameraCapture` owns Media Foundation and copies samples into caller-provided native
  storage. It does not publish frames or call perception.
- `ryoiki_native.cpp` is the runtime/C ABI composition root. It owns lifecycle,
  workers, polling snapshots, and the current HWND adapter.

The `RyoikiTenkai.VisionCore` static target contains `Buffers`, `Pipeline`, `Geometry`,
and `HandPerception`, allowing those layers to be tested without a camera, WPF, or a
native window.

## ABI compatibility

- `ryoiki_get_abi_version` returns `kRyoikiAbiVersion` without creating a runtime.
- WPF must reject a DLL whose ABI version does not match its interop declarations.
- Every polled structure starts with `abi_version` and `struct_size`.
- A layout, field meaning, calling convention, or ownership change requires an ABI
  version increment and matching native and managed changes.
- Structures are blittable values. They contain no pointers or variable-length data.
- Status-returning functions use a signed 32-bit integer: zero is failure and one is
  success. The ABI does not expose C++ `bool`.

## Lifecycle and threading

- `ryoiki_create` borrows `parent_hwnd`; it does not own the parent window.
- The parent window must outlive the returned `RyoikiHandle`.
- `ryoiki_start` is idempotent and starts native worker activity.
- `ryoiki_stop` is idempotent and joins all runtime-owned worker threads before it
  returns.
- `ryoiki_resize` resizes the native child window to match the WPF host.
- `ryoiki_destroy` stops the runtime, destroys its child window, and releases the
  handle. The handle is invalid after this call.
- C++ exceptions never cross the C ABI.

The current `HwndHost` creates and destroys the runtime on the WPF UI thread.
Polling may occur while the worker is active. Native code protects snapshot state
with a mutex and copies it into caller-owned structures.

## Memory ownership

- Native code owns camera, image, tensor, rendering, and worker-thread resources.
- WPF owns the output structure passed to a polling call.
- A successful polling call copies one small metadata snapshot into that structure.
- WPF never retains a native image or tensor pointer.
- Version 4 uses CPU memory internally. Future GPU or NPU buffers remain native-owned.

## Frame identity and time

- `frame_id` is monotonically increasing within one started runtime instance.
- Palm and hand results refer to their processed source frame by `frame_id`.
- The capture worker updates capture metrics and frame delivery only. The perception
  worker exclusively owns palm and hand result snapshots, including zero-detection
  results, so capture cannot erase a valid result before it is polled.
- `capture_timestamp_us` is a monotonic native timestamp in microseconds. Its epoch is
  unspecified, so it is valid for durations and ordering, not wall-clock display.
- The capture worker acquires top-down BGRA32 frames from a fixed native frame pool.
- The display path retains one latest frame. The perception path has a capacity-one
  mailbox and overwrites stale pending work.
- The display path paints whichever frame is latest when `WM_PAINT` runs. Frames
  superseded before painting are intentionally dropped.

## Coordinates

- `bbox` is `[left, top, right, bottom]`.
- Palm results include the highest-scoring bbox and seven `[x, y]` keypoints. The
  `palm_count` may be greater than one even though version 4 copies only the best palm.
- Image `x` and `y` values are normalized to `[0, 1]` in the captured image before
  presentation mirroring. `x` increases right and `y` increases down.
- Each landmark is `[x, y, z]`. `x` and `y` use the image convention above. `z` is
  model-relative and must not be interpreted as a metric depth value.
- `handedness` is the model's right-hand score in `[0, 1]`; a negative value means
  unknown.
- `confidence` is the hand-presence confidence in `[0, 1]`.

## Metrics

`RyoikiMetrics` carries the latest values for:

- camera, display, and perception FPS
- camera wait and frame copy time
- preprocess time
- palm inference and postprocess time
- ROI crop/warp time
- hand inference and landmark postprocess time
- tracking update time
- overlay render time
- end-to-end latency and uncategorized native overhead
- frame-pool acquisition drops and perception-mailbox overwrite drops

All durations use milliseconds. A zero value means the stage is not implemented or
has not produced a sample yet. Optimization work must populate the relevant stage
instead of hiding it in `native_overhead_ms`.

The current native implementation populates all listed CPU stages. Palm stages are
zero on frames that successfully use the landmark ROI loopback; they run again after
tracking confidence falls below the fallback threshold.

## Failure handling

The runtime separates initialization failures from frame-local processing failures:

- Missing models, ONNX Runtime session creation failures, unsupported tensor element
  types, and incompatible model contracts are fatal. The perception worker does not
  enter its frame loop and exposes the diagnostic through `ryoiki_get_last_error`.
- A preprocessing, inference, or coordinate-processing failure for one frame is
  recoverable. The worker records the diagnostic, publishes an empty hand result for
  that processed frame, and continues with the next capacity-one mailbox item.
- Low confidence and an invalid tracking ROI are normal graph outcomes. They clear
  tracking and return to palm detection instead of stopping the worker.

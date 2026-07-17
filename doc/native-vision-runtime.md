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
  -> Pipeline/PerceptionMailbox (capacity 1)
       -> HandPerception/MediaPipeGraph/HandPerceptionGraph
            -> PalmDetectionGraph -> IPalmDetectionRunner
            -> PalmDetectionToRoi -> rotated hand ROI
            -> HandLandmarkGraph -> IHandLandmarkRunner
            -> HandLandmarksToRoi -> next-frame ROI loopback
            -> CPU ONNX Runtime runners
       -> Rendering/NativeRenderStage (latest value, render-thread ownership)
            -> D3D11 + DXGI flip swap chain + Direct2D -> atomic present
```

- `Buffers` owns reusable image/tensor storage and memory-location metadata. It does
  not schedule work or interpret hand coordinates.
- `Pipeline` owns perception delivery policy only. Its capacity-one mailbox
  overwrites stale pending work.
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
- `IGeometryProcessor` owns orientation-aware palm/hand tensor sampling and declares
  its input/output memory locations. The current OpenCV implementation is CPU-to-CPU;
  a future D3D/DirectML implementation can be injected without adding device branches
  to the MediaPipe-like graph.
- `CameraCapture` owns Media Foundation and copies samples into caller-provided native
  storage. It normalizes signed scanline stride but preserves media-type rotation as
  frame metadata. It does not allocate a rotated full-frame intermediate, publish
  frames, or call perception.
- `Rendering` joins a source frame with the palm/hand result produced from that exact
  frame. `NativeRenderStage` retains only the latest completed packet and owns its
  D3D11 device, immediate context, DXGI flip-model swap chain, and Direct2D context on
  one render thread. It uploads CPU BGRA once per accepted frame, draws the image and
  overlays into the same DXGI back buffer, and calls `Present` once. The child HWND has
  no GDI, DIB, OpenCV, or WPF image writer.
  Redraw and resize reuse the uploaded camera bitmap when `frame_id` is unchanged.
  A new frame, camera-bitmap recreation, or device-resource recreation invalidates the
  upload cache.
- `ryoiki_native.cpp` is the runtime/C ABI composition root. It owns lifecycle,
  workers, polling snapshots, and the current HWND adapter.

The `RyoikiTenkai.VisionCore` static target contains `Buffers`, `Pipeline`, `Geometry`,
`HandPerception`, and the platform-neutral render packet slot, allowing those layers
to be tested without a camera, WPF, or a native window.

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
- Version 4 camera and perception frames use CPU memory internally. Rendering uploads
  the retained CPU BGRA frame into a reusable Direct2D bitmap. Future GPU or NPU
  buffers remain native-owned; capture-memory migration criteria are documented in
  `doc/native-frame-memory-roadmap.md`.

## Frame identity and time

- `frame_id` is monotonically increasing within one started runtime instance.
- Palm and hand results refer to their processed source frame by `frame_id`.
- The capture worker updates capture metrics and frame delivery only. The perception
  worker exclusively owns palm and hand result snapshots, including zero-detection
  results, so capture cannot erase a valid result before it is polled.
- `capture_timestamp_us` is a monotonic native timestamp in microseconds. Its epoch is
  unspecified, so it is valid for durations and ordering, not wall-clock display.
- The capture worker acquires top-down BGRA32 frames from a fixed native frame pool.
- The perception path has a capacity-one mailbox and overwrites stale pending work.
- Every accepted perception frame produces one terminal render packet, including
  no-hand and recoverable-error results. A render packet always carries metadata from
  the same `frame_id` as its retained frame.
- The display path retains the latest completed render packet. A newer publish
  replaces an unrendered packet; camera capture does not independently trigger a
  normal frame render. `WM_PAINT` only validates the HWND and requests redraw of the
  last retained packet. `WM_SIZE` publishes a latest-only resize command. Resource
  creation, resize, drawing, presentation, and destruction remain on the render
  thread.

## Coordinates

- Camera buffers use the signed Media Foundation stride contract. `IMF2DBuffer` pitch
  is consumed directly; contiguous buffers use `MF_MT_DEFAULT_STRIDE`, including its
  sign. `MF_MT_VIDEO_ROTATION`, when present, is stored on `FrameBuffer`. The CPU
  geometry backend fuses it into the 192x192 palm and 224x224 hand sampling transforms.
  The renderer preserves storage pixels during upload and applies
  `storageToViewport` on the GPU; upright palm/hand metadata uses the matching
  `uprightToViewport` transform. The
  front camera on the current Surface reports enclosure rotation `0` and no media-type
  rotation, so it must not receive a hard-coded 180-degree device correction.
- `bbox` is `[left, top, right, bottom]`.
- Palm results include the highest-scoring bbox and seven `[x, y]` keypoints. The
  `palm_count` may be greater than one even though version 4 copies only the best palm.
- Image `x` and `y` values are expressed in the logical upright image defined by the
  frame orientation metadata, then normalized to `[0, 1]` before presentation
  mirroring. `x` increases right and `y` increases down. Storage width/height remain
  the unrotated buffer dimensions; upright width/height swap for 90/270-degree frames.
- Renderer coordinates are physical back-buffer pixels. The Direct2D context uses 96
  DPI, so DPI is not applied a second time by Direct2D. Continuous image boundaries
  are `[0, width] x [0, height]`; normalized ABI metadata remains `[0, 1]`.
- `FrameTransforms` is the only display transform authority. It provides
  `storageToUpright`, `uprightToViewport`, their composition, and inverse transforms.
  Letterbox and front-camera mirroring are part of `uprightToViewport`.
- Storage orientation is defined once by `geometry::createStorageToUprightTransform`
  and consumed by both perception and rendering. Its matrices use continuous image
  edges. OpenCV sampling converts them to integer pixel-center coordinates through
  `toPixelCenterTransform`, which explicitly applies the `+0.5` input and `-0.5`
  output adapter. Palm tensor sampling and `LetterboxTransform::tensorToSource`
  therefore use the same half-pixel convention.
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

Display FPS counts newly presented render packet frame IDs. Window exposure or resize
redraws that re-present the same packet do not increment the display cadence metric.
`overlay_render_ms` currently measures the complete render operation: BGRA upload,
camera draw, overlay draw, `EndDraw`, and synchronized `Present`.

## Renderer recovery

- Renderer initialization is synchronous with `ryoiki_start`; failure returns an ABI
  failure and records the DirectX diagnostic.
- `DXGI_ERROR_DEVICE_REMOVED`, `DXGI_ERROR_DEVICE_RESET`, and
  `D2DERR_RECREATE_TARGET` trigger one rebuild of device-dependent resources and one
  retry of the retained packet.
- Resize releases the Direct2D target before `ResizeBuffers`, recreates it from the new
  DXGI surface, and redraws the retained packet.
- The initial policy is `Present(1, 0)` with maximum frame latency set to one when
  `IDXGIDevice1` is available. The two-buffer swap chain uses
  `DXGI_SWAP_EFFECT_FLIP_DISCARD`. A waitable swap chain is deferred until latency
  metrics show that it improves this device.

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

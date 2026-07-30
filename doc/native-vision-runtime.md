# Native Vision Runtime Contract

This document defines the version 18 contract between the WPF host and
`RyoikiTenkai.Native.dll`. The public declarations are in
`src/RyoikiTenkai.Native/include/ryoiki_native.h`.

The downstream boundary from hand perception to application features is specified
in [hand-input-architecture.md](hand-input-architecture.md). Recognition is an
optional interpretation layer within that architecture and is specified in
[gesture-recognition-framework.md](gesture-recognition-framework.md). Version 15
publishes generic latest State snapshots while retaining the gesture-specific
Domain Sign fields for compatibility comparison. Ordered Event publication remains
future work.

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
  upload cache. The render packet also carries the Domain Sign State produced from
  that perception frame. Candidate and Active States color the hand ROI/skeleton and
  draw a DirectWrite label next to the ROI; inactive State draws no recognition
  label. Recognition diagnostics are not rendered in the separate CAD viewport.
- `ryoiki_native.cpp` is the runtime/C ABI composition root. It owns lifecycle,
  workers, polling snapshots, and the current HWND adapter.
- `Presentation` owns the user-selectable horizontal hand-coordinate policy.
  Measurements remain in their documented camera/world domains. Native CAD binding
  and the native 3D hand plot independently consume the same presentation mode at
  their consumer boundaries; WPF selects the mode but performs no coordinate math.

The `RyoikiTenkai.VisionCore` static target contains `Buffers`, `Pipeline`, `Geometry`,
`HandPerception`, and the platform-neutral render packet slot, allowing those layers
to be tested without a camera, WPF, or a native window.

## ABI compatibility

- `ryoiki_get_abi_version` returns `kRyoikiAbiVersion` without creating a runtime.
- WPF must reject a DLL whose ABI version does not match its interop declarations.
- Every polled structure starts with `abi_version` and `struct_size`.
- A layout, field meaning, calling convention, or ownership change requires an ABI
  version increment and matching native and managed changes.
- Version 17 adds `ryoiki_set_hand_presentation_mode`. `MirrorDirect` reflects the
  hand X axis so motion follows the mirrored preview, while `Physical` preserves the
  measured X direction. `ryoiki_configure_cad_hand_interaction` also applies this
  shared setting so CAD and the 3D hand plot cannot silently diverge.
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
- `ryoiki_cad_create` creates a separate native CAD viewport under a borrowed WPF
  host HWND without starting another camera or perception session. Its matching
  resize, view, reset, error, and destroy functions operate on `RyoikiCadHandle`.
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

- The default camera path configures an `IMFDXGIDeviceManager` with the renderer's
  shared, multithread-protected D3D11 device. ARGB32 samples remain retained D3D11
  textures. `MF_MT_VIDEO_ROTATION`, when present, is stored on `FrameBuffer`. The GPU
  geometry backend fuses it into the 192x192 palm and 224x224 hand sampling transforms.
  It performs inverse affine sampling, border fill, RGB conversion, `[0,1]`
  normalization, and NHWC packing in one compute dispatch. The legacy CPU geometry
  backend applies the same contract with OpenCV and is retained as a test reference.
  The renderer preserves storage pixels during upload and applies
  `storageToViewport` on the GPU; upright palm/hand metadata uses the matching
  `uprightToViewport` transform. The
  front camera on the current Surface reports enclosure rotation `0` and no media-type
  rotation, so it must not receive a hard-coded 180-degree device correction.
- `bbox` is `[left, top, right, bottom]`.
- Palm results include the highest-scoring bbox and seven `[x, y]` keypoints. The
  `palm_count` may be greater than one even though version 5 copies only the best palm.
- Hand results include the normalized palm normal used by the native 3D direction
  vector. CAD interaction derives relative yaw and pitch from this same direction
  basis rather than from palm-center translation.
- Hand results also include the raw domain-sign confidence, match flag, and six
  component scores for index extension, middle-finger wrap, middle/ring/pinky curl,
  and thumb tuck. These are copied metadata values; gesture detection never
  reprocesses the image in WPF.
- Version 11 also copies the model's 21 metric-scale world landmarks. The official
  MediaPipe gesture embedder consumes these together with the 21 normalized screen
  landmarks and handedness; C# receives copied values and does not own native model
  buffers.
- Version 12 adds the orthonormal palm rotation basis, palm center, palm scale,
  and pose-valid flag. The basis excludes translation and uniform hand scale and
  is continuously available independently of gesture classification.
- Version 13 adds native clutch-reference capture and a weighted least-squares
  relative palm rotation. The estimator uses wrist, four MCP anchors, and a
  low-weight thumb CMC anchor; removes weighted translation and RMS scale; solves
  a robust Davenport/Kabsch rotation; and publishes its normalized fit
  residual. The solver runs three fixed-size Huber IRLS passes so a single
  unstable MCP landmark has less leverage, rejects collinear/unobservable
  geometry, and uses a symmetric Jacobi eigensolver for the 4x4 Davenport
  matrix. No per-frame heap allocation is introduced.
- Version 14 gives every hand snapshot its source `capture_timestamp_us` and
  publishes the screen-palm measurement computed by native code. World pose
  measurements never fall back to image coordinates. `palm_center` is the
  centroid of wrist plus the four MCP anchors. `screen_palm_center` is normalized
  upright image position; `screen_palm_scale` is an isotropic
  upright-frame-width ratio with first-order palm-plane foreshortening
  correction. Tracking loss resets temporal measurement history.
- Version 15 moves continuous CAD hand interaction to native code. WPF sends
  only interaction intent, presentation mode, and sensitivity. Native code
  applies coordinate conversion, dead zone, captured sensitivity,
  capture-timestamp filtering, pan/zoom mapping, tracking-loss policy, and the
  native CAD view update.
- Version 16 removes WPF's 33 ms interaction update loop. WPF configures intent
  through `ryoiki_configure_cad_hand_interaction` only when input/settings change
  and polls `ryoiki_get_cad_hand_interaction` for display. The perception worker
  advances the native CAD binding directly from typed measurements. A shared
  endpoint synchronizes CAD lifetime, mouse/API view access, and CAD destruction;
  no borrowed CAD pointer is retained by the vision runtime.
- The latest-State snapshot publishes built-in ID `1` (`state.domain_sign`) and
  ID `2` (`state.open_palm`). Both recognizers and their time hysteresis run on
  the native perception worker. The ROI overlay displays Candidate/Active state
  text and color without routing recognition through CAD.
- Version 18 adds `ryoiki_read_hand_events`. Swipe Left (`1`) and Swipe Right
  (`2`) are delivered through a fixed-capacity native ring with monotonic sequence
  numbers, bounded batches, and an explicit dropped-event count. WPF owns only
  the read cursor. The native ROI displays a recognized Swipe for `600 ms`.

`palm_rotation_fit_error` is the ordinary weighted RMS distance between the
normalized current palm points and the rotated normalized reference points.
Both point sets are centered and divided by their own weighted RMS radius, so
the value is dimensionless and excludes translation and uniform hand scale.
The reported residual is intentionally not Huber-clipped: the robust weights
protect the rotation estimate from an outlier, while the public error still
exposes that outlier to diagnostics and interaction policy. It is not a
probability or an angular error, and its acceptance threshold must ultimately
be calibrated from recorded landmark sequences.
- The WPF host has no camera, ONNX inference, landmark projection, or overlay
  rendering fallback. Native startup failure is terminal for perception. WPF
  remains responsible only for the shell, polling small metadata, settings, and
  interaction commands.
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
- The native back buffer is split into a camera viewport and a right-side hand plot.
  `Rendering/hand_3d_plot` owns the wrist-relative orthographic projection of the 21
  world landmarks. The renderer draws its grid, axes, bones, and joints in the existing
  Direct2D pass; it does not allocate a second bitmap or upload landmark geometry. The
  plot converts model +Y-down coordinates to a Y-up 3D basis and applies the same
  horizontal front-camera mirror as the camera viewport before projection.
- Right-pane drag and wheel input are reduced to a latest-value `Hand3dView` command.
  The render thread alone applies view changes and redraws the retained packet; window
  messages never access Direct2D or Direct3D resources. Double-click restores the
  camera-aligned front view.
- A fixed-length cyan palm-normal vector is derived from the palm MCP basis and starts
  at the average of wrist and four MCP landmarks. It is projected in the same pass as
  the skeleton to provide a visual orientation-quality check without another buffer or
  model invocation.
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
- camera upload, camera draw submission, overlay draw submission, `EndDraw`, and
  synchronized `Present` wait time
- end-to-end latency and uncategorized native overhead
- frame-pool acquisition drops and perception-mailbox overwrite drops

All durations use milliseconds. A zero value means the stage is not implemented or
has not produced a sample yet. Optimization work must populate the relevant stage
instead of hiding it in `native_overhead_ms`.

The current default populates preprocess timings around GPU dispatch plus the
model-sized staging readback. `frame_copy_ms` is zero when the camera sample remains
GPU-only. Palm stages are
zero on frames that successfully use the landmark ROI loopback; they run again after
tracking confidence falls below the fallback threshold.

Display FPS counts newly presented render packet frame IDs. Window exposure or resize
redraws that re-present the same packet do not increment the display cadence metric.
`camera_upload_ms`, `camera_draw_ms`, `overlay_draw_ms`, `hand_3d_draw_ms`, `end_draw_ms`, and
`present_wait_ms` are CPU wall-clock measurements around the corresponding Direct2D
and DXGI calls. They identify caller-visible waits; they are not GPU timestamp-query
measurements. `overlay_render_ms` measures the complete render operation, including
those stages and transform/resource bookkeeping.

Set `RYOIKI_NATIVE_METRICS_CSV` to an output path before starting WPF to sample these
metrics as CSV. The initial ARM64 Release baseline and its limitations are recorded in
`doc/native-render-performance-baseline.md`.

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

## ONNX Runtime execution providers

Execution-provider selection is owned by the native runtime and injected into the ORT
model runners. `HandPerceptionGraph` and its ROI loopback do not branch on hardware.

```text
RYOIKI_EXECUTION_PROVIDER unset or cpu
  -> float32 palm_detection.onnx + hand_landmark.onnx
  -> CPUExecutionProvider

RYOIKI_EXECUTION_PROVIDER=qnn-htp
  -> fixed-shape palm_detection_qdq.onnx + hand_landmark_qdq.onnx
  -> QNNExecutionProvider with HTP backend

RYOIKI_EXECUTION_PROVIDER=directml
  -> float32 palm_detection.onnx + hand_landmark.onnx
  -> DmlExecutionProvider on GPU adapter 0
```

The HTP configuration uses balanced performance mode, keeps graph I/O quantization on
QNN, and sets `session.disable_cpu_ep_fallback=1`. This makes unsupported QNN nodes or
an incompatible model visible during session creation instead of producing a hidden
mixed CPU/NPU measurement. CPU preprocessing and CPU-owned float graph I/O remain the
initial baseline; shared-memory tensors and context caching are deferred until the
first compatible QDQ models run correctly.

`Microsoft.ML.OnnxRuntime.QNN` 1.24.4 is the version-matched source for ORT headers,
the import library, CPU EP, QNN provider, HTP backend/stubs, and DSP skeleton files.

The DirectML build variant uses `Microsoft.ML.OnnxRuntime.DirectML` 1.24.4 and the
`OrtDmlApi` provider-registration API. It enforces sequential execution, disables ORT
memory-pattern optimization as required by DirectML, and disables CPU EP fallback.
The variants use separate native and WPF output directories because their
`onnxruntime.dll` files are different distributions with the same module name.

The default DirectML baseline still accepts a CPU `Ort::Value`. The opt-in
`RYOIKI_DIRECTML_GPU_TENSOR=1` path instead builds capture D3D11 on the same D3D12
device used by DirectML. It unwraps the Media Foundation camera texture for each
model preprocess, generates the 192x192 or 224x224 float tensor with a D3D12 compute
shader, and binds that D3D12 buffer to ONNX Runtime. The camera texture is returned to
D3D11 with the preprocess fence before inference continues. This avoids both the
full-frame shared-texture copy and the model-sized CPU staging round trip while keeping
the MediaPipe-like graph independent of execution placement.

The asynchronous implementation uses separate queues on one D3D12 device. Camera
capture and D3D11/D2D presentation use the D3D11On12 direct queue. Tensor generation
and DirectML use a compute queue. A pooled GPU-local display texture receives one
D3D11 copy per accepted frame, while the original Media Foundation texture is
unwrapped only by preprocessing. Latest frame and latest perception metadata are
published independently, so rendering can proceed while DirectML consumes the
model-sized tensor. Both paths retain latest-value semantics rather than FIFO queues.
CMake copies the complete architecture-specific native directory beside the native DLL
and native tests. QNN execution is not considered validated until both QDQ models pass
session creation, inference smoke, accuracy comparison, and the same stage-timing run
as the CPU baseline.

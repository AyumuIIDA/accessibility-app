# Native ONNX/NPU Performance Notes

This note summarizes the changes that made the hand-tracking viewer faster and less memory-heavy. The main shift was moving the hot vision path out of managed WPF/C# and into a native runtime that can keep camera frames, image processing, ONNX inference, and rendering close to the hardware.

## Starting Problem

The earlier path was too expensive for real-time hand tracking:

- Camera frames crossed too much managed code.
- High-frequency image conversion and tensor packing created CPU and memory pressure.
- WPF was involved in work that should not happen per frame.
- ONNX Runtime could silently fall back to CPU.
- It was hard to tell whether the app was really using the NPU.
- Camera resolution was larger than needed for the current landmark viewer.

The result was high CPU usage, high memory use, poor latency, and misleading hardware utilization numbers.

## Native Runtime First

The performance work moved the fast path into `src/RyoikiTenkai.Native`.

The native runtime now owns:

- camera capture
- frame buffering
- image resize/crop/warp work
- tensor preparation
- ONNX Runtime session setup
- palm and hand landmark inference
- hand ROI tracking
- Direct3D/Direct2D rendering
- overlay drawing

The WPF app is kept as the shell. It starts and stops the native runtime, shows settings/logs/metrics, and polls small metadata structs. It does not own the per-frame image or tensor path.

That boundary matters because C# is still useful for UI and app workflow, but repeated frame copies, image mutation, and tensor packing are not good work to keep on the WPF side.

## ONNX Runtime And QNN/HTP

The app uses ONNX Runtime through the native runtime. For NPU execution, it registers the Windows ML QNN execution provider targeting HTP.

The important behavior is strict provider selection:

- The native runtime reports the selected execution providers.
- The app expects `QNNExecutionProvider(WindowsML HTP)` for the NPU path.
- Silent CPU fallback is treated as a failure instead of being hidden.
- Fallback reason strings are exposed through metrics when provider setup fails.

This is why the diagnostics are more useful now. Seeing low NPU percentage in Task Manager is not enough to prove failure. The better signal is whether ONNX Runtime actually selected the QNN/HTP provider and whether stage timings improved.

For small models, the NPU may not show 100 percent utilization. The goal is lower latency and lower CPU pressure, not necessarily saturating the NPU graph in Task Manager.

## MediaPipe-Like Hand Graph

The runtime keeps the hand perception logic close to the MediaPipe Hands shape:

```text
frame
  -> palm detector
  -> detection to ROI
  -> hand landmark model
  -> landmarks to next ROI
  -> tracking loopback
```

The key speedup is avoiding palm detection on every frame. Palm detection is used to discover a hand. Once a hand is found, the landmark model updates the hand and produces the next ROI. That ROI is reused on following frames.

This gives a cheaper steady state:

```text
tracked hand ROI
  -> crop/warp ROI
  -> hand landmark inference
  -> next tracked ROI
```

Palm detection only needs to run again when tracking fails or when the runtime periodically searches for another hand.

## Two-Hand Tracking Tradeoff

The native graph now supports up to two hands. Internally it tracks up to two hand ROIs and renders both hands when both pass confidence checks.

To avoid making every frame expensive, second-hand discovery is periodic:

```cpp
kPalmRediscoveryIntervalFrames = 15
```

That means if one hand is already tracked, the runtime does not run the palm detector every frame just to search for a second hand. This keeps latency lower, but it can make the second hand appear later or disappear briefly if the detector/landmark confidence drops.

Lowering the rediscovery interval can make the second hand appear faster, but it increases palm inference work and can make the app slower.

## Camera And Frame Size

The camera target was reduced to `640x480`.

That is a large memory and bandwidth reduction compared with `1280x720`:

```text
1280 * 720 * 4 bytes = about 3.5 MB per BGRA frame
 640 * 480 * 4 bytes = about 1.2 MB per BGRA frame
```

For a hand landmark viewer, the lower resolution is usually enough because the landmark model runs on cropped hand ROIs. The app does not need to push full HD frames through the whole pipeline just to detect and track hands.

## Latest-Frame Queue Policy

The runtime favors interactivity over processing every camera frame.

The intended policy is:

```text
display path: latest frame only
perception path: capacity 1, drop stale frames
metadata path: latest metadata only
action path: ordered after debounce/cooldown
```

This prevents old frames from building up. For accessibility interaction, a delayed perfect answer is worse than a current approximate one.

## Native Rendering

The viewer now uses native Direct3D/Direct2D rendering instead of creating high-frequency WPF visual objects for every landmark and line.

The native renderer can:

- upload the current camera frame once
- draw the hand overlay in the same native render path
- avoid per-frame WPF `Line` and `Ellipse` allocation
- keep rendering off the high-level UI object tree

WPF still hosts the app, but the repeated visual work stays native.

## Metrics Added

The native ABI exposes performance and runtime diagnostics so optimization can be measured instead of guessed.

Tracked fields include:

- camera FPS
- display FPS
- perception FPS
- camera wait time
- frame copy time
- preprocess time
- palm inference time
- palm postprocess time
- ROI crop/warp time
- hand inference time
- landmark postprocess time
- tracking update time
- overlay render time
- end-to-end latency
- frame bytes
- tensor bytes
- dropped/stale frames
- provider names
- fallback reason

These metrics are the main way to verify whether a change helped.

## Signing Issue

One important non-performance blocker was Windows Smart App Control. It blocked unsigned native DLL loading with error `0x800711C7`.

The local build flow now includes a signing hook so the native runtime can load. Without that, the app may appear to be broken or may fail before the optimized path even starts.

## Verification Done

The optimized native path was verified with:

- ARM64 native/WPF build passing
- native tests passing
- managed tests passing
- provider probe reporting QNN/HTP execution providers
- ABI metrics exposing provider and fallback information

The useful provider string is:

```text
QNNExecutionProvider(WindowsML HTP)
```

If that provider is missing and the runtime falls back to CPU, the app is not on the intended NPU path.

## Useful Tuning Knobs

The most relevant knobs are:

- `RYOIKI_EXECUTION_PROVIDER=cpu` for explicit CPU debugging only
- QNN/HTP provider setup for the default NPU path
- camera target resolution and pixel format
- `kPalmRediscoveryIntervalFrames` for second-hand discovery cadence
- confidence thresholds for palm and landmark acceptance
- latest-frame queue behavior

Do not optimize by only watching Task Manager NPU percentage. Use provider selection, fallback reason, per-stage timings, CPU usage, memory use, and end-to-end latency together.

## Current Direction

The current architecture is:

```text
WPF shell
  -> native runtime
       -> camera
       -> MediaPipe-like hand graph
       -> ONNX Runtime QNN/HTP inference
       -> native renderer
  -> WPF polls metrics and hand metadata
```

That structure keeps the heavy path where it can be optimized and keeps WPF focused on UI, settings, logs, metrics, and future gesture/action workflow.

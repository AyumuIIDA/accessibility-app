# CAD-Style 3D Viewer And Hand Interaction Specification

## 1. Goal

Add a separate in-application window that renders a CAD-style 3D scene and lets the
user inspect it with the hand landmarks already produced by the native perception
runtime.

The first release proves a complete application feature:

```text
native typed hand measurement
  -> temporal interaction state
  -> rotate or zoom command
  -> native 3D scene renderer
  -> separate WPF viewer window
```

It does not manipulate arbitrary operating-system windows and does not add another
camera or perception session.

## 2. Release Scope

### Included

- A modeless `CadViewerWindow` opened from the main WPF window.
- One native 3D viewport hosted inside that window.
- A built-in CAD-style demo assembly made from indexed box and cylinder meshes.
- Solid shaded faces, visible edges, a ground grid, XYZ axes, and a neutral dark
  background.
- Mouse controls retained as a deterministic fallback:
  - left drag: orbit;
  - wheel: zoom;
  - double-click: reset view.
- Single-hand controls:
  - stable open palm arms control and then controls orbit;
  - thumb/index pinch controls zoom;
  - leaving the full open-palm pose disengages orbit control.
- On-screen control state and gesture feedback.
- CPU, DirectML GPU, and QNN HTP perception paths feeding the same interaction layer.

### Deferred

- STL, OBJ, STEP, IGES, or glTF file import.
- Editing mesh geometry or CAD constraints.
- Object selection, translation, measurement, section views, or annotations.
- Two-hand gestures.
- Operating-system window movement or resizing.
- Gesture-driven action automation outside the viewer.

STL import is the intended next viewer feature after interaction quality is accepted.

## 3. Window And Layout

The main window adds an `Open 3D Viewer` command. It opens one modeless viewer; invoking
the command again activates the existing viewer instead of creating another one.
Closing the viewer releases only its rendering resources. Camera capture and hand
perception continue until the main runtime is stopped.

The viewer contains:

```text
+--------------------------------------------------------+
| 3D Viewer                         [Reset View] [Help]   |
+--------------------------------------------------------+
|                                                        |
|                native D3D11 3D viewport                |
|                                                        |
|  XYZ axes                         state: ARMED          |
|                                   gesture: OPEN PALM   |
+--------------------------------------------------------+
```

Viewer controls must remain usable with mouse input when no hand is visible. Gesture
input applies only while this viewer is open and active. It must not affect the main
camera preview or any other application.

## 4. Rendering Contract

The native runtime owns the mesh, vertex/index buffers, depth buffer, shaders, swap
chain, and render thread. WPF owns window lifecycle and ordinary controls, but does
not receive mesh or image buffers.

The first renderer uses:

- D3D11 hardware rendering on the runtime's existing adapter;
- indexed triangle meshes with reusable immutable vertex/index buffers;
- a depth buffer and back-face culling;
- one directional light plus ambient light;
- a contrasting edge pass for the CAD outline;
- a perspective camera with a fixed target at the model origin;
- latest-value view commands so gesture events cannot queue old rotations or zooms.

The default camera is an isometric-like view with yaw `-45 degrees`, pitch
`25 degrees`, and the complete model fitted in the viewport. Zoom changes camera
distance, not model geometry. The supported zoom range is `0.35x` through `4.0x` of
the fitted view.

Device loss, viewer resize, and viewer closure are handled on the native render
thread. Rendering failure is reported through the existing diagnostic/error path and
must not stop perception.

## 5. Data Boundary

The existing perception graph remains unchanged. The native interaction layer
consumes the typed measurement produced for each perception frame:

```text
CadHandInput
  frameId
  captureTimestampUs
  confidence
  tracking quality
  normalized screen-palm center and scale
  reference-relative palm rotation and fit error
  explicit validity flags
```

No image, tensor, landmark array, or per-frame view command crosses into WPF.
The native perception worker advances `CadHandBinding` on each fresh measurement
and submits a latest-value `CadView` to the native render stage. WPF sends only
low-frequency interaction intent (mode, presentation, sensitivity) and polls a
small status snapshot for text display. Beginning Rotate also requests reference
capture atomically with the mode transition.

The CAD renderer never calls a model runner. The MediaPipe-like graph remains
viewer-independent; the native outer pipeline publishes its measurement to the
attached feature endpoint after graph execution.

## 6. Interaction State Machine

```text
Ready
  -> Rotating     explicit clutch captures a reference palm pose
  -> Ready        clutch releases
  -> Paused       tracking is temporarily lost while the clutch remains held
  -> Inactive     tracking exceeds the grace period or control is cancelled
```

Only one mode can be active. Mode changes replace pending view commands; commands are
never accumulated in an unbounded queue.

The current independently testable clutch bindings are:

```text
Space          relative native 3D palm pose -> orbit
Shift+Space    normalized screen palm center -> pan
Ctrl+Space     normalized screen palm size -> zoom
```

Rotation sensitivity is an application-binding gain, not a perception
measurement. The default is `1.0x`, preserving one radian of CAD orbit for one
radian of accepted palm rotation. The UI exposes `0.25x` through `2.0x`.
Sensitivity is captured when an interaction begins so changing the slider cannot
jump an active view. Axis direction/reversal remains a separate coordinate
mapping concern. This follows established 3D navigation practice: Blender
describes orbit sensitivity as a simple speed factor, while 3Dconnexion exposes
speed independently from per-axis reversal and navigation mode.

Presentation direction is a shared native policy, independent of sensitivity:

- `MirrorDirect` reflects the hand X axis so horizontal motion follows the mirrored
  camera presentation;
- `Physical` preserves the measured hand X axis.

The same `HandPresentationTransform` is consumed by `CadHandBinding` and the native
3D hand plot. It is applied only after task-neutral measurements are produced.
WPF selects the mode through ABI v17 but does not transform landmarks, rotations,
or deltas.

Each mode captures its own reference at clutch-down. Gesture labels do not gate
entry, continuation, or release.

### 6.1 Common validity

A sample is usable only when:

- hand detection confidence is at least `0.70`;
- all landmarks needed by the active gesture are finite;
- metadata age is at most `150 ms`;
- the source frame ID is newer than the last consumed frame.

Invalid or missing input freezes the view immediately. A continuous `500 ms` absence
returns the state to `Inactive`; reacquisition must arm again.

### 6.2 Scale normalization

All gesture distances are divided by palm scale:

```text
palmScale = distance(wrist, middle-finger MCP)
```

Samples with a degenerate palm scale are rejected. Thresholds therefore do not depend
directly on camera resolution or hand distance.

### 6.3 Arming pose

Arming requires:

- all four non-thumb fingers have a positive MCP-PIP/PIP-tip alignment
  (`cosine >= 0.35`) and their tips reach beyond their PIP joints;
- the thumb segments are aligned (`cosine >= 0.10`) and its tip reaches beyond its
  IP joint;
- the thumb tip is separated from the index MCP by at least `0.30 palmScale`;
- the pose to remain present for `180 ms` on new perception frames.

The UI displays `ARMING` during the interval and `ARMED` after it completes. Arming
does not move the model.

### 6.4 Orbit

While the clutch is held, the native orthonormal palm basis controls orbit using
the relative 3D rotation from the pose captured at clutch-down:

- the relative rotation is first converted by the selected shared horizontal
  presentation transform, then independently converted from camera Y-down to the
  CAD renderer's Y-up basis;
- azimuth change controls yaw;
- elevation change controls pitch;
- a `0.025 radian` angular dead zone suppresses jitter;
- pitch is clamped to `-85` through `85 degrees`;
- yaw wraps continuously;
- an exponential low-pass filter starts at coefficient `0.25`.

The reference basis is captured at clutch-down. Translation and uniform hand scale
do not rotate the model. Gesture classification is not consulted while rotating.
Invalid tracking freezes rotation, and exceeding the grace period requires a new
clutch press so an old reference cannot produce a large jump.

### 6.5 Pinch zoom

The original pinch experiment remains documented below, but the current
gesture-independent validation path uses Ctrl+Space and the ratio between current
and clutch-down screen palm width. The ratio removes fixed camera resolution and
individual hand-size differences; releasing the clutch discards the reference.

Pinch distance is:

```text
pinchRatio = distance(thumb tip, index tip) / palmScale
```

- enter `Zooming` at `pinchRatio <= 0.32` for at least `80 ms`;
- leave `Zooming` at `pinchRatio >= 0.42` for at least `80 ms`;
- record the pinch ratio and camera distance when zoom starts;
- map the filtered change in ratio exponentially to camera distance;
- increasing finger separation zooms in; decreasing separation zooms out;
- clamp to the renderer's `0.35x` through `4.0x` range.

The separate entry/release thresholds provide hysteresis. Zoom has priority over
orbit while the pinch is active.

### 6.6 Disengage and reset

Leaving the full open-palm pose freezes the view immediately and returns to the
inactive state after `120 ms` without changing the view.
Viewer deactivation also disengages control. Reset View is initially a button and
double-click action only; no reset gesture is included in the first release.

## 7. Feedback And Accessibility

The viewer reports these states without relying on color alone:

- `NO HAND`
- `ARMING`
- `ARMED`
- `ROTATING`
- `RELEASE OPEN PALM`
- `SHOW OPEN PALM`
- `ZOOMING`
- `PAUSED: LOW CONFIDENCE`

It also shows the current zoom value and a short instruction for the next valid action.
State changes are logged with frame ID and reason, but per-frame movement is not
logged. Gesture control can be disabled independently while mouse control remains
available.

## 8. Native ABI Direction

The implementation may extend the ABI with viewer lifecycle and status operations.
The exact names may change during implementation, but the contract is:

```text
create/attach CAD viewport to a borrowed parent HWND
resize CAD viewport
set gesture-control enabled
reset CAD view
poll CAD interaction status
detach/destroy CAD viewport resources
```

The main runtime remains the sole owner of camera/perception. The parent HWND is
borrowed and must outlive its attached viewport. C++ exceptions do not cross the ABI,
and any ABI layout change increments the ABI version with matching managed changes.

## 9. Performance Requirements

- No mesh allocation, shader compilation, or WPF visual creation per frame.
- No image/tensor transfer to C#.
- Interaction processing must remain below `0.25 ms` p95 on the target device.
- View-command storage is latest-only.
- Viewer rendering targets display cadence and must not reduce perception throughput
  by more than 5 percent in the same runtime configuration.
- Closing the viewer leaves no render thread, HWND, swap chain, or retained mesh
  resource associated with it.

## 10. Verification And Acceptance

Automated tests cover:

- palm-scale normalization;
- five-finger open-palm and pinch classification;
- 180 ms arming and 120 ms open-palm release timings;
- stale/missing metadata disengagement;
- orbit dead zone, pitch clamp, and yaw wrapping;
- zoom direction and range clamping;
- latest-value command replacement;
- viewer lifecycle without starting a second camera.

Manual acceptance on the target device requires:

1. Open the viewer while native perception is already running.
2. Confirm the built-in model renders with faces, edges, grid, axes, and depth.
3. Arm with an open palm without moving the model during arming.
4. Orbit smoothly in both axes without visible motion while the hand is stationary.
5. Pinch to zoom in and out without orbit occurring at the same time.
6. Close any finger, confirm rotation stops immediately, and confirm a new open palm
   captures a fresh reference without a view jump.
7. Remove and reacquire the hand and confirm re-arming is required.
8. Confirm mouse orbit, wheel zoom, reset, resize, and repeated open/close still work.
9. Repeat with CPU, DirectML, and QNN HTP paths; interaction semantics must match.
10. Confirm closing the viewer does not stop the camera or native runtime.

## 11. Implementation Sequence

1. Extract reusable native 3D camera/view command types from the current hand plot.
2. Add the separate CAD viewport lifecycle and a built-in indexed demo mesh.
3. Add D3D11 depth-tested face, edge, grid, and axis rendering plus mouse controls.
4. Add the pure hand-interaction state machine with synthetic-landmark tests.
5. Connect latest perception metadata to latest CAD view commands.
6. Add the WPF modeless window, status polling, enable toggle, reset, and help text.
7. Run native tests, WPF build, and target-device manual acceptance.
8. After acceptance, specify and implement STL import as a separate change.

## 12. Domain Expansion Sign Detection Probe

Domain Expansion recognition is not a CAD feature and is no longer displayed in the
CAD viewer. Its diagnostic visualization belongs to the main native camera/ROI
overlay. The reference shape is
an extended index finger with the middle finger bent across and around its axis,
while the ring and little fingers remain folded. Official merchandise identifies a
dedicated `Unlimited Void` right-hand part, and visual references consistently show
the intertwined index/middle configuration.

The detector uses only the existing 21 image landmarks and scores:

```text
I  index reach and joint straightness
W  middle MCP and tip on opposite sides of the index axis,
   fingertip proximity to the axis, and projected skeleton intersection
M  middle finger curl
R  ring finger curl
P  pinky curl
T  thumb tuck (lower weight)
```

The combined threshold is `0.68`, with additional gates on index extension, middle
wrap, ring curl, and pinky curl. A raw match enters the timed State framework:

- `Candidate` changes the camera hand ROI and skeleton to amber and displays
  `DOMAIN SIGN CHECKING`.
- A match sustained for 150 ms becomes `Active`; the ROI, skeleton, and label become
  cyan and the label displays `DOMAIN SIGN ACTIVE`.
- The active state is released after 120 ms of a valid non-match.
- Tracking loss is retained as stale for 220 ms, then cancelled.
- Inactive State has no Domain Sign label and uses the normal hand-overlay color.

The CAD viewer contains only CAD interaction state, zoom/pan, pose delta, and its
next-action guidance. Domain Sign does not start, stop, or decorate CAD manipulation.

References:

- https://jujutsukaisen.jp/goods/goods2374.php
- https://jujutsukaisen.jp/goods/goods5406.php

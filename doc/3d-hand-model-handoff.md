# 3D Hand Model / CAD Rotation Handoff

Updated: 2026-07-30

## Purpose

This document lets a new agent continue the native 3D hand-model and CAD
rotation work without reconstructing the full conversation.

Read these repository documents before changing architecture:

- `AGENTS.md`
- `doc/hand-input-architecture.md`
- `doc/cad-hand-interaction-spec.md`
- `doc/native-vision-runtime.md`

The repository is a dirty, concurrently edited worktree. Do not reset, revert,
or overwrite unrelated changes.

## Current Product State

The native pipeline is operational:

```text
camera
  -> MediaPipe-like native hand perception
  -> 21 image/world landmarks
  -> MultiHandMeasurementStage
  -> palm-basis rotation measurement
  -> RotationObservationGate
  -> One Euro filter by default
  -> native CAD interaction binding
  -> 3D viewer
```

Both physical and mirror presentation modes have been exercised successfully.
The 3D viewer and CAD manipulation are functional, though rotation precision
and continuity still require refinement.

C# is the UI/orchestration boundary. High-frequency landmark geometry, rotation
measurement, filtering, and CAD manipulation calculations remain in C++.

## Relevant Files

Primary runtime integration:

```text
src/RyoikiTenkai.Native/src/ryoiki_native.cpp
```

Measurements and rotation:

```text
src/RyoikiTenkai.Native/src/HandInput/Measurements/
  hand_measurement_extractor.{h,cpp}
  multi_hand_measurement_stage.{h,cpp}
  palm_basis_rotation_tracker.{h,cpp}
  rotation_observation_gate.{h,cpp}
  palm_rotation_one_euro_filter.{h,cpp}
  palm_rotation_eskf.{h,cpp}
```

CAD interaction:

```text
src/RyoikiTenkai.Native/src/Features/Cad/
  cad_hand_binding.{h,cpp}
  cad_interaction_endpoint.{h,cpp}
```

Rendering:

```text
src/RyoikiTenkai.Native/src/Rendering/
  d3d11_d2d_renderer.cpp
  hand_3d_plot.*
  native_render_stage.*
```

Tests:

```text
src/RyoikiTenkai.Native/tests/hand_perception_tests.cpp
```

## Measurement Contract

The active contract is multi-hand:

```text
MultiHandMeasurementFrame
  frameId
  timestampUs
  hands[]
    trackId
    HandMeasurements
    ScreenPalmMeasurement
```

Do not equate `handCount > 0` with a usable rotation measurement. Rotation
requires:

```cpp
hand.present
&& hand.quality == HandMeasurementQuality::Valid
```

The rotation interaction binds to a `trackId`. UI recognition and legacy
single-hand presentation continue to consume the primary hand independently.

## Palm Rotation Measurement

The low-cost palm basis is shared conceptually with the rendered palm pose:

```text
X = normalize(world[5] - world[17])
Y = normalize((world[9] - world[0]) projected orthogonal to X)
Z = normalize(X cross Y)
```

`PalmBasisRotationTracker` stores a reference basis and returns:

```text
Rrelative = Rcurrent * transpose(Rreference)
```

Position and uniform scale are mathematically removed by basis normalization.
Nevertheless, MediaPipe world-landmark bias can change with ROI position, hand
size, camera distance, occlusion, and palm pitch. This can appear as
position/scale-dependent rotation error even though the basis calculation itself
is translation- and scale-invariant.

The earlier six-point fit and ESKF remain available for diagnostics or
experimentation but are not the default observation/filter path.

## Reference Capture and Track Rebinding

A previously identified severe bug consumed the reference-capture request even
when the current measurement was invalid:

```cpp
capturePalmRotationReference.exchange(false)
```

This was replaced by a level-triggered contract:

- Keep the request pending until both palm-basis trackers successfully capture
  a valid typed measurement.
- Clear the request only after success.
- Bind the rotation session to the captured `trackId`.

Strict permanent `trackId` binding then caused multi-second stalls after
perception assigned a new ID to the same physical hand. The runtime now:

- prefers the current `trackId`;
- evaluates replacement candidates by handedness and last screen position;
- requires the same candidate for two consecutive observations;
- captures the new local reference;
- invokes continuity-preserving gate reacquisition;
- preserves the last published output orientation across the handoff.

Do not remove this behavior when changing the observation gate.

## Current Rotation Observation Gate

The gate intentionally no longer uses camera timestamps or `dt`.

Current rule:

```text
For consecutive valid inference inputs:
  delta = SO(3) geodesic angle between last raw rotation and current rotation
  accept when delta <= 90 degrees
  reject as AngularJump when delta > 90 degrees
```

The geodesic angle is derived from:

```text
deltaR = transpose(Rprevious) * Rcurrent
angle = acos(clamp((trace(deltaR) - 1) / 2, -1, 1))
```

Important details:

- The old fixed 20-degree threshold was removed because it rejected normal fast
  hand rotation.
- The old 720-degrees/second rule was removed because timestamp intervals
  occasionally collapsed to about 0.04 ms, turning 1–5 degree movements into
  enormous false angular speeds.
- `RotationObservationRejection::AngularSpeed` remains only for CSV/numeric
  compatibility and should no longer be emitted.
- Invalid measurements enter `Holding`.
- A stable finite trajectory enters `Reacquiring`.
- Explicit track rebinding calls `beginReacquisition()`.
- Reacquisition rebases the new raw trajectory onto the last effective output,
  avoiding a visible orientation reset.
- Three stable candidate inputs are currently required before publication.

The gate compares consecutive valid inference inputs, not camera timestamps and
not adjacent capture-frame IDs. The perception path uses latest-frame semantics,
so skipped capture IDs are normal and must not automatically trigger
reacquisition.

## Temporal Filter and Sensitivity

Default temporal filter:

```text
SO(3) One Euro
  min cutoff: 6 Hz
  beta: 0.05
  derivative cutoff: 1 Hz
```

ESKF was tested but did not produce a clearly better subjective result. One Euro
was retained for lower latency.

CAD rotation currently applies an independent pitch gain:

```text
pitch gain = 1.6
```

The user reported that this improved pitch responsiveness.

Do not compensate measurement defects by silently changing CAD sensitivity.
Keep measurement validity/filtering separate from interaction mapping.

## Latest Log Findings

Rotation diagnostics are written through:

```text
RYOIKI_ROTATION_COMPARISON_CSV
```

Recent files:

```text
gesture-recordings/rotation-gate-validation-latest.csv
gesture-recordings/rotation-comparison.csv
```

The last analyzed log before replacing the angular-speed gate contained:

```text
1,760 rows
58.987 seconds
AngularJump: 0
AngularSpeed: 32
Invalid measurement: 116
Filtered valid: 1,568
Basis valid: 1,644
Reacquired: 18
```

False angular-speed examples:

```text
2.21 degrees / 0.037 ms
3.16 degrees / 0.041 ms
0.86 degrees / 0.037 ms
```

Likely inference failures:

```text
177.62 degrees between valid observations
178.76 degrees between valid observations
```

This evidence motivated the current time-independent 90-degree SO(3) rule.

### 90-degree gate validation run (2026-07-31)

A 2,070-row / 69.1-second run at 30.0 fps confirmed all three criteria:

```text
gate_rejection == AngularSpeed        0    (was 32)
AngularJump                           4    all at 115.6 - 161.9 degrees
raw deltas above 90 degrees           4    no jump rejected below 90
stale-trackId signature rows          1    (six_valid=1 and basis_valid=0)
Tracking / Holding / Reacquiring      1958 / 99 / 13
reacquisitions                        6
```

The only run longer than one second was the end-of-log interval where the hand
left the frame, not a stale-track stall. Timestamp collapse still occurs (13
intervals below 1 ms, minimum 0.032 ms), which reconfirms that the angular-speed
rule had to be removed. The old fixed 20-degree threshold would have rejected
about 4.2 percent of this run.

The four rejections are genuine single-frame inference flips: at frame 340 the
palm basis and the independent six-point estimate moved together from 85 to 172
and back to 84 degrees. A two-trajectory or prediction-innovation gate is
therefore not currently justified.

Remaining quantified findings:

```text
quasi-stationary per-frame delta   p50 1.03 deg, p90 3.65 deg, mean 2.13 deg
1-second path length / net change  1.2 - 6.0, median about 2.2
six-vs-basis difference by rotation magnitude
  0-15 deg   p50 1.31      30-60 deg   p50 4.38
  15-30 deg  p50 1.96      130+ deg    p50 6.35
```

Accuracy degrades with rotation magnitude relative to the reference pose.
Position- and scale-dependence remains unproven because the CSV still lacks
screen center, screen scale, and track diagnostics.

### Reference drift instrumentation

`RotationObservationGate` now reports the divergence between the published
trajectory and the raw absolute measurement:

```text
rebaseOffsetDegrees   net divergence, reported on every update
rebaseStepDegrees     divergence injected by one recovery
```

Each recovery discards the motion across its unobservable gap, so the offset
accumulates for the whole session and is never re-anchored to the absolute
basis. At the measured moving speed of 5.3 degrees per frame and a two-frame
gap, one recovery injects roughly 10 degrees.

The CSV gained `gate_rebase_offset_deg` and `gate_rebase_step_deg`. Row emission
no longer depends on the legacy six-point comparison reference, which previously
suppressed every row when that reference failed to capture and produced an empty
log. Analyze a run with:

```powershell
pwsh tools/analyze_rotation_gate.ps1 -Path gesture-recordings/<log>.csv
```

### Drift measurement run (2026-07-31, 181.8 s, 5,437 rows)

Reference drift does **not** accumulate across a session. Every reference
capture calls `palmRotationObservationGate.reset()`, so a clutch press clears
the offset. Three resets were observed. Within a hold, however, one recovery
injects far more than a gap-motion estimate suggests:

```text
recoveries              9
injected offset sum     775.8 deg   (mean 86 deg per recovery)
peak offset             158.8 deg
time above 30 deg       about 7.4 s of 181.8 s
```

The offset distribution is bimodal: 94.7 percent of published frames carry
under 1 degree and the remainder carry more than 30, with nothing in between.
This is not gradual drift; it is a per-event injection.

Session 3 ran 98 seconds with 2,857 published frames, zero recoveries and zero
drift. The problem is concentrated entirely in perception-failure episodes.

Frames 1686-1830 contain seven of the nine recoveries. The rejected deltas
include 175.09, 176.68 and 174.84 degrees, and the six-point estimate moved with
the palm basis (difference 13.9 to 20.3 degrees), so the landmarks themselves
flip rather than either estimator failing. Large rotations are where this
concentrates:

```text
six-vs-basis difference   [ 60, 90) p50  4.04, p90  8.26
                          [130,200) p50 12.44, p90 31.74
```

Recovery causes split as follows, which bounds what outlier rejection can fix:

```text
transient flip during Tracking     3 recoveries   288.3 deg injected
recovery after real tracking loss  6 recoveries   487.5 deg injected
```

### Transient outlier rejection

Rejecting an implausible delta now holds the output without abandoning the
trajectory. `lastRaw_` is kept, so an observation that returns to the previous
branch resumes with no injected offset. Only a run longer than
`kOutlierBudgetFrames` (5 frames, about 167 ms, deliberately shorter than the
CAD binding's 220 ms grace) escalates to the previous rebase recovery. An
invalid measurement still abandons the trajectory immediately, because tracking
loss carries no continuity information.

The 90-degree threshold is a per-inference-step rule, never a per-timestamp
angular speed. The recorded separation supports it directly:

```text
accepted raw deltas   max 67.01 deg   (about 2,010 deg/s)
rejected raw deltas   min 98.02 deg
```

The CSV gained `gate_outlier_frames`. This change addresses the transient-flip
share only; the post-loss share still needs either an absolute re-anchor or
structural detection of the basis sign flip.

Two open measurement gaps: the CSV records no CAD interaction state, so it
cannot yet show how much of a residual offset reached the model, and it does not
distinguish "no rotation session bound" from "rotation lost during a session".
The first 949 rows and the last 77 rows of the drift run are the former.

Two operational hazards:

- `RYOIKI_ROTATION_COMPARISON_CSV` opens with `std::ios::trunc`, so every
  application start destroys the previous log. Use a timestamped file name.
- WPF copies its native DLL from `build/RyoikiTenkai.Native.Qnn.Arm64`, not from
  the `build/RyoikiTenkai.Native.OpenCv` directory used by the verification
  commands below. Build both before recording a run.

## User-Reported Remaining Problems

1. Rare, very discontinuous rotation still needs observation with the new
   90-degree gate.
2. Accuracy seems to degrade as the hand moves away from its initial position,
   apparent size, and capture conditions.
3. Jitter remains visible in some palm orientations, especially pitch.

The current CSV cannot prove position/scale-dependent degradation because it
does not record all required variables.

Recommended next logging additions:

```text
active_track_id
track_rebound
screen_palm_center_x/y
screen_palm_scale
palm_world_scale
absolute_basis_angle
raw consecutive SO(3) delta
gate rejection/state
ROI center/size/rotation
hand confidence
world-landmark topology/shape residual
```

Add metrics without moving calculations into C# and without per-frame heap
allocation.

## Likely Next Refinement

First collect a fresh log using the current 90-degree gate. Do not immediately
introduce another filter.

If position/scale correlation is confirmed, evaluate a hybrid rotation
measurement:

```text
primary interaction motion:
  integrate accepted frame-to-frame relative palm-basis deltas

slow correction:
  use absolute reference rotation only when ROI/scale/shape conditions are
  comparable to the reference capture
```

This can reduce initial-reference distribution bias, but it introduces drift
management. Keep absolute and incremental measurements typed and separate; do
not bury the correction inside CAD sensitivity code.

For outlier refinement beyond the temporary 90-degree rule, prefer a
two-trajectory or prediction-innovation gate:

- normal continuous trajectory;
- provisional outlier trajectory;
- use the following observation to distinguish a genuine fast movement from a
  one-frame inference flip.

Avoid returning to a low fixed threshold such as 20 degrees.

## Verification Status

After the current time-independent gate implementation:

```text
ARM64 native OpenCV build: passed
CTest: 2/2 passed
ARM64 WPF build: passed
Warnings/errors: 0
```

Commands used:

```powershell
$buildCommand = 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=arm64 -host_arch=arm64 && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build\RyoikiTenkai.Native.OpenCv --config Debug && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build\RyoikiTenkai.Native.OpenCv --output-on-failure -C Debug'
cmd.exe /d /c $buildCommand

dotnet build src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj `
  --no-restore -p:Platform=arm64
```

Do not launch the GUI during an unattended agent run.

## Cross-Model Delegation Note

The `cross-model-agent-delegation` marketplace package exists locally, but in
the session that created this handoff it was present only under Codex's
temporary marketplace cache and was not listed in the active skill catalog.

The provided `delegate-to-claude` wrapper correctly selected:

```text
review
claude-sonnet-5
high effort
read-only
```

Claude authentication was valid, but normal sandboxed shell execution could not
connect to `api.anthropic.com:443`. DNS worked. TCP succeeded when tested outside
the Codex sandbox. Therefore future Claude delegation must be run with approved
external-network execution, not by changing authentication or adding an API
key.

## Handoff Checklist

1. Read the four architecture documents listed at the top.
2. Inspect `git status`; preserve every unrelated modification.
3. Read the current gate, runtime integration, and its tests rather than relying
   only on this summary.
4. Ask the user to generate a fresh `-latest` rotation log with the 90-degree
   gate.
5. Analyze rejection counts and longest invalid-output runs.
6. Add position/scale/track/ROI diagnostic fields before claiming reference
   drift.
7. Keep the heavy path and all physical calculations in C++.
8. Build native ARM64, run CTest, and build ARM64 WPF before handoff.

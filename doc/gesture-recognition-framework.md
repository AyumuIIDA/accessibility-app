# Hand Recognition Framework

Status: incremental design. Recognition is an optional interpretation layer within
the architecture defined by [hand-input-architecture.md](hand-input-architecture.md).

`HandMeasurements`, `HandMeasurementExtractor`, Domain Sign geometry, and the
weighted native palm-rotation tracker are implemented and replay-tested under
`HandInput/Measurements` and `HandInput/Recognition`. ABI version 18 exposes world
landmarks, palm pose, relative rotation, and fit quality. Generic latest-State
publication is implemented for Domain Sign and Open Palm. Ordered Event
publication and the first Swipe Left/Right Event recognizer are implemented.

Native CAD rotation passes two geometrically estimated observations through
`PalmRotationEskf` before feature mapping:

- clutch-reference to current landmarks provides an absolute SO(3) orientation
  observation and prevents accumulated drift;
- previous-frame to current landmarks provides a local angular-velocity observation;
- the nominal state is orientation plus body angular velocity, with a 6x6 error
  covariance over small-angle and angular-velocity errors;
- normalized rigid-fit error increases observation covariance, and a Mahalanobis
  gate rejects implausible corrections.

The filter does not select landmarks or change the rigid fitting algorithm.
`WeightedPalmRotationTracker` remains the observation source so landmark-set and
weight experiments can be evaluated independently from temporal filtering. The CAD
binding no longer adds its former fixed 75 ms exponential filter; it consumes the
ESKF rotation and retains application sensitivity, dead zone, presentation, and
view constraints.

The default is ESKF. For an A/B run using the same landmark source and rigid fit,
set `RYOIKI_PALM_ROTATION_FILTER=raw` before starting the process. Unset the
variable, or set it to `eskf`, to restore filtering. This switch deliberately does
not change landmark selection, weights, presentation, or CAD sensitivity.

### Rotation-source comparison capture

Set `RYOIKI_ROTATION_COMPARISON_CSV` to enable a diagnostic comparison between:

- the six-anchor robust rigid fit (`0, 5, 9, 13, 17, 1`); and
- the orthonormal palm basis used by the 3D plot, constructed from
  index MCP minus pinky MCP (`5 - 17`) and wrist to middle MCP (`9 - 0`).

Despite the convenient “three-axis basis” name, the plot calculation uses four
distinct landmarks (`0, 5, 9, 17`), not three landmarks. Both comparison trackers
capture the same first valid frame after logging starts. The CSV records raw
source estimates before ESKF, Presentation, CAD sensitivity, or dead-zone mapping.
It includes each rotation magnitude, the SO(3) geodesic difference, an XYZ
difference rotation vector, tracking confidence, and the six-point fit error.
Because this opt-in diagnostic writes on the perception worker, it must be disabled
for normal latency measurements.

Use `tools/analyze_rotation_comparison.ps1` to report mean, median, p95, maximum,
RMS, axis RMS, and errors grouped by palm-basis rotation magnitude.

The native Open Palm baseline consumes typed finger-extension and
thumb-separation measurements. It uses a `120 ms` enter duration, a `150 ms`
exit duration, and a `220 ms` missing-input grace period. This is a practical
geometry baseline, not a claim of parity with the official canned classifier;
thresholds must be calibrated from native-runtime recordings.

## Scope

Recognition converts observations and typed measurements into semantic results:

```text
State
  a condition that persists
  Open Palm active, Pinch active, Domain Sign active

Event
  a bounded occurrence recognized once
  Swipe Left, Circle Complete, Wave
```

Measurements are not recognition results:

```text
palm rotation
palm translation
pinch distance
screen scale
finger extension
```

A feature may consume a Measurement directly. CAD orbit, for example, requires a
reliable reference-relative palm rotation and does not require a gesture classifier.
A State may act as an interaction gate, and an Event may map directly to a command.

`Pose`, `Analog`, and `Sequence` describe possible algorithm characteristics, not
the public result taxonomy. They are not mutually exclusive: pinch is a persistent
State that exposes a distance Measurement, and a circle may be either a completed
Event or a continuous measurement depending on the application.

Recognition does not own application intent, pointer capture, CAD policy, Windows
input injection, model-provider selection, or the MediaPipe-like ROI loop.

## Pipeline placement

```text
CameraStage
  -> HandPerceptionStage
       MediaPipe-like palm / ROI / landmark graph
  -> HandObservation
  -> HandMeasurementStage
  -> optional RecognitionStage
       State recognizers
       temporal Event recognizers
  -> GesturePublicationStage
       latest States
       ordered Events

C++ native
  -> InteractionSession
  -> ApplicationBinding
  -> high-rate feature command

C# / WPF
  -> low-rate intent/configuration
  -> lightweight status polling
```

Recognition runs after perception on the perception worker. It cannot change ROI
loopback, confidence fallback, or model-runner placement.

## Recognition input

The long-term input is the typed observation and measurement model described in
`hand-input-architecture.md`. The current aggregate `HandMeasurements` is an
incremental bridge and must not become an unstructured bag of every possible
feature.

The input must make these coordinate domains explicit:

```text
canonical hand-local landmarks
  pose and shape invariance

image-space center, scale, and velocity
  pan, swipe, and screen interaction

palm orientation and relative rotation
  3D orientation and CAD manipulation

quality and validity
  whether any of the above may be trusted
```

Do not derive screen translation from world-landmark origin motion. MediaPipe-style
world landmarks describe hand geometry and are not a guaranteed camera-space hand
position. Use image-space palm motion for pan and swipe.

Common shape measurements may include:

```text
finger extension and curl
finger spread
thumb opposition
normalized thumb-index distance
signed joint angles where needed
```

Thumb geometry must not be treated as identical to the four fingers when precision
matters.

## Quality and stability

Recognizer confidence answers "how strongly did this recognizer match?" Input
quality answers "was the underlying observation trustworthy?" They are separate.

Relevant quality gates may include:

```text
presence confidence
finite and plausible bone geometry
temporal landmark stability
screen coverage and clipping
palm incidence
tracking-reacquisition age
world/image availability
rotation fit residual
```

An invalid input cancels or suspends recognition according to the recognizer
lifecycle. It is not a low-confidence negative sample and must not be represented by
zero-valued landmarks.

Persistent State recognizers use time-based hysteresis:

```text
candidate duration
enter threshold
maintain threshold
exit threshold
missing-input grace period
reacquisition warm-up
minimum hold duration where appropriate
```

Threshold duration is measured in monotonic time, not frames. A bounded EMA or
median filter may be used when replay tests demonstrate reduced jitter without
unacceptable activation latency.

## Results and identity

Every recognition result has a persisted numeric ID and descriptor:

```text
0                         invalid
1..65535                  built-in
65536..2147483647         application/template-defined
```

IDs are not array positions or raw hashes. Descriptors map IDs to stable UTF-8 keys:

```text
state.open_palm
state.pinch
state.domain_sign
event.swipe_left
```

```cpp
enum class HandRecognitionKind : std::uint32_t
{
    State = 1,
    Event = 2
};

enum class HandRecognitionPhase : std::uint32_t
{
    Idle = 0,
    Candidate = 1,
    Began = 2,
    Updated = 3,
    Recognized = 4,
    Ended = 5,
    Cancelled = 6
};

struct HandRecognitionResult
{
    std::uint32_t id{};
    HandRecognitionKind kind{};
    HandRecognitionPhase phase{};
    std::uint32_t flags{};
    float confidence{};
    float inputQuality{};
    float progress{};
    std::uint64_t beganFrameId{};
    std::uint64_t currentFrameId{};
    std::uint64_t timestampUs{};
};
```

Usual lifecycle:

```text
State: Candidate -> Began -> Updated -> Ended / Cancelled
Event: Candidate -> Recognized / Cancelled
```

Continuous numeric values remain typed Measurements. Do not copy palm rotation or
pinch distance into a recognition result merely because a State gates their use.

## Recognizer implementations

Do not introduce one universal recognizer interface until two concrete
implementations need the same contract. Static State and temporal Event recognizers
have different storage and output requirements and may initially use separate
concrete APIs.

A justified State interface may eventually be:

```cpp
class IHandStateRecognizer
{
public:
    virtual ~IHandStateRecognizer() = default;
    virtual void reset() noexcept = 0;
    virtual void process(
        const HandRecognitionInput& input,
        HandRecognitionSink& output) noexcept = 0;
};
```

A recognizer:

- processes one new perception result at a time;
- retains bounded scalar/feature history, never image frames;
- performs no steady-state heap allocation;
- never calls WPF, CAD, actions, or execution-provider APIs;
- resets on runtime restart, incompatible tracking reacquisition, or configuration
  change;
- reports diagnostics without throwing across the C ABI.

### Geometry and model-backed States

Simple poses should start with geometry or a hybrid of geometry and an available
classifier. The official MediaPipe canned classifier is useful evidence but is not
assumed to be authoritative for this project's landmark model. Recorded validation
shows a major Open Palm recall decline at high palm incidence.

A practical Open Palm State therefore combines:

```text
available classifier score
finger extension and thumb geometry
palm-incidence quality gate
time-based hysteresis
tracking-loss grace
```

Do not require a new learned model for the demo. A custom classifier is justified
only after recorded false positives and negatives provide a representative dataset.

Model-backed implementations depend on an `IHandGestureModelRunner` only when a real
model is integrated. The runner owns tensor contracts and provider selection.
Packages must declare model/version, feature schema, normalization, inputs/outputs,
class-to-ID mapping, and recommended thresholds. A contract mismatch disables that
recognizer while perception remains available.

### Temporal Events

The first production implementation is a bounded, measurement-based Swipe
Left/Right recognizer. It uses normalized screen-palm displacement, path
straightness, duration, scale consistency, tracking quality, and the raw Open Palm
condition. It retains scalar trajectory state only, emits once, and enters a
cooldown to prevent duplicate delivery. This establishes segmentation and ordered
Event delivery without claiming that a learned temporal model is present.

The current thresholds are an initial demo calibration:

```text
motion start             0.025 screen widths
accepted displacement    0.16 screen widths
duration                 100-700 ms
minimum straightness     0.78
cooldown                 350 ms
```

They must be evaluated against positive and everyday-motion negative recordings.

The first temporal implementation may be a user-dependent DTW recognizer. It is
appropriate for a small vocabulary with explicit recording and interaction gates;
it is not a general cross-user dynamic-gesture solution.

Minimum DTW design:

```text
gesture-specific low-dimensional projection
fixed-cadence resampling
fixed-capacity history
multiple templates per gesture
Sakoe-Chiba constraint
rolling cost rows
early abandonment
acceptance threshold
second-best ambiguity margin
```

Begin with explicit segmentation or an active interaction gate. Automatic spotting
in continuous everyday motion is deferred until negative replay demonstrates an
acceptable false-activation rate.

Do not add an abstract temporal-algorithm hierarchy until a second algorithm such as
an HMM or learned temporal model is actually integrated.

## Registration and configuration

Compiled recognizers are composed explicitly by the runtime. This is not a dynamic
plugin system or a service locator.

```cpp
recognizers.add(createDomainSignState(config));
recognizers.add(createOpenPalmState(config));
recognizers.add(createSwipeLeftEvent(config, templates));
```

Configuration is immutable during one run. Controlled restart applies threshold,
model, or template changes and resets temporal state. Template recording is an
application workflow and performs no filesystem work in the per-frame `process`
path.

## Publication

States and Events require different delivery:

```text
State  latest snapshot, with lifecycle phase
Event  bounded ordered ring, monotonic sequence, caller-owned cursor
```

Overflow is explicit, never blocks perception, and causes the application to cancel
unsafe interaction state. Logs record lifecycle transitions and overflows, not every
frame.

ABI version 15 retains temporary gesture-specific fields for comparison, but WPF
consumes Domain Sign lifecycle through `ryoiki_get_latest_states`. New recognizers
must not add another field to `RyoikiHandResult`.

The implemented State contract uses independent current phase and transition axes:

```cpp
struct RyoikiHandState
{
    std::uint32_t id, phase, transition, flags;
    float confidence, input_quality;
    std::uint32_t reserved0[2];
    std::uint64_t began_frame_id, current_frame_id, timestamp_us;
    std::uint32_t reserved[4];
};

struct RyoikiHandStateSnapshot
{
    std::uint32_t abi_version, struct_size;
    std::uint64_t frame_id, timestamp_us;
    std::uint32_t count, flags;
    RyoikiHandState states[16];
};
```

The ordered Event contract uses:

```cpp
struct RyoikiHandEvent
{
    std::uint64_t sequence;
    RyoikiHandState state;
};

struct RyoikiHandEventBatch
{
    std::uint32_t abi_version, struct_size, count, flags;
    std::uint64_t newest_sequence;
    RyoikiHandEvent events[16];
};
```

APIs:

```cpp
ryoiki_get_latest_states(handle, out_snapshot);       // implemented
ryoiki_poll_events(handle, after_sequence, out_batch); // future
```

State transitions in a latest snapshot are informational and may be overwritten
before a slow caller polls them. Action-triggering transitions must use the future
ordered Event path or be derived safely from successive State phases.

## Application interaction boundary

`NativeVisionHost` owns ABI validation, settings and intent commands,
latest-state polling, and an event cursor. It never classifies landmarks or
calculates continuous CAD motion.

The native `CadHandBinding` owns capture lifecycle and maps typed native
Measurements to native CAD view values. The current ABI accepts low-frequency
application intent, presentation mode, and sensitivity and returns a small
status snapshot for display.

Examples:

```text
physical Space gate + PalmRotationMeasurement -> CAD orbit
Open Palm State + PalmRotationMeasurement     -> optional CAD orbit
Pinch State + PinchDistanceMeasurement        -> CAD zoom
Swipe Left Event                              -> registered action
```

CAD rotation sensitivity remains in the native `CadHandBinding`. It is a dimensionless
application gain applied after the physical dead zone and before temporal
filtering; it never changes the native rotation estimate or its fit residual.

## Metrics and validation

Expose, when implemented:

```text
measurement extraction ms
state recognition ms
event recognition ms
publication ms
invalid-quality frames
state transitions
event-ring overflows
```

Replay is the primary deterministic test input. It must preserve observation,
measurement, quality, frame ID, and timestamp when available.

Validation includes:

```text
State:
  precision, recall, confusion matrix
  false activations per hour
  activation and release latency
  palm-incidence and screen-coverage buckets
  left/right hand and subject-held-out evaluation
  tracking loss and reacquisition

Measurement:
  static jitter, drift, step response
  fit residual and angular error
  end-to-end latency
  missing-input suspension and recovery

Event:
  recall and false activations per hour
  speed, size, position, and direction variation
  second-best ambiguity
  negative everyday-motion replay
```

For PC actions, false activation rate is a primary acceptance criterion; aggregate
classification accuracy alone is insufficient.

## Migration

1. **Implemented:** extract common hand measurements and weighted palm-relative
   rotation; retain replay tests.
2. Align the current implementation and documentation on image, world, canonical,
   orientation, and quality fields.
3. Preserve current CAD behavior while separating measurement
   semantics from recognition semantics.
4. **Implemented:** move Domain Sign behind a timed State recognizer and generic
   latest-State snapshot; retain old fields for comparison.
5. Add a quality-gated Open Palm State without making it mandatory for CAD.
6. **Implemented:** move the continuous CAD interaction session, coordinate
   conversion, sensitivity, filtering, and view mapping into native
   `CadHandBinding`; WPF retains only intent/configuration and status display.
7. Add the first temporal Event with explicit gating and replay evaluation.
8. Add the ordered Event ABI after the first temporal Event is implemented.
9. Remove deprecated gesture-specific ABI fields in a separately versioned change.

The CPU path and MediaPipe-like tracking loop remain operational after every step.

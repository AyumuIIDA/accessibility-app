# Hand Input Architecture

Status: architectural direction. This document is the source of truth for the
boundary between native hand technology and application features. It describes the
target structure and incremental migration rules; it does not claim that every
named component already exists.

The current implementation and C ABI are documented in
[native-vision-runtime.md](native-vision-runtime.md). Recognition algorithms and
their publication contract are detailed in
[gesture-recognition-framework.md](gesture-recognition-framework.md).

## Purpose

Hand tracking technology and application behavior change for different reasons.
Camera models, coordinate transforms, tracking quality, and measurement algorithms
must be replaceable without rewriting CAD or action behavior. Likewise, changing a
CAD sensitivity curve or interaction gate must not change hand perception.

The stable conceptual boundary is:

```text
Technology
  Camera / Geometry / Models
          |
          v
Domain facts
  Observation / Measurement
          |
          v
Interpretation
  State / Event recognition
          |
          v
Interaction
  Intent / Capture / Session
          |
          v
Feature
  CAD / Pointer / Registered actions
```

This is a small domain model, not a general gesture plugin framework. Add concrete
types when a demonstrated feature needs them. Do not create empty interfaces,
arbitrary dependency graphs, dynamic plugin loading, or generic feature registries
in anticipation of unknown use cases.

## Runtime flow

```text
CameraStage
  -> HandPerceptionStage
       MediaPipe-like palm / ROI / landmark tracking graph
  -> HandObservation
       validated result from one perception frame
  -> HandMeasurementStage
       typed, task-neutral measurements
  -> optional RecognitionStage
       persistent states and one-shot events
  -> publication
       latest measurements / latest states / ordered events

C# / WPF
  -> intent and configuration
       gate request, mode, sensitivity, presentation policy

C++ native
  -> InteractionSession
       capture, reference, suspend, release, cancel
  -> ApplicationBinding
       measurement or recognition result -> high-rate feature command
  -> Presentation
       shared consumer-side mirror/physical coordinate policy
  -> native CAD / pointer measurement adapter

C# / WPF
  -> status display / registered OS action dispatch
```

Recognition is optional. Direct manipulation should consume a measurement when no
semantic gesture classification is required:

Presentation is also separate from measurement. `HandInput/Measurements` publishes
one physically documented coordinate domain and never changes meaning with a UI
setting. Native feature bindings and native rendering apply the shared
`HandPresentationTransform` at their consumer boundaries. This keeps CAD and the 3D
hand model consistent without introducing presentation branches into perception or
duplicating coordinate arithmetic in C#.

```text
PalmRotationMeasurement
  -> Space-gated interaction session
  -> CAD orbit binding
  -> CadViewCommand
```

A symbolic motion uses recognition:

```text
ScreenPalmMeasurement history
  -> SwipeRecognizer
  -> SwipeLeft event
  -> PreviousPageCommand
```

## Domain concepts

### HandObservation

An observation describes what the perception graph reported for one source frame.
It shields downstream processing from model-runner tensor contracts but retains the
information needed to derive new measurements.

Typical data:

```text
frame ID and monotonic timestamp
presence and handedness
image landmarks
world landmarks
model confidence
orientation and tracking provenance
validity flags
```

An observation contains no application meaning. It must not contain CAD commands,
pointer actions, Open Palm interaction policy, or Windows input behavior.

### HandMeasurement

A measurement is a typed, task-neutral quantity derived from an observation or a
bounded sequence of observations.

Initial concrete measurements:

```text
PalmPoseMeasurement
  absolute orientation, center, scale

PalmRotationMeasurement
  reference-relative rotation, fit residual

ScreenPalmMeasurement
  image-space center, scale, delta, velocity

HandShapeMeasurement
  extension, curl, spread, pinch distance
```

Every published measurement has:

```text
frame ID
timestamp
validity
quality
documented coordinate system and unit
```

`quality` answers whether the value is trustworthy. It is not a gesture-class
confidence. Tracking loss produces an invalid measurement; it must never be encoded
as a valid zero.

Internally, measurements remain concrete C++ types. A generic ID plus `float[]` is
acceptable only at a stable ABI or persistence boundary where a descriptor defines
the exact schema.

### Recognition

Recognition gives semantic meaning to measurements. It is used only when a feature
needs a persistent condition or a temporal pattern.

Recognition publishes:

```text
State
  a condition that remains active
  examples: Open Palm active, Pinch active, Domain Sign active

Event
  a bounded occurrence recognized once
  examples: Swipe Left, Circle Complete, Wave
```

`Pose`, `Analog`, and `Sequence` may describe an algorithm internally, but are not
the public input taxonomy. A pinch can be both a State and the gate for a continuous
distance Measurement; a circle can be either a completed Event or a continuous
measurement depending on the feature.

Recognition must not call WPF, CAD, Windows input injection, or execution-provider
selection. See [gesture-recognition-framework.md](gesture-recognition-framework.md).

### InteractionSession

An interaction session represents application intent and capture. It is the
hand-input equivalent of pointer capture, not a perception algorithm.

Minimal lifecycle:

```text
Inactive
  -> Active
  -> Suspended
  -> Active
  -> Ended / Cancelled
```

Operations:

```text
Begin
Update
Suspend
Resume
End
Cancel
```

The session may retain:

```text
captured hand identity when available
reference frame and reference measurements
last accepted frame ID
tracking-loss start time
grace-period and cancellation state
```

An intent gate begins or ends a session. For the demo it may be a physical key. A
later binding may use a recognized State such as Open Palm. Changing the gate must
not require changes to palm rotation measurement or CAD view mapping.

The application owns logical session policy. When reference capture must initialize
a native estimator, WPF sends an explicit native command; the high-rate estimator
and its state remain native-owned.

### ApplicationBinding and Command

A binding maps domain facts to one feature. It owns application semantics such as
sensitivity, axes, clamping, mode selection, and command choice.

Examples:

```text
Space + relative palm rotation -> CAD orbit command
Shift+Space + palm translation -> CAD pan command
Ctrl+Space + palm scale ratio  -> CAD zoom command
Swipe Left event               -> previous-page command
```

A binding does not calculate landmarks, infer a pose, select a model provider, or
own camera buffers.

## Native and managed ownership

| Responsibility | Owner |
| --- | --- |
| Camera, ROI, inference, landmark projection | C++ |
| Per-frame validation and typed measurements | C++ |
| High-rate filtering and bounded landmark history | C++ |
| Recognition requiring per-frame landmarks | C++ |
| Latest small-value polling | C# |
| Application intent and user configuration | C# |
| High-rate capture/session state and continuous feature binding | C++ |
| Binding policy selection and status display | C# |
| Windows action execution | C# |
| Camera, landmark, and native CAD rendering | C++ |

This is a placement rule, not a demand for extra process boundaries. The C ABI
copies small metadata values. C# must not reprocess 21 landmarks every frame in the
target path or retain native image/tensor pointers.

## Publication and ABI direction

The delivery policy follows the semantic lifetime:

```text
Measurement  latest-only
State        latest-only plus lifecycle transition
Event        bounded ordered ring with a monotonic cursor
```

Do not put Events in a latest-only slot; an unpolled command could disappear. Do not
put every measurement update in an ordered queue; stale analog motion would build
latency.

The demo uses the version 14 hand result while boundaries are
extracted. A later ABI should separate:

```text
ryoiki_get_latest_measurements(...)
ryoiki_get_latest_states(...)
ryoiki_poll_events(...)
```

Do not increment the ABI merely to rename internal types. Increment it when the
native/managed binary contract actually changes.

For any fixed generic ABI value array, its descriptor must define:

```text
stable ID and key
value count and meaning
unit and coordinate system
absolute or reference-relative semantics
validity flags
recommended range
```

Internal C++ code should continue to use typed structures.

## Quality contract

Input quality is a first-class domain fact. It must not be inferred solely from a
recognizer confidence.

Measurements and recognizers should be able to gate on relevant factors such as:

```text
hand presence
landmark finite/bone consistency
temporal stability
screen coverage and clipping
palm incidence
tracking reacquisition age
rotation fit residual
world/image landmark availability
```

The exact factors remain measurement-specific. Do not build one universal quality
formula. Publish a useful aggregate quality plus explicit validity/reason flags.

Time-based hysteresis and grace periods are preferred over frame counts because
perception FPS can vary.

## Target code organization

This tree is directional. Create a directory only when moving or adding a real
implementation.

```text
src/RyoikiTenkai.Native/src/
  Buffers/
  Geometry/
  HandPerception/
    MediaPipeGraph/
    ModelRunners/
  HandInput/
    Observation/
    Measurements/
    Recognition/
    Publication/
  Presentation/
  Pipeline/
  Rendering/
  Runtime/
  camera_capture.*
  cad_runtime.cpp
  ryoiki_native.cpp

src/RyoikiTenkai.Wpf/
  Native/
  Interaction/
  Features/
    Cad/

src/RyoikiTenkai/
  Actions/
  Core/
  Storage/
```

The first responsibility-only migration has been completed:

```text
hand_measurement_extractor    -> HandInput/Measurements
weighted_palm_rotation_tracker -> HandInput/Measurements
palm_rotation_eskf             -> HandInput/Measurements
domain_expansion_state_recognizer -> HandInput/Recognition
```

The algorithms and thresholds were retained during that move. New code must use the
new locations; do not recreate a broad `Patterns` directory.

## Demo scope

Implement before or during the demo only when required:

```text
typed measurements used by a demonstrated feature
frame/time/validity/quality contract
native reference capture where required
Inactive / Active / Suspended session behavior
explicit CAD binding
replayable measurement records
```

Defer:

```text
dynamic gesture plugins
arbitrary feature dependency graphs
general model registries
full Pointer Events compatibility
user-editable universal binding UI
automatic recognizer conflict resolution
general multi-hand capture
large learned temporal models
automatic DTW segmentation before explicit-gate replay succeeds
```

## Incremental migration

1. **Implemented:** preserve current CAD behavior while moving
   measurement and recognition code to their responsibility directories.
2. **Implemented:** expose palm pose, relative rotation, screen center, screen
   scale, timestamp, validity, and quality as native typed measurements.
3. **Implemented:** move continuous CAD capture, coordinate conversion,
   sensitivity, filtering, and view mapping into native `CadHandBinding`. WPF
   supplies only intent and settings and polls a small status snapshot.
   Mirror/physical presentation is applied through the shared native
   `Presentation` layer and is also consumed by the native 3D hand plot.
4. **Implemented:** move Domain Sign behind a timed State recognizer and publish it
   through the generic latest-State ABI while retaining compatibility fields.
5. **Implemented:** add a native Open Palm State with measurement-quality gates
   and timed hysteresis. It is published independently from CAD; replacing the
   physical gate remains a separate binding-policy change.
6. **Implemented:** add Swipe Left/Right as the first temporal Event and publish
   it through a bounded ordered ring with a caller-owned sequence cursor.
7. Generalize an interface only after two concrete implementations demonstrate the
   same contract.

The CPU perception path and MediaPipe-like ROI loop must remain operational after
every step.

## Architecture invariants

All future hand-input work must preserve these rules:

1. Perception never depends on application features.
2. Measurements and recognition results are different concepts.
3. Absolute and reference-relative values are explicit.
4. Every result carries frame ID, timestamp, validity, and useful quality.
5. Tracking loss is invalid/suspended state, never a valid zero measurement.
6. Latest values and ordered events use different publication paths.
7. Application meaning lives in a binding, not a model runner or measurement.
8. High-frequency landmark processing remains native.
9. Internal data is typed; generic fixed arrays are boundary representations.
10. Coordinate systems, units, and value schemas are documented.
11. Recognition and interaction-session state machines remain separate.
12. New abstractions require a demonstrated second use or implementation.

These invariants are more important than the exact class or directory names.

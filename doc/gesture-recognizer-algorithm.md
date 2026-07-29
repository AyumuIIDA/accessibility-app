# Gesture Recognizer Algorithm

This document describes the current custom gesture recognizer used by the WPF prototype.

The recognizer is not a neural classifier. The neural model only produces hand landmarks. Gesture recognition is a deterministic, template-based algorithm over recorded 21-point hand skeletons.

## Pipeline

```text
camera frame
  -> hand perception model
  -> 21 hand landmarks + confidence + handedness + bbox
  -> per-frame feature extraction
  -> rolling-window candidate generation
  -> topology gates
  -> DTW template scoring
  -> confirmed gesture after stable consecutive matches
```

The raw 21-landmark skeleton remains the source of truth. Derived features are saved so the debug UI and recognizer inspect the same evidence.

## Recording

Each custom gesture needs 3 accepted takes. A take records every usable skeleton frame during the capture window.

The recording is rejected before becoming a template when:

- not enough frames have 21 landmarks
- confidence is too low
- duration is too short
- usable FPS is too low
- there is no meaningful topology change

The current meaningful-change signals are:

- palm movement through the frame
- palm axis rotation
- handedness range
- hand side / handedness mean
- hand-size change, such as approach or retreat from the camera
- translation direction and distance
- finger open/closed mask transitions
- continuous finger curl/straightness range
- palm-turn topology

For `grab`, the important signal is finger topology: open palm -> curled fingers/fist. It can pass either through boolean finger mask changes or through continuous finger straightness change.

## Feature Bank

For every usable frame, the extractor builds a feature vector and a readable feature record.

Core skeleton features:

- normalized 2D positions for all 21 landmarks
- bone shape distances
- fingertip-to-base distances
- fingertip spacing

Finger features:

- 5 continuous straightness values
- 5-bit finger mask
- finger transition count over time
- max finger straightness range over time

Palm-turn features:

- signed palm area from wrist, index MCP, pinky MCP
- signed-area crossing count
- palm compression from index MCP to pinky MCP span
- compression drop across the gesture
- landmark Z range
- bbox aspect ratio

Motion features:

- palm center trajectory
- palm velocity
- translation delta and distance
- active segment
- duration and FPS

Hand-side and size features:

- handedness score mean and range
- raw palm scale
- scale ratio relative to the first frame
- bbox area ratio relative to the first frame

Scale ratio is the main "hand approaching camera" signal. The normalized skeleton still makes shape matching robust across different starting distances, but the scale-ratio channel preserves whether the hand got larger or smaller during the gesture.

## Palm Flip

Palm flip is not detected by wrist travel. In the real recording, the wrist barely moved and the wrist-to-middle-finger angle only changed a little.

The actual palm-turn signature is:

```text
wide palm
  -> edge-on compressed hand
  -> wide palm again, opposite signed palm area
```

The recognizer checks:

- signed palm area crosses sides
- palm compression drops near the middle
- depth range increases near the edge-on frame
- finger mask remains compatible with the template

Handedness is only advisory. It is not trusted as the main palm-face/palm-back signal.

## Grab

Grab is expected to be:

```text
open palm
  -> fingers curl
  -> fist / closed hand
```

The recognizer checks:

- start/end finger masks differ, when the mask is clear
- or continuous finger straightness changes enough
- skeleton shape and bone distances match the recorded template
- palm-turn gates do not apply unless the recording also contains palm-turn topology

This prevents a stable open palm from recording as grab, and prevents palm flip from being confused with grab when fingers remain open.

## Matching

Recognition is always running on a rolling window.

When the native runtime reports two hands, the WPF layer feeds each visible hand into an independent recognizer bucket based on the handedness score. This prevents frames from two different hands from corrupting the same one-hand rolling window.

For true two-hand gestures, the app now records a frame set: both hand skeletons at the same timestamp. Those templates are marked `HandCount = 2` and use a separate two-hand recognizer. The one-hand recognizer ignores those templates.

The managed fallback also exposes a multi-hand detection path. Native is still the preferred real-time runtime, but managed inference no longer forces recognition down to a single hand.

Two-hand features include:

- low-handedness hand feature sequence
- high-handedness hand feature sequence
- relative X/Y vector between palm centers
- relative distance ratio between hands
- relative angle between hands

This lets the recognizer distinguish gestures like both hands moving together, apart, crossing, rotating around each other, or changing distance while each individual hand shape stays mostly the same.

Pairing is stabilized by choosing the two highest-confidence hands, then ordering by handedness when the handedness gap is clear. If handedness is ambiguous, the pair falls back to left-to-right palm-center ordering. A two-hand take that sees paired hands does not silently fall back to one-hand recording; it must pass two-hand quality gates or show a two-hand failure reason.

The recognizer builds candidate windows of several durations, extracts feature sequences, and compares them with saved templates.

Before scoring, topology gates reject impossible matches:

- palm-turn template requires palm-turn evidence
- finger-state template requires compatible finger state
- strong movement templates require enough movement
- hand-specific templates reject the opposite handedness bucket
- approach/retreat templates require enough hand-size change and the same size-change direction
- translation templates require enough translation and reject the opposite direction
- two-hand templates require two visible hands and reject opposite relative-distance direction
- opposite motion direction is rejected

After gates pass, the recognizer uses bounded Dynamic Time Warping over unified feature sequences. DTW allows the same gesture to be performed faster or slower while still matching the same shape over time.

Score parts are tracked separately:

- joints
- bones
- curl
- finger state
- spacing
- motion
- palm turn
- depth
- hand side
- size
- translation

The final confidence is derived from the DTW score. A match must remain stable for consecutive recognizer updates before triggering.

## Debugging

The debug UI records every skeleton frame and lets the user scrub it like video.

The inspector shows:

- raw skeleton
- finger mask and straightness
- palm area
- compression
- Z range
- bbox aspect
- scale and scale ratio
- bbox area ratio
- translation vector
- paired hand skeletons for two-hand frames
- topology summary
- DTW score and score breakdown
- rejection reason

The important debugging principle is: if a take fails or a live candidate does not match, the UI should show which feature gate or score part caused it.

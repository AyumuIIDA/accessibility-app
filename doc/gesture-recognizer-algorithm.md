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
- active segment
- duration and FPS

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

The recognizer builds candidate windows of several durations, extracts feature sequences, and compares them with saved templates.

Before scoring, topology gates reject impossible matches:

- palm-turn template requires palm-turn evidence
- finger-state template requires compatible finger state
- strong movement templates require enough movement
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
- topology summary
- DTW score and score breakdown
- rejection reason

The important debugging principle is: if a take fails or a live candidate does not match, the UI should show which feature gate or score part caused it.


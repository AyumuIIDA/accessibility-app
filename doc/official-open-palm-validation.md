# Official Open Palm Reference Validation

> Historical validation utility only. The production WPF application no longer
> starts the Python/LiteRT reference process and does not copy these tools to its
> output. Production perception and rendering are C++ native-only.

## Purpose

This is a development reference path, not the final production runtime. It verifies
that the existing hand-perception output can drive Google's official canned gesture
models and that `Open_Palm` can act as the CAD orbit clutch.

## Confirmed model contract

The official `gesture_recognizer.task` contains:

```text
gesture_embedder.tflite
  hand         float32 [1, 21, 3]
  handedness   float32 [1, 1]
  world_hand   float32 [1, 21, 3]
  -> embedding float32 [1, 128]

canned_gesture_classifier.tflite
  embedding    float32 [1, 128]
  -> scores    float32 [1, 8]
```

Score index 2 is `Open_Palm`. The complete order is:

```text
None, Closed_Fist, Open_Palm, Pointing_Up,
Thumb_Down, Thumb_Up, Victory, ILoveYou
```

ABI version 11 exposes the model's metric-scale world landmarks in addition to the
already normalized screen landmarks and handedness.

## Development setup

Download the official bundle to:

```text
src/RyoikiTenkai/models/gesture_recognizer.task
```

Create the isolated x64 reference environment on Windows ARM64:

```powershell
& 'C:\Users\hyena\AppData\Local\Programs\Python\Python311\python.exe' `
  -m venv .venv-official-gesture

.\.venv-official-gesture\Scripts\python.exe -m pip install ai-edge-litert
```

The x64 process runs under Windows ARM64 emulation. The application finds this
workspace-local environment automatically, or accepts:

```powershell
$env:RYOIKI_OFFICIAL_GESTURE_PYTHON = 'C:\path\to\python.exe'
```

Inspect the model without starting the app:

```powershell
.\.venv-official-gesture\Scripts\python.exe `
  src/RyoikiTenkai.Wpf/Tools/official_gesture_reference.py `
  --model src/RyoikiTenkai/models/gesture_recognizer.task `
  --inspect
```

## Runtime behavior

The WPF reference host sends only the latest landmark sample to the sidecar. The
channel capacity is one and drops stale samples. Results older than 15 perception
frames are not used for CAD control.

CAD orbit uses:

```text
enter Open Palm  winning class and confidence >= 0.50
maintain         winning class and confidence >= 0.40
```

The viewer displays the official label, winning confidence, Open Palm confidence,
and inference time. If the reference process is unavailable, the temporary geometry
detector remains as fallback.

Every development session records JSON Lines under `gesture-recordings/`:

```text
frame ID
handedness
21 normalized screen landmarks
21 metric-scale world landmarks
all eight official scores
winning label
Open Palm confidence
inference time
```

Set `RYOIKI_GESTURE_RECORD_PATH` to override the output file.

## Verified result

On 2026-07-27, the ARM64 WPF app, native ABI version 11, x64 LiteRT reference process,
official embedder, canned classifier, and CAD viewer were run together against the
live camera. The classifier produced a live result in approximately 0.8 ms and the
CAD clutch rejected a non-Open-Palm sample.

## Angle validation result

The labelled Open Palm session `official-open-palm-20260726-160007.jsonl`
contained 1,036 samples with landmarks. The official classifier selected
`Open_Palm` for 675 samples (65.2% conditional recall).

Palm incidence is measured from the world-landmark palm normal. Zero degrees
means that the palm plane faces the camera; 90 degrees means an edge-on palm.

| Palm incidence | Samples | Recall |
| --- | ---: | ---: |
| 0-15 degrees | 188 | 96.8% |
| 15-30 degrees | 363 | 89.3% |
| 30-45 degrees | 188 | 63.8% |
| 45-60 degrees | 101 | 46.5% |
| 60-75 degrees | 102 | 1.0% |
| 75-90 degrees | 94 | 1.1% |

This is conditional on landmark availability and therefore excludes tracking
losses. It demonstrates a sharp failure boundary when the palm becomes more
than about 45 degrees edge-on.

The official `SingleHandGestureRecognizerGraph` does not feed raw landmarks
directly to `gesture_embedder.tflite`. Its `LandmarksToMatrixCalculator`
corrects screen coordinates for image aspect ratio, applies input image
rotation, translates both landmark sets to landmark 0 (wrist), and divides x,
y, and z by the maximum x/y extent plus `1e-5`.

The reference bridge initially bypassed this graph preprocessing. Offline
replay with the official wrist/extent normalization raised conditional recall
only from 65.2% to approximately 66-67%, depending on the assumed camera aspect
ratio. Missing preprocessing is real but does not explain the edge-on collapse
by itself. The more important compatibility risk is that the classifier was
trained and evaluated on Google's MediaPipe Hands outputs, while this
application supplies landmarks from a separate MediaPipe-compatible model.
Landmark depth/world semantics and errors under self-occlusion are not
guaranteed to match.

Run the angle report with:

```powershell
.\.venv-official-gesture\Scripts\python.exe `
  src\RyoikiTenkai.Wpf\Tools\analyze_official_open_palm.py `
  gesture-recordings\<recording>.jsonl
```

Replay the official preprocessing variants with:

```powershell
.\.venv-official-gesture\Scripts\python.exe `
  src\RyoikiTenkai.Wpf\Tools\compare_official_preprocessing.py `
  gesture-recordings\<recording>.jsonl `
  --model src\RyoikiTenkai\models\gesture_recognizer.task
```

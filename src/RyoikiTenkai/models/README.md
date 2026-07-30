# RyoikiTenkai Models

Place MediaPipe Hands compatible model files here.

Expected development paths:

- `palm_detection.onnx`
- `hand_landmark.onnx`
- `gesture_recognizer.task` (official MediaPipe reference bundle)

The intended pipeline follows MediaPipe Hands:

1. palm detector / BlazePalm detects a hand ROI from the camera frame
2. hand landmark model predicts 21 hand landmarks from that ROI
3. downstream gesture recognizers consume normalized screen landmarks, metric-scale
   world landmarks, and handedness without reprocessing the image

The ONNX palm and hand-landmark runners are wired through ONNX Runtime. During
development, the official `.task` bundle is executed through the isolated LiteRT
reference process documented in
[`doc/official-open-palm-validation.md`](../../../doc/official-open-palm-validation.md).
This reference process is not the final native deployment path.

Official reference bundle:

```text
https://storage.googleapis.com/mediapipe-models/gesture_recognizer/gesture_recognizer/float16/latest/gesture_recognizer.task
```

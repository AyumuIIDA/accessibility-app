# RyoikiTenkai Models

Place MediaPipe Hands compatible model files here.

Expected development paths:

- `palm_detection.onnx`
- `hand_landmark.onnx`

The intended pipeline follows MediaPipe Hands:

1. palm detector / BlazePalm detects a hand ROI from the camera frame
2. hand landmark model predicts 21 hand landmarks from that ROI
3. `MediaPipeLandmarkGestureModel` maps landmarks to gesture ids such as `open_palm`, `fist`, and `pinch`

The current code has the pipeline boundary and landmark-based gesture classifier in place. The concrete model runner still needs to be wired through ONNX Runtime / Windows ML or a verified MediaPipe .NET binding.

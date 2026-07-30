# RyoikiTenkai Models

Place MediaPipe Hands compatible model files here.

Expected development paths:

- `palm_detection.onnx`
- `hand_landmark.onnx`

The intended pipeline follows MediaPipe Hands:

1. palm detector / BlazePalm detects a hand ROI from the camera frame
2. hand landmark model predicts 21 hand landmarks from that ROI
3. recorded custom gesture recognition consumes landmark windows

The WPF prototype uses recorded custom gesture templates only. Built-in static gesture ids are intentionally not part of the recognition path.

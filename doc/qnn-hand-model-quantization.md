# QNN Hand Model Quantization

## Model identity and numeric types

Three related model distributions must not be treated as the same artifact:

1. Legacy MediaPipe Hands publishes separate palm detection and hand landmark TFLite
   models. Its model list does not label or publish an INT8 Hands variant.
2. The current MediaPipe Gesture Recognizer documentation labels its packaged hand and
   gesture bundle as float16. That does not make the OpenCV Zoo ONNX files float16.
3. This repository currently uses OpenCV Zoo's February 2023 TFLite-to-ONNX conversions.
   Their graph inputs and outputs are float32 and the base artifact is the unquantized
   float model. Native preprocessing consequently produces float32 NHWC tensors in the
   `[0, 1]` range. Use `inspect_model.py` to record initializer types and Q/DQ counts
   for every exact model revision used in an experiment.

OpenCV Zoo publishes INT8 variants, but its hand pose model documentation warns that
the INT8 model may produce invalid results because of a significant accuracy drop.
Those artifacts are therefore not accepted as the QNN production models without the
same compatibility and accuracy gates defined below.

## What is quantized for QNN HTP

QNN HTP requires quantized fixed-shape models. The first candidate uses the ONNX
Runtime QNN recommendation:

```text
weights:              uint8
internal activations: uint16
graph input/output:   float32 with Q/DQ boundary nodes
preprocessing:        CPU float32
postprocessing:       CPU float32
ROI/tracking state:   CPU float32
ABI metadata:         float32
```

This is not full application-wide INT8 conversion. The neural graph is quantized while
geometry, confidence policy, NMS, landmark decoding, and MediaPipe ROI loopback retain
their current numeric contracts. Sixteen-bit activations are the initial accuracy-first
choice. An all-8-bit activation experiment is allowed only after this baseline passes.

QNN's mixed-precision QDQ support can promote sensitive activation regions to 16-bit.
Use that only when output comparison identifies a specific accuracy-sensitive region;
do not introduce per-layer overrides before measuring the uniform U16/U8 candidate.

## MediaPipe guidance and limits

There is no hand-specific MediaPipe statement that endorses post-training INT8 for the
published palm and landmark models. The official legacy list provides float Hands
models but explicitly calls out quantized variants for some other tasks. An open
MediaPipe issue asking how to quantize the float32 hand landmark model is still awaiting
an official response and is not guidance.

Google's general LiteRT guidance requires a representative calibration dataset to
estimate activation ranges. MediaPipe Model Maker documents QAT for INT8 object
detection and describes float16 post-training quantization as usually having a smaller
accuracy impact, but that guidance is not a validation result for Hands. Therefore this
project must establish its own landmark, tracking, and relocalization accuracy gates.

## Capture representative native tensors

Calibration data must come from the production geometry path so camera orientation,
letterboxing, ROI rotation, interpolation, RGB ordering, and normalization are exact.
Enable capture only for a deliberate calibration session:

```powershell
$env:RYOIKI_EXECUTION_PROVIDER = "cpu"
$env:RYOIKI_CALIBRATION_DIR = Join-Path (Get-Location) "tmp\qnn-calibration"
$env:RYOIKI_CALIBRATION_LIMIT = "1000"
$env:RYOIKI_CALIBRATION_PALM_INTERVAL = "10"
dotnet run --project src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj
```

The runner writes raw float32 NHWC tensors below `palm/` and `hand/`. Normal execution
does not open files or allocate capture buffers when `RYOIKI_CALIBRATION_DIR` is unset.
When `RYOIKI_CALIBRATION_PALM_INTERVAL` is a positive integer, the calibration run also
executes palm detection every N processed perception frames while landmark tracking is
active. The probe output is discarded, so it collects the production palm input without
replacing the tracked ROI or changing the normal MediaPipe-like loop. Leave the variable
unset during normal execution.

Collect varied lighting, backgrounds, skin tones, left/right hands, distance, rotation,
partial visibility, motion blur, and no-hand frames. Without the explicit interval,
palm tensors are produced only when the graph invokes palm fallback, so deliberately
make the hand enter and leave the frame and break/recover tracking. Even with the
interval enabled, a long sequence of one stable tracked hand is not a representative
palm calibration set.

Captured tensors retain low-resolution visual information. Keep them below ignored
`tmp/qnn-calibration/`, obtain consent where necessary, and do not commit or distribute
them as ordinary test fixtures.

## Generate the QDQ models

ORT's QNN quantizer must run in an x64 Python process, including on Windows ARM64.
Python 3.11 x64 is the conservative setup for the current toolchain:

```powershell
py -3.11-64 -m venv .venv-qnn-x64
.\.venv-qnn-x64\Scripts\python.exe -m pip install -r tools\qnn\requirements.txt

.\.venv-qnn-x64\Scripts\python.exe tools\qnn\inspect_model.py `
  src\RyoikiTenkai\models\palm_detection.onnx `
  src\RyoikiTenkai\models\hand_landmark.onnx

.\.venv-qnn-x64\Scripts\python.exe tools\qnn\quantize_models.py `
  --model-dir src\RyoikiTenkai\models `
  --calibration-dir tmp\qnn-calibration `
  --output-dir src\RyoikiTenkai\models
```

The generated files are `palm_detection_qdq.onnx` and
`hand_landmark_qdq.onnx`. The quantizer also writes hashes, precision choices, and
sample counts to `qnn-quantization-report.json`.

## Accuracy and runtime gates

First compare raw outputs with identical captured tensors:

```powershell
.\.venv-qnn-x64\Scripts\python.exe tools\qnn\compare_models.py `
  --model-dir src\RyoikiTenkai\models `
  --calibration-dir tmp\qnn-calibration `
  --output tmp\qnn-model-comparison.json
```

Do not split adjacent frames from one capture process between calibration and holdout.
Freeze captures by complete session and record every input hash first:

```powershell
.\.venv-qnn-x64\Scripts\python.exe tools\qnn\build_fixed_dataset.py `
  --capture-dir tmp\qnn-calibration `
  --output-dir tmp\qnn-fixed-evaluation `
  --holdout-session <capture-process-id>
```

The generated `dataset-manifest.json` records the fixed tensor contract, split session
IDs, relative paths, and SHA-256 for every sample. Pass the manifest to comparison so
the report also identifies the exact dataset and model binaries:

```powershell
.\.venv-qnn-x64\Scripts\python.exe tools\qnn\compare_models.py `
  --model-dir tmp\qnn-fixed-evaluation\models `
  --calibration-dir tmp\qnn-fixed-evaluation\holdout `
  --dataset-manifest tmp\qnn-fixed-evaluation\dataset-manifest.json `
  --output tmp\qnn-fixed-evaluation\accuracy-report.json
```

Raw-output comparison is necessary but not sufficient. Promotion to the runtime test
requires all of the following on a held-out sequence that was not used for calibration:

- QNN session creation succeeds with CPU EP fallback disabled.
- Palm detection recall and false-positive rate do not materially regress.
- Landmark pixel error, presence decisions, and handedness decisions remain acceptable.
- Tracking loss and palm relocalization frequency do not materially increase.
- Temporal landmark jitter does not materially increase for a stationary hand.
- End-to-end latency, CPU usage, and sustained background power improve over CPU.

Thresholds must be recorded with the evaluation dataset and use case. Do not accept a
model only because mean tensor error is small; palm threshold flips and a few inaccurate
landmarks can destabilize the ROI loopback for many later frames.

`compare_models.py` reports both raw-output error and task-facing decisions. The latter
include palm detection flips and top-anchor agreement, hand presence and handedness
flips, and 21-landmark XY error in the 224-pixel tensor coordinate system. Always pass
a holdout directory rather than reusing the calibration tensors for the final report.

## Preliminary Windows ARM64 result (2026-07-18)

The first local U8-weight/U16-activation experiment is a feasibility result, not a model
promotion. It used exact tensors captured after native preprocessing. Complete capture
sessions were isolated between splits; dataset manifest SHA-256 is
`02f12190e497d322770346416613b25ed41708a15d717b41844e3cca4e53e0f0`.
The hand split had 660 calibration and 1,000 holdout tensors. Palm capture was sparse
because the ROI loopback avoided detector execution; it had only 36 calibration and 7
holdout tensors.

Hand holdout results:

| Metric | Result |
| --- | ---: |
| Presence decision flip rate | 0.80% |
| Handedness decision flip rate | 0.00% |
| Landmark XY mean error | 3.63 px |
| Landmark XY p95 error | 7.70 px |
| Samples above 5 px landmark RMSE | 15.02% |
| Maximum per-sample landmark RMSE | 60.86 px |

The same fixed 1,000-tensor holdout was also evaluated against the model's
`Identity_3` world-landmark output. These values compare QDQ output with the float
model on identical native tensors; they are fidelity errors in model world units, not
absolute errors against measured 3D ground truth.

| World-landmark metric | Result |
| --- | ---: |
| X component MAE / p95 | 0.0140 / 0.0301 |
| Y component MAE / p95 | 0.0109 / 0.0269 |
| Z component MAE / p95 | 0.0119 / 0.0332 |
| 3D point error mean / p95 | 0.0249 / 0.0448 |
| Wrist-relative 3D point error mean / p95 | 0.0312 / 0.0621 |
| Float wrist-to-middle-MCP median scale | 0.0951 |
| Wrist-relative error / hand scale mean / p95 | 33.0% / 66.5% |
| Bone-length relative error mean / p95 | 60.9% / 174.2% |
| Consecutive-frame wrist-relative delta error mean / p95 | 0.00723 / 0.02026 |

The wrist-relative metric matches the 3D viewer's translation removal. Its error is too
large for 3D cursor mapping or CAD manipulation, and the frame-delta result predicts
additional visible jitter. This QDQ candidate must not be promoted for world-landmark
features even though its inference latency is substantially lower. The generated report
is `tmp/qnn-fixed-20260718/fixed-holdout-xyz-accuracy-report.json`.

Eight presence decisions changed: seven false negatives and one false positive. The
longest consecutive flip run was two samples (`904-000253` and `904-000254`). Samples
`904-000575` through `904-000577` contain the largest landmark errors, with presence
false negatives at the first and last sample. In the stateful graph, one false negative
can release tracking and trigger palm fallback, so the 0.8% aggregate rate is not
sufficient for promotion without fixed-sequence graph replay.

Palm accuracy remains inconclusive because only two of the seven holdout tensors crossed
the float model's detection threshold. More detector inputs, including positive palms,
are required before model promotion.

Sustained camera trials showed that HTP materially reduced inference time:

| Stage | CPU median | QNN HTP median | Median speedup |
| --- | ---: | ---: | ---: |
| Palm inference | 7.41 ms (4 samples) | 1.53 ms (9 samples) | 4.8x |
| Hand inference | 7.78 ms (56 samples) | 1.20 ms (52 samples) | 6.5x |
| End-to-end | 9.92 ms | 3.70 ms | 2.7x |

The HTP trial reported 20 perception drops before its first sampled result. Those drops
occurred while the perception worker created the QNN sessions concurrently with camera
startup; the count stayed constant during steady-state processing. Treat session warmup
and startup ordering as a separate runtime issue from per-frame NPU latency.

## Validated per-channel W8A16 result (2026-07-18)

Per-channel U8 convolution weights with per-tensor U16 activations removed most of the
uniform per-tensor candidate's hand-landmark error. QNN compatibility keeps MatMul/Gemm
weights per-tensor and retains float32 graph I/O. The final `enriched_v2` experiment
combined complete native CPU capture sessions into 3,950 hand tensors and 566 palm
tensors. One all-zero palm tensor from an earlier fallback capture was excluded.

The dedicated periodic-palm session used an interval of five processed perception
frames and produced 1,000 hand tensors and 410 palm tensors. Palm exact-duplicate rate
was 0.73%, median adjacent-frame input MAE was 0.0112, and no tensor was malformed or
all-zero. The periodic detector output was discarded; it did not replace the landmark
tracking ROI. The interval is ignored unless `RYOIKI_CALIBRATION_DIR` is also set.

The original session-isolated holdout remained unchanged: 1,000 hand inputs and seven
palm inputs. Results compare QDQ output with the float32 model, not human-annotated
ground truth.

| Metric | Enriched v1 | Enriched v2 |
| --- | ---: | ---: |
| Hand presence decision flip rate | 0.00% | 0.00% |
| Hand handedness decision flip rate | 0.00% | 0.00% |
| Hand landmark XY MAE | 0.55277 px | 0.55272 px |
| Hand landmark XY p95 | 1.32049 px | 1.31936 px |
| Palm detection decision flip rate | 0.00% | 0.00% |
| Palm top-score MAE | 0.01502 | 0.01435 |
| Palm selected-anchor regression MAE | 0.4952 | 0.4810 |

The seven-input palm holdout is too small for a promotion-quality recall or top-anchor
conclusion. A separate periodic-palm capture session must be reserved entirely for
holdout before making that claim.

Both v2 models created QNN HTP sessions and ran as W8A16 without CPU fallback in the
WinML performance path:

| Model | Mean | p50 | p95 |
| --- | ---: | ---: | ---: |
| Hand landmark | 1.746 ms | 1.613 ms | 2.518 ms |
| Palm detection | 2.975 ms | 2.515 ms | 6.251 ms |

ORT still warns that 16 hand-model biases exceed the int32 quantized range because the
bias scale is too small. The model passes current fidelity and NPU execution checks,
but this warning remains a tracked limitation for future mixed-precision or scale
experiments. Generated tensors, models, and JSON reports remain below ignored `tmp/`;
the reproducible code and commands are the committed artifacts.

## Primary references

- MediaPipe legacy Hands models and model card:
  <https://github.com/google-ai-edge/mediapipe/blob/master/docs/solutions/models.md#hands>
- MediaPipe Hands graph and model design:
  <https://github.com/google-ai-edge/mediapipe/blob/master/docs/solutions/hands.md>
- Current MediaPipe Gesture Recognizer model bundle:
  <https://ai.google.dev/edge/mediapipe/solutions/vision/gesture_recognizer>
- LiteRT representative calibration dataset:
  <https://ai.google.dev/edge/litert/api_docs/python/tf/lite/RepresentativeDataset>
- ONNX Runtime QNN HTP requirements and quantizer:
  <https://onnxruntime.ai/docs/execution-providers/QNN-ExecutionProvider.html>
- OpenCV Zoo hand pose conversion and INT8 warning:
  <https://huggingface.co/opencv/handpose_estimation_mediapipe>

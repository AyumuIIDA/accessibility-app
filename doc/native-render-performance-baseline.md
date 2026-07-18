# Native Render Performance Baseline

## Measurement

Measurement date: 2026-07-17

The ARM64 Release build ran the native camera, CPU ONNX Runtime perception, and
D3D11/DXGI/Direct2D preview for 35 seconds on the target PC. WPF sampled the latest
ABI metrics every 250 ms. The first 20 samples (approximately five seconds) were
discarded as warm-up.

Set `RYOIKI_NATIVE_METRICS_CSV` to enable CSV capture:

```powershell
$env:RYOIKI_NATIVE_METRICS_CSV = "$PWD\tmp\native-render-metrics.csv"
dotnet run --project src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj -c Release
```

The render subdivisions are CPU wall-clock measurements around Direct2D/DXGI API
calls. They measure caller-visible work and waits, not GPU timestamp-query durations.

## Stable Interval

The stable interval contained 83 samples before a later palm-reacquisition/inference
stall. Values are milliseconds.

| Metric | Mean | p50 | p95 | Max |
|---|---:|---:|---:|---:|
| Camera CPU frame copy | 0.502 | 0.437 | 0.538 | 2.856 |
| ROI crop/warp | 0.914 | 0.756 | 1.501 | 5.775 |
| Hand inference | 6.913 | 4.645 | 17.187 | 30.612 |
| Camera bitmap upload | 0.427 | 0.394 | 0.547 | 1.495 |
| Camera draw submission | 0.009 | 0.008 | 0.011 | 0.030 |
| Overlay draw submission | 0.024 | 0.021 | 0.039 | 0.134 |
| EndDraw | 0.294 | 0.246 | 0.503 | 2.103 |
| Present wait | 0.523 | 0.142 | 0.249 | 11.720 |
| Complete render | 1.277 | 0.830 | 1.376 | 12.455 |
| Capture-to-present | 15.593 | 13.067 | 28.971 | 50.224 |

The camera upload represented 48.6% of median render time but 3.0% of median
capture-to-present latency. Eliminating both the CPU frame copy and display upload
has a median direct-cost ceiling of approximately 0.83 ms in this configuration.
It can still prevent an occasional missed presentation deadline, so the benefit may
be larger than the arithmetic mean when it removes a synchronization spike.

## Inference Stall

The final part of the run repeatedly fell back to palm detection. Thirteen sampled
palm inference frames measured p50 24.920 ms, p95 83.035 ms, and max 156.707 ms.
Positive hand inference samples measured p50 6.097 ms, p95 30.215 ms, and max
158.794 ms. Capture-to-present consequently reached 451.480 ms while processing stale
source frames. This is independent of the normal 0.4-0.5 ms camera upload and is the
larger latency risk.

The next performance work should therefore:

1. Validate QNN HTP model compatibility and inference latency with CPU preprocessing.
2. Investigate CPU inference stalls and ensure stale work remains bounded.
3. Prototype DXGI camera capture only together with GPU preprocessing, avoiding a
   full-frame CPU readback.
4. Compare CPU and DXGI capture using sustained p50/p95 latency, drops, and power.

## D3D11 GPU-direct inference status

The current renderer does not provide a GPU-resident inference input. D3D11 owns the
display device and back buffer, but the capture path still requests RGB32, copies it
through `IMF2DBuffer::Lock2D` into the native CPU `FrameBuffer`, and the perception
geometry processor creates CPU float32 tensors. QNN HTP consumes those fixed-shape CPU
inputs. A D3D11 camera texture used by the renderer must therefore not be described as
zero-copy inference.

The current HTP trial provides this baseline (median, milliseconds):

| Stage | CPU/QNN HTP path |
|---|---:|
| Camera CPU frame copy | 0.4 |
| Camera bitmap upload to D3D11/Direct2D | 0.4 |
| Hand ROI crop/warp | 1.1 |
| Hand HTP inference | 1.2 |
| End-to-end | 3.7 |

The direct GPU-input experiment is **not measured yet**. It requires a separate capture
backend and runner contract. For an honest A/B result, both paths must report:

- camera surface type and memory location (`Cpu`, `D3D11 texture`, or shared surface)
- camera conversion/copy time
- GPU upload time and GPU preprocessing time
- GPU-to-CPU readback or tensor handoff time
- palm and hand inference time
- synchronization/fence wait time
- end-to-end latency, frame drops, and steady-state FPS
- CPU/GPU/NPU utilization, memory bandwidth, and package power

The GPU-native candidate is:

```text
Media Foundation DXGI surface
  -> ID3D11Texture2D
  -> GPU rotation/resize/ROI preprocessing
  -> a GPU-compatible inference runner
```

The last arrow is not satisfied by the current QNN HTP runner: its ORT session contract
currently accepts CPU float32 tensors. If GPU preprocessing must read back 192x192 and
224x224 tensors to CPU before QNN, the full-frame copy may disappear while a smaller
readback remains. That is a different experiment from true GPU-resident inference and
must be measured separately. DirectML is also a separate runner path; it should not be
silently substituted into the QNN graph.

## Limitations

- Hand motion and palm fallback were not controlled test inputs.
- CSV contains 4 Hz snapshots rather than every presented frame.
- GPU execution time and memory bandwidth require D3D timestamp queries and ETW/WPA.
- Package power, temperature, and NPU/GPU utilization were not captured in this run.
- Results apply to the current camera mode, CPU ONNX models, and target device only.

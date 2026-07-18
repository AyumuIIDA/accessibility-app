from __future__ import annotations

import argparse
import hashlib
import json
import platform
import tempfile
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
from onnxruntime.quantization import CalibrationDataReader, QuantType, quantize
from onnxruntime.quantization.execution_providers.qnn import (
    get_qnn_qdq_config,
    qnn_preprocess_model,
)


@dataclass(frozen=True)
class ModelSpec:
    name: str
    source_name: str
    output_name: str
    shape: tuple[int, ...]


MODEL_SPECS = (
    ModelSpec(
        "palm",
        "palm_detection.onnx",
        "palm_detection_qdq.onnx",
        (1, 192, 192, 3),
    ),
    ModelSpec(
        "hand",
        "hand_landmark.onnx",
        "hand_landmark_qdq.onnx",
        (1, 224, 224, 3),
    ),
)


class TensorDataReader(CalibrationDataReader):
    def __init__(self, model_path: Path, tensor_dir: Path, shape: tuple[int, ...]):
        session = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
        inputs = session.get_inputs()
        if len(inputs) != 1:
            raise ValueError(f"{model_path} must expose exactly one input")
        if tuple(inputs[0].shape) != shape or inputs[0].type != "tensor(float)":
            raise ValueError(
                f"{model_path} input must be float32 with fixed shape {shape}; "
                f"got {inputs[0].type} {inputs[0].shape}"
            )

        self.input_name = inputs[0].name
        self.shape = shape
        self.files = sorted(tensor_dir.glob("*.f32"))
        if not self.files:
            raise ValueError(f"No calibration tensors found in {tensor_dir}")
        self._iterator = iter(self.files)

    def get_next(self) -> dict[str, np.ndarray] | None:
        try:
            path = next(self._iterator)
        except StopIteration:
            return None

        tensor = np.fromfile(path, dtype=np.float32)
        expected = int(np.prod(self.shape))
        if tensor.size != expected:
            raise ValueError(f"{path} contains {tensor.size} floats; expected {expected}")
        return {self.input_name: tensor.reshape(self.shape)}

    def rewind(self) -> None:
        self._iterator = iter(self.files)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def quantize_model(
    spec: ModelSpec,
    model_dir: Path,
    calibration_dir: Path,
    output_dir: Path,
    per_channel: bool,
) -> dict[str, object]:
    source = model_dir / spec.source_name
    destination = output_dir / spec.output_name
    if not source.is_file():
        raise FileNotFoundError(source)

    reader = TensorDataReader(source, calibration_dir / spec.name, spec.shape)
    with tempfile.TemporaryDirectory(prefix=f"ryoiki-{spec.name}-") as temporary:
        preprocessed = Path(temporary) / f"{spec.name}.preprocessed.onnx"
        changed = qnn_preprocess_model(str(source), str(preprocessed))
        model_to_quantize = preprocessed if changed else source
        qnn_config = get_qnn_qdq_config(
            str(model_to_quantize),
            reader,
            activation_type=QuantType.QUInt16,
            weight_type=QuantType.QUInt8,
            per_channel=per_channel,
        )
        quantize(str(model_to_quantize), str(destination), qnn_config)

    onnx.checker.check_model(str(destination))
    session = ort.InferenceSession(str(destination), providers=["CPUExecutionProvider"])
    if tuple(session.get_inputs()[0].shape) != spec.shape:
        raise ValueError(f"Quantized {spec.name} model changed its input shape")
    return {
        "model": spec.name,
        "source": str(source),
        "output": str(destination),
        "calibration_samples": len(reader.files),
        "activation_type": "QUInt16",
        "weight_type": "QUInt8",
        "per_channel": per_channel,
        "sha256": sha256(destination),
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Quantize the Ryoiki hand models to QNN-compatible QDQ models."
    )
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--calibration-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--model",
        action="append",
        choices=[spec.name for spec in MODEL_SPECS],
        help="Quantize only the selected model. Repeat to select multiple models.",
    )
    parser.add_argument("--per-channel", action="store_true")
    args = parser.parse_args()

    if platform.machine().lower() not in {"amd64", "x86_64"}:
        raise RuntimeError(
            "ONNX Runtime QNN quantization must run under an x64 Python process."
        )

    args.output_dir.mkdir(parents=True, exist_ok=True)
    selected = set(args.model) if args.model else {spec.name for spec in MODEL_SPECS}
    reports = [
        quantize_model(
            spec,
            args.model_dir,
            args.calibration_dir,
            args.output_dir,
            args.per_channel,
        )
        for spec in MODEL_SPECS
        if spec.name in selected
    ]
    report_path = args.output_dir / "qnn-quantization-report.json"
    report_path.write_text(json.dumps(reports, indent=2) + "\n", encoding="utf-8")
    print(report_path)


if __name__ == "__main__":
    main()

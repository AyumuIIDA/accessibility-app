from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
from winml.modelkit.quant import WinMLQuantizationConfig, quantize_onnx


class NativeTensorCalibrationReader:
    def __init__(self, model_path: Path, tensor_dir: Path) -> None:
        session = ort.InferenceSession(
            str(model_path), providers=["CPUExecutionProvider"]
        )
        inputs = session.get_inputs()
        if len(inputs) != 1:
            raise ValueError(f"{model_path} must expose exactly one input")

        model_input = inputs[0]
        if model_input.type != "tensor(float)":
            raise ValueError(
                f"{model_path} input must be float32; got {model_input.type}"
            )
        if any(not isinstance(dimension, int) for dimension in model_input.shape):
            raise ValueError(
                f"{model_path} input must have a fixed shape; got {model_input.shape}"
            )

        self.input_name = model_input.name
        self.shape = tuple(int(dimension) for dimension in model_input.shape)
        self.files = sorted(path.resolve() for path in tensor_dir.glob("*.f32"))
        if not self.files:
            raise ValueError(f"No calibration tensors found in {tensor_dir}")

        expected_bytes = int(np.prod(self.shape)) * np.dtype(np.float32).itemsize
        for path in self.files:
            if path.stat().st_size != expected_bytes:
                raise ValueError(
                    f"{path} has {path.stat().st_size} bytes; expected "
                    f"{expected_bytes} for {self.shape}"
                )
        self._iterator = iter(self.files)

    def get_next(self) -> dict[str, np.ndarray] | None:
        try:
            path = next(self._iterator)
        except StopIteration:
            return None
        tensor = np.fromfile(path, dtype=np.float32).reshape(self.shape)
        return {self.input_name: tensor}

    def rewind(self) -> None:
        self._iterator = iter(self.files)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Quantize an ONNX model with Foundry Toolkit's WinML quantizer "
            "and native production-path calibration tensors."
        )
    )
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--calibration-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument(
        "--per-channel",
        action="store_true",
        help="Quantize supported weights with a separate scale per output channel.",
    )
    args = parser.parse_args()

    args.model = args.model.resolve()
    args.calibration_dir = args.calibration_dir.resolve()
    args.output = args.output.resolve()
    args.report = args.report.resolve()

    if not args.model.is_file():
        raise FileNotFoundError(args.model)

    reader = NativeTensorCalibrationReader(args.model, args.calibration_dir)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    config = WinMLQuantizationConfig(
        mode="qdq",
        samples=len(reader.files),
        calibration_method="minmax",
        calibration_data=reader,
        weight_type="uint8",
        activation_type="uint16",
        per_channel=args.per_channel,
        symmetric=False,
    )
    result = quantize_onnx(args.model, args.output, config=config)
    if not result.success or result.output_path is None:
        raise RuntimeError("; ".join(result.errors) or "Toolkit quantization failed")

    onnx.checker.check_model(str(result.output_path))
    quantized = onnx.load(str(result.output_path), load_external_data=False)
    operation_counts: dict[str, int] = {}
    for node in quantized.graph.node:
        operation_counts[node.op_type] = operation_counts.get(node.op_type, 0) + 1

    report = {
        "tool": "Foundry Toolkit WinML modelkit.quant.quantize_onnx",
        "source_model": str(args.model),
        "source_model_sha256": sha256(args.model),
        "output_model": str(result.output_path),
        "output_model_sha256": sha256(result.output_path),
        "calibration_directory": str(args.calibration_dir),
        "calibration_samples": len(reader.files),
        "input_name": reader.input_name,
        "input_shape": list(reader.shape),
        "input_dtype": "float32",
        "calibration_method": "minmax",
        "weight_type": "uint8",
        "activation_type": "uint16",
        "per_channel": args.per_channel,
        "quantize_linear_nodes": operation_counts.get("QuantizeLinear", 0),
        "dequantize_linear_nodes": operation_counts.get("DequantizeLinear", 0),
        "timing_seconds": {
            "calibration": result.calibration_time_seconds,
            "qdq_insertion": result.qdq_insertion_time_seconds,
            "postprocess": result.postproc_time_seconds,
            "total": result.total_time_seconds,
        },
        "warnings": result.warnings,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(args.output)
    print(args.report)


if __name__ == "__main__":
    main()

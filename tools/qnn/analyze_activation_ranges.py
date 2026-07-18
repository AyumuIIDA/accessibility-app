from __future__ import annotations

import argparse
import json
import tempfile
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
from onnx import TensorProto, helper, numpy_helper, shape_inference


def tensor_type(model: onnx.ModelProto, name: str) -> onnx.ValueInfoProto | None:
    for value in [*model.graph.value_info, *model.graph.input, *model.graph.output]:
        if value.name == name:
            return value
    return None


def quantization_ranges(qdq_path: Path) -> dict[str, tuple[float, float]]:
    model = onnx.load(str(qdq_path), load_external_data=True)
    initializers = {item.name: numpy_helper.to_array(item) for item in model.graph.initializer}
    ranges: dict[str, tuple[float, float]] = {}
    for node in model.graph.node:
        if node.op_type != "QuantizeLinear" or len(node.input) < 3:
            continue
        tensor_name, scale_name, zero_point_name = node.input[:3]
        scale = float(np.asarray(initializers[scale_name]).reshape(-1)[0])
        zero_point_array = np.asarray(initializers[zero_point_name])
        zero_point = int(zero_point_array.reshape(-1)[0])
        if zero_point_array.dtype == np.uint16:
            quantized_min, quantized_max = 0, 65535
        elif zero_point_array.dtype == np.uint8:
            quantized_min, quantized_max = 0, 255
        elif zero_point_array.dtype == np.int16:
            quantized_min, quantized_max = -32768, 32767
        elif zero_point_array.dtype == np.int8:
            quantized_min, quantized_max = -128, 127
        else:
            continue
        ranges[tensor_name] = (
            (quantized_min - zero_point) * scale,
            (quantized_max - zero_point) * scale,
        )
    return ranges


def create_diagnostic_model(
    float_path: Path, candidate_names: set[str], destination: Path
) -> list[str]:
    model = onnx.load(str(float_path), load_external_data=True)
    model = shape_inference.infer_shapes(model)
    produced = {output for node in model.graph.node for output in node.output}
    existing_outputs = {output.name for output in model.graph.output}
    names: list[str] = []
    for name in sorted(candidate_names & produced):
        if name in existing_outputs:
            names.append(name)
            continue
        value = tensor_type(model, name)
        if value is None:
            model.graph.output.append(helper.make_tensor_value_info(name, TensorProto.FLOAT, None))
        else:
            model.graph.output.append(value)
        names.append(name)
    onnx.save(model, str(destination))
    return names


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Find holdout activations outside QDQ calibration ranges."
    )
    parser.add_argument("--float-model", type=Path, required=True)
    parser.add_argument("--qdq-model", type=Path, required=True)
    parser.add_argument("--holdout-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    ranges = quantization_ranges(args.qdq_model)
    with tempfile.TemporaryDirectory(prefix="ryoiki-activation-ranges-") as temporary:
        diagnostic_path = Path(temporary) / "diagnostic.onnx"
        names = create_diagnostic_model(args.float_model, set(ranges), diagnostic_path)
        session = ort.InferenceSession(
            str(diagnostic_path), providers=["CPUExecutionProvider"]
        )
        input_info = session.get_inputs()[0]
        shape = tuple(int(value) for value in input_info.shape)
        files = sorted(args.holdout_dir.glob("*.f32"))
        if not files:
            raise ValueError(f"No holdout tensors found in {args.holdout_dir}")

        stats = {
            name: {
                "below": 0,
                "above": 0,
                "elements": 0,
                "affected_samples": 0,
                "worst_sample": None,
                "worst_fraction": 0.0,
            }
            for name in names
        }
        for path in files:
            tensor = np.fromfile(path, dtype=np.float32).reshape(shape)
            values = session.run(names, {input_info.name: tensor})
            for name, value in zip(names, values, strict=True):
                lower, upper = ranges[name]
                below = int(np.count_nonzero(value < lower))
                above = int(np.count_nonzero(value > upper))
                count = int(value.size)
                affected = below + above
                item = stats[name]
                item["below"] += below
                item["above"] += above
                item["elements"] += count
                if affected:
                    item["affected_samples"] += 1
                    fraction = affected / count
                    if fraction > item["worst_fraction"]:
                        item["worst_fraction"] = fraction
                        item["worst_sample"] = path.name

    results = []
    for name, item in stats.items():
        lower, upper = ranges[name]
        outside = item["below"] + item["above"]
        results.append(
            {
                "tensor": name,
                "quantized_range": [lower, upper],
                "outside_element_rate": outside / item["elements"],
                "below_count": item["below"],
                "above_count": item["above"],
                "affected_sample_rate": item["affected_samples"] / len(files),
                "worst_sample": item["worst_sample"],
                "worst_sample_outside_rate": item["worst_fraction"],
            }
        )
    results.sort(key=lambda item: item["outside_element_rate"], reverse=True)
    report = {
        "float_model": str(args.float_model),
        "qdq_model": str(args.qdq_model),
        "holdout_samples": len(files),
        "analyzed_activation_boundaries": len(results),
        "activations": results,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(args.output)


if __name__ == "__main__":
    main()

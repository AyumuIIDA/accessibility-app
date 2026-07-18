from __future__ import annotations

import argparse
import json
import tempfile
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
from onnx import TensorProto, helper, shape_inference


def add_outputs(
    model: onnx.ModelProto, names: list[str]
) -> onnx.ModelProto:
    model = shape_inference.infer_shapes(model)
    values = {
        value.name: value
        for value in [*model.graph.value_info, *model.graph.input, *model.graph.output]
    }
    existing = {output.name for output in model.graph.output}
    for name in names:
        if name in existing:
            continue
        value = values.get(name)
        model.graph.output.append(
            value
            if value is not None
            else helper.make_tensor_value_info(name, TensorProto.FLOAT, None)
        )
    return model


def activation_pairs(
    float_model: onnx.ModelProto, qdq_model: onnx.ModelProto
) -> list[tuple[int, str, str]]:
    float_order = {
        output: index
        for index, node in enumerate(float_model.graph.node)
        for output in node.output
    }
    dequantized_by_quantized = {
        node.input[0]: node.output[0]
        for node in qdq_model.graph.node
        if node.op_type == "DequantizeLinear"
    }
    pairs: list[tuple[int, str, str]] = []
    for node in qdq_model.graph.node:
        if node.op_type != "QuantizeLinear":
            continue
        float_name = node.input[0]
        qdq_name = dequantized_by_quantized.get(node.output[0])
        if float_name in float_order and qdq_name is not None:
            pairs.append((float_order[float_name], float_name, qdq_name))
    pairs.sort()
    return pairs


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Locate layer-wise error growth between float and QDQ models."
    )
    parser.add_argument("--float-model", type=Path, required=True)
    parser.add_argument("--qdq-model", type=Path, required=True)
    parser.add_argument("--tensor-dir", type=Path, required=True)
    parser.add_argument("--sample", action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    float_model = onnx.load(str(args.float_model), load_external_data=True)
    qdq_model = onnx.load(str(args.qdq_model), load_external_data=True)
    pairs = activation_pairs(float_model, qdq_model)
    float_names = [pair[1] for pair in pairs]
    qdq_names = [pair[2] for pair in pairs]

    with tempfile.TemporaryDirectory(prefix="ryoiki-intermediate-errors-") as temporary:
        float_diagnostic = Path(temporary) / "float.onnx"
        qdq_diagnostic = Path(temporary) / "qdq.onnx"
        onnx.save(add_outputs(float_model, float_names), str(float_diagnostic))
        onnx.save(add_outputs(qdq_model, qdq_names), str(qdq_diagnostic))
        float_session = ort.InferenceSession(
            str(float_diagnostic), providers=["CPUExecutionProvider"]
        )
        qdq_session = ort.InferenceSession(
            str(qdq_diagnostic), providers=["CPUExecutionProvider"]
        )
        input_info = float_session.get_inputs()[0]
        shape = tuple(int(value) for value in input_info.shape)

        samples: dict[str, list[dict[str, object]]] = {}
        for sample in args.sample:
            path = args.tensor_dir / sample
            tensor = np.fromfile(path, dtype=np.float32).reshape(shape)
            expected = float_session.run(float_names, {input_info.name: tensor})
            actual = qdq_session.run(qdq_names, {input_info.name: tensor})
            metrics = []
            for (order, name, _), float_value, qdq_value in zip(
                pairs, expected, actual, strict=True
            ):
                difference = qdq_value.astype(np.float32) - float_value.astype(np.float32)
                rmse = float(np.sqrt(np.mean(np.square(difference))))
                reference_rms = float(np.sqrt(np.mean(np.square(float_value))))
                metrics.append(
                    {
                        "node_order": order,
                        "tensor": name,
                        "rmse": rmse,
                        "relative_rmse": rmse / max(reference_rms, 1.0e-8),
                        "maximum_absolute_error": float(np.max(np.abs(difference))),
                    }
                )
            samples[sample] = metrics

    report = {
        "float_model": str(args.float_model),
        "qdq_model": str(args.qdq_model),
        "activation_pairs": len(pairs),
        "samples": samples,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(args.output)


if __name__ == "__main__":
    main()

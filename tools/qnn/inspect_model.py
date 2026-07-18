from __future__ import annotations

import argparse
import collections
import json
from pathlib import Path

import onnx


def tensor_type(value_info: onnx.ValueInfoProto) -> str:
    return onnx.TensorProto.DataType.Name(value_info.type.tensor_type.elem_type)


def main() -> None:
    parser = argparse.ArgumentParser(description="Report ONNX numeric and QDQ contracts.")
    parser.add_argument("models", nargs="+", type=Path)
    args = parser.parse_args()

    reports = []
    for path in args.models:
        model = onnx.load(str(path), load_external_data=False)
        initializer_types = collections.Counter(
            onnx.TensorProto.DataType.Name(initializer.data_type)
            for initializer in model.graph.initializer
        )
        operators = collections.Counter(node.op_type for node in model.graph.node)
        reports.append(
            {
                "path": str(path),
                "inputs": {value.name: tensor_type(value) for value in model.graph.input},
                "outputs": {value.name: tensor_type(value) for value in model.graph.output},
                "initializer_types": dict(initializer_types),
                "quantize_linear_nodes": operators["QuantizeLinear"],
                "dequantize_linear_nodes": operators["DequantizeLinear"],
            }
        )
    print(json.dumps(reports, indent=2))


if __name__ == "__main__":
    main()

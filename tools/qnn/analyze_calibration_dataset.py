from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
import onnxruntime as ort


MODEL_SHAPES = {
    "hand": (224, 224, 3),
    "palm": (192, 192, 3),
}


def percentiles(values: list[float]) -> dict[str, float]:
    if not values:
        return {}
    result = np.percentile(values, [0, 5, 25, 50, 75, 95, 100])
    return dict(zip(("min", "p05", "p25", "median", "p75", "p95", "max"), map(float, result)))


def analyze(directory: Path, shape: tuple[int, int, int]) -> dict[str, object]:
    files = sorted(directory.glob("*.f32"))
    expected = int(np.prod(shape))
    hashes: list[str] = []
    tensor_means: list[float] = []
    tensor_stds: list[float] = []
    tensor_mins: list[float] = []
    tensor_maxes: list[float] = []
    adjacent_mae: list[float] = []
    coarse_features: list[np.ndarray] = []
    invalid_files: list[str] = []
    previous: np.ndarray | None = None

    for path in files:
        tensor = np.fromfile(path, dtype=np.float32)
        if tensor.size != expected or not np.isfinite(tensor).all():
            invalid_files.append(path.name)
            continue

        hashes.append(hashlib.sha256(tensor.tobytes()).hexdigest())
        tensor_means.append(float(tensor.mean()))
        tensor_stds.append(float(tensor.std()))
        tensor_mins.append(float(tensor.min()))
        tensor_maxes.append(float(tensor.max()))
        if previous is not None:
            adjacent_mae.append(float(np.mean(np.abs(tensor - previous))))
        previous = tensor

        image = tensor.reshape(shape)
        block_height = shape[0] // 16
        block_width = shape[1] // 16
        coarse = image.reshape(16, block_height, 16, block_width, 3).mean(axis=(1, 3))
        coarse_features.append(coarse.ravel())

    unique_count = len(set(hashes))
    valid_count = len(hashes)
    feature_array = np.asarray(coarse_features)
    return {
        "files": len(files),
        "valid": valid_count,
        "invalid_files": invalid_files,
        "exact_unique": unique_count,
        "exact_duplicate_rate": (
            (valid_count - unique_count) / valid_count if valid_count else 0.0
        ),
        "tensor_mean": percentiles(tensor_means),
        "tensor_std": percentiles(tensor_stds),
        "tensor_min": percentiles(tensor_mins),
        "tensor_max": percentiles(tensor_maxes),
        "adjacent_frame_mae": percentiles(adjacent_mae),
        "coarse_feature_std_mean": (
            float(feature_array.std(axis=0).mean()) if len(feature_array) else None
        ),
    }


def analyze_hand_outputs(directory: Path, model_path: Path) -> dict[str, object]:
    session = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    input_name = session.get_inputs()[0].name
    presence: list[float] = []
    handedness: list[float] = []
    centers_x: list[float] = []
    centers_y: list[float] = []
    palm_scales: list[float] = []
    orientations: list[float] = []
    spans: list[float] = []

    for path in sorted(directory.glob("*.f32")):
        tensor = np.fromfile(path, dtype=np.float32).reshape(1, 224, 224, 3)
        landmarks_raw, presence_raw, handedness_raw, _ = session.run(None, {input_name: tensor})
        score = float(presence_raw.item())
        presence.append(score)
        if score < 0.5:
            continue
        handedness.append(float(handedness_raw.item()))
        points = landmarks_raw.reshape(21, 3)
        xy = points[:, :2] / 224.0
        centers_x.append(float(xy[:, 0].mean()))
        centers_y.append(float(xy[:, 1].mean()))
        palm_vector = xy[9] - xy[0]
        palm_scales.append(float(np.linalg.norm(palm_vector)))
        orientations.append(float(np.degrees(np.arctan2(palm_vector[1], palm_vector[0]))))
        spans.append(float(np.linalg.norm(xy.max(axis=0) - xy.min(axis=0))))

    detected = len(handedness)
    return {
        "samples": len(presence),
        "detected": detected,
        "detected_rate": detected / len(presence) if presence else 0.0,
        "left_rate_at_0_5_threshold": (
            sum(value < 0.5 for value in handedness) / detected if detected else 0.0
        ),
        "presence": percentiles(presence),
        "handedness": percentiles(handedness),
        "landmark_center_x": percentiles(centers_x),
        "landmark_center_y": percentiles(centers_y),
        "wrist_to_middle_mcp_scale": percentiles(palm_scales),
        "wrist_to_middle_mcp_angle_degrees": percentiles(orientations),
        "landmark_bbox_diagonal": percentiles(spans),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Analyze native calibration tensor diversity.")
    parser.add_argument("--dataset", action="append", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--hand-model", type=Path)
    args = parser.parse_args()

    report = {
        str(root): {
            model: analyze(root / model, shape)
            for model, shape in MODEL_SHAPES.items()
        }
        for root in args.dataset
    }
    if args.hand_model:
        for root in args.dataset:
            report[str(root)]["hand_model_outputs"] = analyze_hand_outputs(
                root / "hand", args.hand_model
            )
    rendered = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
        print(args.output)
    else:
        print(rendered, end="")


if __name__ == "__main__":
    main()

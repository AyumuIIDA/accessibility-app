from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import onnxruntime as ort

from quantize_models import MODEL_SPECS, sha256

PALM_SCORE_THRESHOLD = 0.35
HAND_PRESENCE_THRESHOLD = 0.5
HANDEDNESS_THRESHOLD = 0.5
HAND_CONNECTIONS = (
    (0, 1), (1, 2), (2, 3), (3, 4),
    (0, 5), (5, 6), (6, 7), (7, 8),
    (0, 9), (9, 10), (10, 11), (11, 12),
    (0, 13), (13, 14), (14, 15), (15, 16),
    (0, 17), (17, 18), (18, 19), (19, 20),
    (5, 9), (9, 13), (13, 17),
)


def sigmoid(values: np.ndarray) -> np.ndarray:
    positive = values >= 0
    result = np.empty_like(values, dtype=np.float32)
    result[positive] = 1.0 / (1.0 + np.exp(-values[positive]))
    exponential = np.exp(values[~positive])
    result[~positive] = exponential / (1.0 + exponential)
    return result


def summarize(values: np.ndarray) -> dict[str, float]:
    return {
        "mae": float(np.mean(values)),
        "rmse": float(np.sqrt(np.mean(np.square(values)))),
        "p95_absolute_error": float(np.percentile(values, 95)),
        "maximum_absolute_error": float(np.max(values)),
    }


def summarize_palm_decisions(
    expected_outputs: list[list[np.ndarray]],
    actual_outputs: list[list[np.ndarray]],
    sample_names: list[str],
) -> dict[str, object]:
    expected_regressions, expected_logits = expected_outputs
    actual_regressions, actual_logits = actual_outputs
    detection_flips = 0
    top_anchor_matches = 0
    detected_samples = 0
    top_score_errors: list[float] = []
    selected_regression_errors: list[np.ndarray] = []
    detection_flip_samples: list[dict[str, object]] = []

    for sample_name, expected_boxes, expected_scores_raw, actual_boxes, actual_scores_raw in zip(
        sample_names,
        expected_regressions,
        expected_logits,
        actual_regressions,
        actual_logits,
        strict=True,
    ):
        expected_scores = sigmoid(expected_scores_raw.reshape(-1))
        actual_scores = sigmoid(actual_scores_raw.reshape(-1))
        expected_top = int(np.argmax(expected_scores))
        actual_top = int(np.argmax(actual_scores))
        expected_detected = bool(expected_scores[expected_top] >= PALM_SCORE_THRESHOLD)
        actual_detected = bool(actual_scores[actual_top] >= PALM_SCORE_THRESHOLD)
        detection_flips += int(expected_detected != actual_detected)
        if expected_detected != actual_detected:
            detection_flip_samples.append(
                {
                    "sample": sample_name,
                    "baseline_detected": expected_detected,
                    "candidate_detected": actual_detected,
                    "baseline_top_score": float(expected_scores[expected_top]),
                    "candidate_top_score": float(actual_scores[actual_top]),
                }
            )
        top_anchor_matches += int(expected_top == actual_top)
        top_score_errors.append(
            abs(float(expected_scores[expected_top]) - float(actual_scores[expected_top]))
        )
        if expected_detected:
            detected_samples += 1
            selected_regression_errors.append(
                np.abs(
                    expected_boxes.reshape(-1, 18)[expected_top]
                    - actual_boxes.reshape(-1, 18)[expected_top]
                )
            )

    sample_count = len(expected_regressions)
    result: dict[str, object] = {
        "score_threshold": PALM_SCORE_THRESHOLD,
        "baseline_detected_samples": detected_samples,
        "sample_detection_flip_rate": detection_flips / sample_count,
        "top_anchor_match_rate": top_anchor_matches / sample_count,
        "top_score_mae": float(np.mean(top_score_errors)),
        "detection_flip_samples": detection_flip_samples,
    }
    if selected_regression_errors:
        selected = np.concatenate(selected_regression_errors)
        result["selected_anchor_regression_mae"] = float(np.mean(selected))
        result["selected_anchor_regression_p95"] = float(np.percentile(selected, 95))
    return result


def summarize_hand_decisions(
    expected_outputs: list[list[np.ndarray]],
    actual_outputs: list[list[np.ndarray]],
    sample_names: list[str],
) -> dict[str, object]:
    expected_landmarks = expected_outputs[0]
    actual_landmarks = actual_outputs[0]
    expected_presence = expected_outputs[1]
    actual_presence = actual_outputs[1]
    expected_handedness = expected_outputs[2]
    actual_handedness = actual_outputs[2]
    expected_world_landmarks = expected_outputs[3]
    actual_world_landmarks = actual_outputs[3]
    presence_flips = 0
    presence_false_negatives = 0
    presence_false_positives = 0
    current_flip_run = 0
    longest_flip_run = 0
    handedness_flips = 0
    detected_samples = 0
    landmark_point_errors: list[np.ndarray] = []
    sample_landmark_rmses: list[tuple[str, float]] = []
    world_component_errors: list[np.ndarray] = []
    world_point_errors: list[np.ndarray] = []
    relative_world_point_errors: list[np.ndarray] = []
    normalized_relative_world_point_errors: list[np.ndarray] = []
    world_hand_scales: list[float] = []
    bone_length_absolute_errors: list[np.ndarray] = []
    bone_length_relative_errors: list[np.ndarray] = []
    sample_relative_world_rmses: list[tuple[str, float]] = []
    relative_world_delta_errors: list[np.ndarray] = []
    presence_flip_samples: list[dict[str, object]] = []
    previous_sample_prefix: str | None = None
    previous_sample_index: int | None = None
    previous_expected_relative_world: np.ndarray | None = None
    previous_actual_relative_world: np.ndarray | None = None

    for (
        sample_name,
        expected_points_raw,
        actual_points_raw,
        expected_p,
        actual_p,
        expected_h,
        actual_h,
        expected_world_raw,
        actual_world_raw,
    ) in zip(
        sample_names,
        expected_landmarks,
        actual_landmarks,
        expected_presence,
        actual_presence,
        expected_handedness,
        actual_handedness,
        expected_world_landmarks,
        actual_world_landmarks,
        strict=True,
    ):
        expected_detected = bool(expected_p.item() >= HAND_PRESENCE_THRESHOLD)
        actual_detected = bool(actual_p.item() >= HAND_PRESENCE_THRESHOLD)
        presence_flips += int(expected_detected != actual_detected)
        if expected_detected != actual_detected:
            presence_false_negatives += int(expected_detected and not actual_detected)
            presence_false_positives += int(not expected_detected and actual_detected)
            current_flip_run += 1
            longest_flip_run = max(longest_flip_run, current_flip_run)
            presence_flip_samples.append(
                {
                    "sample": sample_name,
                    "baseline_presence": float(expected_p.item()),
                    "candidate_presence": float(actual_p.item()),
                }
            )
        else:
            current_flip_run = 0
        if not expected_detected:
            continue

        detected_samples += 1
        handedness_flips += int(
            bool(expected_h.item() >= HANDEDNESS_THRESHOLD)
            != bool(actual_h.item() >= HANDEDNESS_THRESHOLD)
        )
        expected_points = expected_points_raw.reshape(21, 3)
        actual_points = actual_points_raw.reshape(21, 3)
        point_errors = np.linalg.norm(expected_points[:, :2] - actual_points[:, :2], axis=1)
        landmark_point_errors.append(point_errors)
        sample_landmark_rmses.append(
            (sample_name, float(np.sqrt(np.mean(np.square(point_errors)))))
        )

        expected_world = expected_world_raw.reshape(21, 3)
        actual_world = actual_world_raw.reshape(21, 3)
        world_difference = actual_world - expected_world
        world_component_errors.append(np.abs(world_difference))
        world_point_errors.append(np.linalg.norm(world_difference, axis=1))
        expected_relative_world = expected_world - expected_world[0]
        actual_relative_world = actual_world - actual_world[0]
        relative_difference = actual_relative_world - expected_relative_world
        relative_point_error = np.linalg.norm(relative_difference, axis=1)
        relative_world_point_errors.append(relative_point_error)
        hand_scale = float(np.linalg.norm(expected_world[9] - expected_world[0]))
        if hand_scale > 1.0e-6:
            world_hand_scales.append(hand_scale)
            normalized_relative_world_point_errors.append(
                relative_point_error / hand_scale
            )
        expected_bone_lengths = np.asarray(
            [
                np.linalg.norm(expected_world[end] - expected_world[start])
                for start, end in HAND_CONNECTIONS
            ]
        )
        actual_bone_lengths = np.asarray(
            [
                np.linalg.norm(actual_world[end] - actual_world[start])
                for start, end in HAND_CONNECTIONS
            ]
        )
        bone_absolute_error = np.abs(actual_bone_lengths - expected_bone_lengths)
        bone_length_absolute_errors.append(bone_absolute_error)
        bone_length_relative_errors.append(
            bone_absolute_error / np.maximum(expected_bone_lengths, 1.0e-6)
        )
        sample_relative_world_rmses.append(
            (sample_name, float(np.sqrt(np.mean(np.square(relative_point_error)))))
        )

        sample_prefix, _, sample_number = Path(sample_name).stem.rpartition("-")
        current_sample_index = int(sample_number) if sample_number.isdigit() else None
        if (
            previous_sample_prefix == sample_prefix
            and previous_sample_index is not None
            and current_sample_index == previous_sample_index + 1
            and previous_expected_relative_world is not None
            and previous_actual_relative_world is not None
        ):
            expected_delta = expected_relative_world - previous_expected_relative_world
            actual_delta = actual_relative_world - previous_actual_relative_world
            relative_world_delta_errors.append(
                np.linalg.norm(actual_delta - expected_delta, axis=1)
            )
        previous_sample_prefix = sample_prefix
        previous_sample_index = current_sample_index
        previous_expected_relative_world = expected_relative_world
        previous_actual_relative_world = actual_relative_world

    sample_count = len(expected_landmarks)
    result: dict[str, object] = {
        "presence_threshold": HAND_PRESENCE_THRESHOLD,
        "baseline_detected_samples": detected_samples,
        "sample_presence_flip_rate": presence_flips / sample_count,
        "presence_false_negative_count": presence_false_negatives,
        "presence_false_positive_count": presence_false_positives,
        "longest_consecutive_presence_flip_run": longest_flip_run,
        "presence_flip_samples": presence_flip_samples,
    }
    if detected_samples:
        points = np.concatenate(landmark_point_errors)
        rmses = np.asarray([rmse for _, rmse in sample_landmark_rmses])
        worst_samples = sorted(
            sample_landmark_rmses, key=lambda item: item[1], reverse=True
        )[:10]
        result.update(
            {
                "handedness_flip_rate": handedness_flips / detected_samples,
                "landmark_xy_mae_pixels": float(np.mean(points)),
                "landmark_xy_p95_pixels": float(np.percentile(points, 95)),
                "landmark_xy_max_pixels": float(np.max(points)),
                "samples_over_5px_rmse_rate": float(np.mean(rmses > 5.0)),
                "sample_landmark_rmse_p95_pixels": float(np.percentile(rmses, 95)),
                "sample_landmark_rmse_max_pixels": float(np.max(rmses)),
                "worst_landmark_samples": [
                    {"sample": sample, "rmse_pixels": rmse}
                    for sample, rmse in worst_samples
                ],
            }
        )
        world_components = np.concatenate(world_component_errors, axis=0)
        world_points = np.concatenate(world_point_errors)
        relative_world_points = np.concatenate(relative_world_point_errors)
        relative_world_rmses = np.asarray(
            [rmse for _, rmse in sample_relative_world_rmses]
        )
        normalized_relative_world_points = np.concatenate(
            normalized_relative_world_point_errors
        )
        bone_absolute_errors = np.concatenate(bone_length_absolute_errors)
        bone_relative_errors = np.concatenate(bone_length_relative_errors)
        result.update(
            {
                "world_landmark_units": "model_world_units",
                "world_xyz_component_mae": {
                    "x": float(np.mean(world_components[:, 0])),
                    "y": float(np.mean(world_components[:, 1])),
                    "z": float(np.mean(world_components[:, 2])),
                },
                "world_xyz_component_p95": {
                    "x": float(np.percentile(world_components[:, 0], 95)),
                    "y": float(np.percentile(world_components[:, 1], 95)),
                    "z": float(np.percentile(world_components[:, 2], 95)),
                },
                "world_point_3d_mae": float(np.mean(world_points)),
                "world_point_3d_p95": float(np.percentile(world_points, 95)),
                "world_point_3d_max": float(np.max(world_points)),
                "wrist_relative_world_point_3d_mae": float(
                    np.mean(relative_world_points)
                ),
                "wrist_relative_world_point_3d_p95": float(
                    np.percentile(relative_world_points, 95)
                ),
                "wrist_relative_sample_rmse_p95": float(
                    np.percentile(relative_world_rmses, 95)
                ),
                "baseline_wrist_to_middle_mcp_median": float(
                    np.median(world_hand_scales)
                ),
                "wrist_relative_error_over_hand_scale_mae": float(
                    np.mean(normalized_relative_world_points)
                ),
                "wrist_relative_error_over_hand_scale_p95": float(
                    np.percentile(normalized_relative_world_points, 95)
                ),
                "bone_length_absolute_error_mae": float(
                    np.mean(bone_absolute_errors)
                ),
                "bone_length_relative_error_mae": float(
                    np.mean(bone_relative_errors)
                ),
                "bone_length_relative_error_p95": float(
                    np.percentile(bone_relative_errors, 95)
                ),
            }
        )
        if relative_world_delta_errors:
            delta_errors = np.concatenate(relative_world_delta_errors)
            result.update(
                {
                    "consecutive_frame_pairs": len(relative_world_delta_errors),
                    "wrist_relative_frame_delta_error_mae": float(
                        np.mean(delta_errors)
                    ),
                    "wrist_relative_frame_delta_error_p95": float(
                        np.percentile(delta_errors, 95)
                    ),
                }
            )
    return result


def compare_model(
    name: str,
    float_path: Path,
    qdq_path: Path,
    tensor_dir: Path,
    shape: tuple[int, ...],
    dataset_manifest_sha256: str | None,
) -> dict[str, object]:
    baseline = ort.InferenceSession(str(float_path), providers=["CPUExecutionProvider"])
    candidate = ort.InferenceSession(str(qdq_path), providers=["CPUExecutionProvider"])
    baseline_outputs = [output.name for output in baseline.get_outputs()]
    candidate_outputs = [output.name for output in candidate.get_outputs()]
    if baseline_outputs != candidate_outputs:
        raise ValueError(f"{name} QDQ output contract differs from the float model")

    baseline_input = baseline.get_inputs()[0]
    candidate_input = candidate.get_inputs()[0]
    if (
        tuple(baseline_input.shape) != shape
        or tuple(candidate_input.shape) != shape
        or baseline_input.type != "tensor(float)"
        or candidate_input.type != "tensor(float)"
    ):
        raise ValueError(f"{name} input contract changed during quantization")
    files = sorted(tensor_dir.glob("*.f32"))
    if not files:
        raise ValueError(f"No comparison tensors found in {tensor_dir}")

    errors: dict[str, list[np.ndarray]] = {output: [] for output in baseline_outputs}
    expected_by_output: list[list[np.ndarray]] = [[] for _ in baseline_outputs]
    actual_by_output: list[list[np.ndarray]] = [[] for _ in baseline_outputs]
    sample_names = [path.name for path in files]
    for path in files:
        tensor = np.fromfile(path, dtype=np.float32).reshape(shape)
        expected = baseline.run(None, {baseline_input.name: tensor})
        actual = candidate.run(None, {candidate_input.name: tensor})
        for output_index, (output_name, expected_value, actual_value) in enumerate(
            zip(baseline_outputs, expected, actual, strict=True)
        ):
            expected_by_output[output_index].append(expected_value.astype(np.float32))
            actual_by_output[output_index].append(actual_value.astype(np.float32))
            errors[output_name].append(
                np.abs(expected_value.astype(np.float32) - actual_value.astype(np.float32)).ravel()
            )

    output_metrics = {
        output_name: summarize(np.concatenate(output_errors))
        for output_name, output_errors in errors.items()
    }
    report: dict[str, object] = {
        "model": name,
        "float_model_sha256": sha256(float_path),
        "qdq_model_sha256": sha256(qdq_path),
        "dataset_manifest_sha256": dataset_manifest_sha256,
        "samples": len(files),
        "outputs": output_metrics,
    }
    if name == "palm":
        report["task_metrics"] = summarize_palm_decisions(
            expected_by_output, actual_by_output, sample_names
        )
    elif name == "hand":
        report["task_metrics"] = summarize_hand_decisions(
            expected_by_output, actual_by_output, sample_names
        )
    return report


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Compare float and QNN QDQ raw outputs on identical native tensors."
    )
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--calibration-dir", type=Path, required=True)
    parser.add_argument("--dataset-manifest", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    manifest_hash = None
    if args.dataset_manifest is not None:
        if not args.dataset_manifest.is_file():
            raise FileNotFoundError(args.dataset_manifest)
        manifest_hash = sha256(args.dataset_manifest)

    reports = []
    for spec in MODEL_SPECS:
        reports.append(
            compare_model(
                spec.name,
                args.model_dir / spec.source_name,
                args.model_dir / spec.output_name,
                args.calibration_dir / spec.name,
                spec.shape,
                manifest_hash,
            )
        )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(reports, indent=2) + "\n", encoding="utf-8")
    print(args.output)


if __name__ == "__main__":
    main()

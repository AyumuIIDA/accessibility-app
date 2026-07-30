#!/usr/bin/env python3
"""Analyze a labelled-as-Open-Palm official reference JSONL recording."""

from __future__ import annotations

import argparse
import collections
import json
import math
from pathlib import Path


def subtract(left: list[float], right: list[float]) -> tuple[float, float, float]:
    return left[0] - right[0], left[1] - right[1], left[2] - right[2]


def cross(
    left: tuple[float, float, float], right: tuple[float, float, float]
) -> tuple[float, float, float]:
    return (
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0],
    )


def palm_angles(flat: list[float]) -> tuple[float, float, float]:
    points = [flat[index : index + 3] for index in range(0, len(flat), 3)]
    normal = cross(subtract(points[5], points[0]), subtract(points[17], points[0]))
    length = math.sqrt(sum(value * value for value in normal))
    if length <= 1e-8:
        return math.nan, math.nan, math.nan
    x, y, z = (value / length for value in normal)
    incidence = math.degrees(math.acos(max(0.0, min(1.0, abs(z)))))
    return (
        incidence,
        math.degrees(math.atan2(x, abs(z))),
        math.degrees(math.atan2(y, abs(z))),
    )


def hand_roll(flat: list[float]) -> float:
    points = [flat[index : index + 3] for index in range(0, len(flat), 3)]
    direction = subtract(points[9], points[0])
    return math.degrees(math.atan2(direction[0], -direction[1]))


def add_angle_bin(
    bins: dict[str, list[bool]], value: float, success: bool, signed: bool
) -> None:
    if not math.isfinite(value):
        bins["invalid"].append(success)
        return
    if signed:
        value = max(-89.999, min(89.999, value))
        lower = math.floor(value / 15) * 15
    else:
        value = max(0.0, min(89.999, value))
        lower = int(value // 15) * 15
    bins[f"{lower:+03d}..{lower + 15:+03d}"].append(success)


def percentile(values: list[float], ratio: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return math.nan
    return ordered[round((len(ordered) - 1) * ratio)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("recording", type=Path)
    args = parser.parse_args()
    samples = [
        json.loads(line)
        for line in args.recording.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    labels = collections.Counter(sample["result"]["label"] for sample in samples)
    open_scores = [sample["result"]["open_palm_confidence"] for sample in samples]
    failures = [sample for sample in samples if sample["result"]["label"] != "Open_Palm"]

    longest_failure = 0
    current_failure = 0
    for sample in samples:
        if sample["result"]["label"] == "Open_Palm":
            current_failure = 0
        else:
            current_failure += 1
            longest_failure = max(longest_failure, current_failure)

    incidence_bins: dict[str, list[bool]] = collections.defaultdict(list)
    horizontal_bins: dict[str, list[bool]] = collections.defaultdict(list)
    vertical_bins: dict[str, list[bool]] = collections.defaultdict(list)
    roll_bins: dict[str, list[bool]] = collections.defaultdict(list)
    for sample in samples:
        incidence, horizontal, vertical = palm_angles(
            sample["input"]["world_landmarks"]
        )
        roll = hand_roll(sample["input"]["image_landmarks"])
        success = sample["result"]["label"] == "Open_Palm"
        add_angle_bin(incidence_bins, incidence, success, False)
        add_angle_bin(horizontal_bins, horizontal, success, True)
        add_angle_bin(vertical_bins, vertical, success, True)
        add_angle_bin(roll_bins, roll, success, True)

    def summarize(bins: dict[str, list[bool]]) -> dict[str, dict[str, float | int]]:
        return {
            key: {
                "samples": len(values),
                "open_palm_winner": sum(values),
                "recall": sum(values) / len(values),
            }
            for key, values in sorted(bins.items())
        }

    report = {
        "samples_with_landmarks": len(samples),
        "open_palm_winner": labels["Open_Palm"],
        "conditional_recall": labels["Open_Palm"] / len(samples) if samples else 0.0,
        "labels": dict(labels),
        "open_palm_score": {
            "minimum": min(open_scores, default=math.nan),
            "p10": percentile(open_scores, 0.10),
            "median": percentile(open_scores, 0.50),
            "p90": percentile(open_scores, 0.90),
            "maximum": max(open_scores, default=math.nan),
        },
        "longest_classifier_failure_samples": longest_failure,
        "first_failure_frame_ids": [
            sample["result"]["frame_id"] for sample in failures[:10]
        ],
        "palm_incidence_degrees_0_is_camera_facing": summarize(incidence_bins),
        "palm_horizontal_tilt_degrees": summarize(horizontal_bins),
        "palm_vertical_tilt_degrees": summarize(vertical_bins),
        "hand_roll_degrees": summarize(roll_bins),
    }
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

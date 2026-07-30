#!/usr/bin/env python3
"""Replay a recording through MediaPipe's official landmark preprocessing."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

from official_gesture_reference import OfficialGestureReference


def normalize_object(values: list[float], aspect: float, screen: bool) -> np.ndarray:
    landmarks = np.asarray(values, dtype=np.float32).reshape(21, 3).copy()
    if screen:
        if aspect >= 1.0:
            landmarks[:, 1] = (landmarks[:, 1] - 0.5) / aspect + 0.5
        else:
            landmarks[:, 0] = (landmarks[:, 0] - 0.5) * aspect + 0.5
    landmarks -= landmarks[0]
    extent = max(
        float(np.ptp(landmarks[:, 0])),
        float(np.ptp(landmarks[:, 1])),
    )
    landmarks /= extent + 1e-5
    return landmarks.reshape(63)


def evaluate(
    recognizer: OfficialGestureReference, samples: list[dict], aspect: float
) -> tuple[int, float, float]:
    successes = 0
    scores: list[float] = []
    for sample in samples:
        request = dict(sample["input"])
        request["image_landmarks"] = normalize_object(
            request["image_landmarks"], aspect, True
        ).tolist()
        request["world_landmarks"] = normalize_object(
            request["world_landmarks"], aspect, False
        ).tolist()
        result = recognizer.classify(request)
        scores.append(float(result["open_palm_confidence"]))
        successes += result["label"] == "Open_Palm"
    return successes, successes / len(samples), float(np.median(scores))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("recording", type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--limit", type=int)
    args = parser.parse_args()
    with args.recording.open(encoding="utf-8") as recording:
        samples = [json.loads(line) for line in recording if line.strip()]
    if args.limit:
        samples = samples[:: max(1, len(samples) // args.limit)][: args.limit]
    if not samples:
        print("Recording contains no classified samples.", file=sys.stderr)
        return 1

    recognizer = OfficialGestureReference(args.model)
    print(f"samples={len(samples)}")
    for name, aspect in (("square", 1.0), ("4:3", 4 / 3), ("16:9", 16 / 9)):
        successes, recall, median = evaluate(recognizer, samples, aspect)
        print(
            f"{name:6} open_palm={successes:4}/{len(samples)} "
            f"recall={recall:.3%} median_score={median:.4f}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""JSON-lines bridge to the official MediaPipe canned gesture models."""

from __future__ import annotations

import argparse
import io
import json
import sys
import time
import zipfile
from pathlib import Path

import numpy as np
from ai_edge_litert.interpreter import Interpreter


LABELS = (
    "None",
    "Closed_Fist",
    "Open_Palm",
    "Pointing_Up",
    "Thumb_Down",
    "Thumb_Up",
    "Victory",
    "ILoveYou",
)


class OfficialGestureReference:
    def __init__(self, bundle_path: Path, record_path: Path | None = None) -> None:
        with zipfile.ZipFile(bundle_path) as outer:
            nested_bundle = outer.read("hand_gesture_recognizer.task")
        with zipfile.ZipFile(io.BytesIO(nested_bundle)) as classifier_bundle:
            embedder_model = classifier_bundle.read("gesture_embedder.tflite")
            classifier_model = classifier_bundle.read("canned_gesture_classifier.tflite")

        self._embedder = Interpreter(model_content=embedder_model, num_threads=1)
        self._embedder.allocate_tensors()
        self._classifier = Interpreter(model_content=classifier_model, num_threads=1)
        self._classifier.allocate_tensors()

        embedder_inputs = {item["name"]: item for item in self._embedder.get_input_details()}
        required = {"hand", "handedness", "world_hand"}
        if set(embedder_inputs) != required:
            raise RuntimeError(
                f"Unexpected official embedder inputs: {sorted(embedder_inputs)}"
            )
        self._hand_input = embedder_inputs["hand"]["index"]
        self._handedness_input = embedder_inputs["handedness"]["index"]
        self._world_hand_input = embedder_inputs["world_hand"]["index"]
        self._embedding_output = self._embedder.get_output_details()[0]["index"]
        self._classifier_input = self._classifier.get_input_details()[0]["index"]
        self._classifier_output = self._classifier.get_output_details()[0]["index"]
        self._record_file = None
        if record_path is not None:
            record_path.parent.mkdir(parents=True, exist_ok=True)
            self._record_file = record_path.open("a", encoding="utf-8", buffering=1)

    def classify(self, request: dict[str, object]) -> dict[str, object]:
        image_landmarks = np.asarray(
            request["image_landmarks"], dtype=np.float32
        ).reshape(1, 21, 3)
        world_landmarks = np.asarray(
            request["world_landmarks"], dtype=np.float32
        ).reshape(1, 21, 3)
        handedness = np.asarray(
            [[request["handedness"]]], dtype=np.float32
        )
        if not np.isfinite(image_landmarks).all():
            raise ValueError("image_landmarks contains a non-finite value")
        if not np.isfinite(world_landmarks).all():
            raise ValueError("world_landmarks contains a non-finite value")

        started_ns = time.perf_counter_ns()
        self._embedder.set_tensor(self._hand_input, image_landmarks)
        self._embedder.set_tensor(self._handedness_input, handedness)
        self._embedder.set_tensor(self._world_hand_input, world_landmarks)
        self._embedder.invoke()
        embedding = self._embedder.get_tensor(self._embedding_output)
        self._classifier.set_tensor(self._classifier_input, embedding)
        self._classifier.invoke()
        scores = self._classifier.get_tensor(self._classifier_output)[0]
        elapsed_ms = (time.perf_counter_ns() - started_ns) / 1_000_000.0
        best_index = int(np.argmax(scores))
        result = {
            "frame_id": int(request["frame_id"]),
            "label": LABELS[best_index],
            "confidence": float(scores[best_index]),
            "open_palm_confidence": float(scores[2]),
            "scores": [float(value) for value in scores],
            "inference_ms": elapsed_ms,
        }
        if self._record_file is not None:
            self._record_file.write(
                json.dumps({"input": request, "result": result}, separators=(",", ":"))
                + "\n"
            )
        return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--record", type=Path)
    parser.add_argument("--inspect", action="store_true")
    args = parser.parse_args()
    recognizer = OfficialGestureReference(
        args.model.resolve(),
        args.record.resolve() if args.record is not None else None,
    )
    if args.inspect:
        print(
            json.dumps(
                {
                    "ready": True,
                    "inputs": {
                        "hand": [1, 21, 3],
                        "handedness": [1, 1],
                        "world_hand": [1, 21, 3],
                    },
                    "embedding": [1, 128],
                    "labels": LABELS,
                }
            ),
            flush=True,
        )
        return 0

    print(json.dumps({"ready": True}), flush=True)
    for line in sys.stdin:
        try:
            request = json.loads(line)
            print(json.dumps(recognizer.classify(request)), flush=True)
        except Exception as exception:
            frame_id = request.get("frame_id", 0) if "request" in locals() else 0
            print(
                json.dumps(
                    {
                        "frame_id": frame_id,
                        "error": f"{type(exception).__name__}: {exception}",
                    }
                ),
                flush=True,
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

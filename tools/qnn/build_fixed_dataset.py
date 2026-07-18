from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path

import numpy as np

from quantize_models import MODEL_SPECS, ModelSpec


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def session_id(path: Path) -> str:
    stem = path.stem
    if "-" not in stem:
        raise ValueError(f"Tensor name must contain a capture session prefix: {path.name}")
    session, sequence = stem.rsplit("-", 1)
    if not session or not sequence.isdigit():
        raise ValueError(f"Invalid captured tensor name: {path.name}")
    return session


def validate_tensor(path: Path, spec: ModelSpec) -> None:
    expected_bytes = int(np.prod(spec.shape)) * np.dtype(np.float32).itemsize
    actual_bytes = path.stat().st_size
    if actual_bytes != expected_bytes:
        raise ValueError(
            f"{path} contains {actual_bytes} bytes; expected {expected_bytes} for {spec.shape}"
        )


def link_sample(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    os.link(source, destination)


def build_model_split(
    spec: ModelSpec,
    capture_root: Path,
    output_root: Path,
    holdout_sessions: set[str],
) -> dict[str, object]:
    source_directory = capture_root / spec.name
    files = sorted(source_directory.glob("*.f32"))
    if not files:
        raise ValueError(f"No {spec.name} tensors found in {source_directory}")

    samples: dict[str, list[dict[str, str]]] = {"calibration": [], "holdout": []}
    sessions: dict[str, set[str]] = {"calibration": set(), "holdout": set()}
    for source in files:
        validate_tensor(source, spec)
        capture_session = session_id(source)
        split = "holdout" if capture_session in holdout_sessions else "calibration"
        destination = output_root / split / spec.name / source.name
        link_sample(source, destination)
        sessions[split].add(capture_session)
        samples[split].append(
            {
                "path": destination.relative_to(output_root).as_posix(),
                "session_id": capture_session,
                "sha256": sha256(source),
            }
        )

    for split in ("calibration", "holdout"):
        if not samples[split]:
            raise ValueError(f"{spec.name} has no samples in the {split} split")

    if sessions["calibration"] & sessions["holdout"]:
        raise AssertionError(f"{spec.name} capture session leaked across dataset splits")

    return {
        "shape": list(spec.shape),
        "dtype": "float32",
        "layout": "NHWC",
        "calibration_sessions": sorted(sessions["calibration"]),
        "holdout_sessions": sorted(sessions["holdout"]),
        "calibration_samples": samples["calibration"],
        "holdout_samples": samples["holdout"],
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Freeze captured native tensors into session-isolated calibration and holdout sets."
    )
    parser.add_argument("--capture-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--holdout-session",
        action="append",
        required=True,
        help="Capture process/session prefix reserved for holdout. Repeat for multiple sessions.",
    )
    args = parser.parse_args()

    if args.output_dir.exists() and any(args.output_dir.iterdir()):
        raise ValueError(f"Output directory must be empty: {args.output_dir}")
    args.output_dir.mkdir(parents=True, exist_ok=True)

    holdout_sessions = set(args.holdout_session)
    models = {
        spec.name: build_model_split(
            spec, args.capture_dir, args.output_dir, holdout_sessions
        )
        for spec in MODEL_SPECS
    }
    manifest = {
        "schema_version": 1,
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "split_unit": "capture_session",
        "holdout_sessions": sorted(holdout_sessions),
        "models": models,
    }
    manifest_path = args.output_dir / "dataset-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(manifest_path)


if __name__ == "__main__":
    main()

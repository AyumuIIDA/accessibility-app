from __future__ import annotations

import argparse
import json
from pathlib import Path

from compare_models import compare_model, sha256


HAND_SHAPE = (1, 224, 224, 3)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Compare hand QDQ candidates against one float baseline."
    )
    parser.add_argument("--float-model", type=Path, required=True)
    parser.add_argument("--candidate", action="append", nargs=2, metavar=("NAME", "PATH"), required=True)
    parser.add_argument("--holdout-dir", type=Path, required=True)
    parser.add_argument("--dataset-manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    manifest_hash = sha256(args.dataset_manifest)
    reports: dict[str, object] = {
        "float_model": str(args.float_model),
        "float_model_sha256": sha256(args.float_model),
        "dataset_manifest": str(args.dataset_manifest),
        "dataset_manifest_sha256": manifest_hash,
        "holdout_directory": str(args.holdout_dir),
        "candidates": {},
    }
    candidates = reports["candidates"]
    assert isinstance(candidates, dict)
    for name, candidate_path_text in args.candidate:
        candidate_path = Path(candidate_path_text)
        candidates[name] = compare_model(
            "hand",
            args.float_model,
            candidate_path,
            args.holdout_dir,
            HAND_SHAPE,
            manifest_hash,
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(reports, indent=2) + "\n", encoding="utf-8")
    print(args.output)


if __name__ == "__main__":
    main()

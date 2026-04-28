#!/usr/bin/env python3
# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import argparse
from pathlib import Path
import re
import sys

import yaml


def expand_range(prefix: str, start: int, end: int, suffix: str) -> list[str]:
    return [f"{prefix}{value:04d}{suffix}" for value in range(start, end + 1)]


def build_90604_manifest() -> tuple[list[str], list[list[str]]]:
    required = ["cuPhyChEstCoeffs.h5"]
    required.extend(expand_range("TVnr_DLMIX_", 5960, 6055, "_gNB_FAPI_s0.h5"))
    required.extend(expand_range("TVnr_DLMIX_", 6080, 6159, "_gNB_FAPI_s0.h5"))
    required.extend(expand_range("TVnr_DLMIX_", 6168, 6175, "_gNB_FAPI_s0.h5"))
    required.extend(expand_range("TVnr_ULMIX_", 2864, 2895, "_gNB_FAPI_s0.h5"))
    alternates = [["launch_pattern_nrSim_90604.yaml", "launch_pattern_F08_8C_57.yaml"]]
    return required, alternates


def collect_h5_names(node, out: set[str]) -> None:
    if isinstance(node, dict):
        for value in node.values():
            collect_h5_names(value, out)
        return
    if isinstance(node, list):
        for value in node:
            collect_h5_names(value, out)
        return
    if isinstance(node, str) and node.endswith(".h5"):
        out.add(Path(node).name)


def render_range(prefix: str, start: int, end: int, suffix: str) -> str:
    if start == end:
        return f"{prefix}{start:04d}{suffix}"
    return f"{prefix}{start:04d}..{end:04d}{suffix}"


def compress_tv_names(names: list[str]) -> list[str]:
    grouped: dict[tuple[str, str], list[int]] = {}
    passthrough: list[str] = []
    pattern = re.compile(r"^(TVnr_(?:DLMIX|ULMIX)_)(\d{4})(_gNB_FAPI_s\d+\.h5)$")

    for name in sorted(names):
        match = pattern.match(name)
        if not match:
            passthrough.append(name)
            continue
        key = (match.group(1), match.group(3))
        grouped.setdefault(key, []).append(int(match.group(2)))

    compressed = []
    for (prefix, suffix), values in sorted(grouped.items()):
        start = prev = values[0]
        for value in values[1:]:
            if value == prev + 1:
                prev = value
                continue
            compressed.append(render_range(prefix, start, prev, suffix))
            start = prev = value
        compressed.append(render_range(prefix, start, prev, suffix))

    compressed.extend(sorted(passthrough))
    return compressed


def fail(message: str) -> int:
    print(f"ERROR: {message}", file=sys.stderr)
    return 1


def load_launch_pattern(path: Path) -> set[str]:
    with path.open("r", encoding="utf-8") as handle:
        parsed = yaml.safe_load(handle)
    required: set[str] = set()
    collect_h5_names(parsed, required)
    required.add("cuPhyChEstCoeffs.h5")
    return required


def find_generic_manifest(tv_src: Path, pattern: str) -> tuple[list[str], list[list[str]], str] | None:
    launch_patterns = [tv_src / f"launch_pattern_nrSim_{pattern}.yaml"]
    launch_patterns.extend(sorted(tv_src.glob(f"launch_pattern_F08_*_{pattern}.yaml")))

    for launch_pattern in launch_patterns:
        if launch_pattern.exists():
            required = sorted(load_launch_pattern(launch_pattern))
            return required, [[launch_pattern.name]], f"launch pattern {launch_pattern.name}"

    exact_tvs = sorted(path.name for path in tv_src.glob(f"TVnr_{pattern}_gNB_FAPI_s*.h5"))
    if exact_tvs:
        required = ["cuPhyChEstCoeffs.h5", *exact_tvs]
        return required, [], f"direct TV files for pattern {pattern}"

    return None


def check_manifest(tv_src: Path, pattern: str) -> int:
    if pattern == "90604":
        required, alternates = build_90604_manifest()
        manifest_source = "special-case alias of launch_pattern_F08_8C_57.yaml"
    else:
        generic_manifest = find_generic_manifest(tv_src, pattern)
        if generic_manifest is None:
            return fail(
                f"could not infer required assets for pattern {pattern} in {tv_src}. "
                "No launch pattern YAML or matching TVnr_* files were found."
            )
        required, alternates, manifest_source = generic_manifest

    available = {path.name for path in tv_src.iterdir() if path.is_file()}
    missing = [name for name in required if name not in available]
    missing_alt_groups = [group for group in alternates if not any(name in available for name in group)]

    print("Waveform Asset Check")
    print(f"  pattern:         {pattern}")
    print(f"  source:          {tv_src}")
    print(f"  manifest source: {manifest_source}")
    print(f"  required files:  {len(required)}")
    if alternates:
        print(f"  alt groups:      {len(alternates)}")

    if not missing and not missing_alt_groups:
        print("Result: PASS")
        return 0

    print("Result: FAIL")
    if missing_alt_groups:
        print("Missing launch-pattern aliases:")
        for group in missing_alt_groups:
            print("  - one of: " + ", ".join(group))

    if missing:
        print(f"Missing required files: {len(missing)}")
        for item in compress_tv_names(missing):
            print(f"  - {item}")

    return 1


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify that a waveform asset bundle contains the files needed for an Aerial pattern."
    )
    parser.add_argument("pattern", help="Waveform pattern, for example 90604")
    parser.add_argument("tv_src", help="Path to the GPU_test_input directory")
    args = parser.parse_args()

    tv_src = Path(args.tv_src).resolve()
    if not tv_src.is_dir():
        return fail(f"test-vector source is not a directory: {tv_src}")

    return check_manifest(tv_src, args.pattern)


if __name__ == "__main__":
    sys.exit(main())

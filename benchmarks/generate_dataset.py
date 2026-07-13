#!/usr/bin/env python3
"""Generate deterministic LocalVault benchmark datasets without destructive cleanup."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


GENERATOR_VERSION = 1
GIB = 1024**3
STREAM_BLOCK_SIZE = 1024**2


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--profile", choices=("large-files", "large"), required=True)
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--size-gib", type=int, required=True)
    arguments = parser.parse_args()
    if arguments.size_gib <= 0:
        parser.error("--size-gib must be positive")
    return arguments


def prepare_output(output: Path) -> Path:
    output = output.expanduser().resolve()
    if output.exists():
        if not output.is_dir():
            raise SystemExit(f"output exists and is not a directory: {output}")
        if any(output.iterdir()):
            raise SystemExit(f"refusing to modify non-empty output directory: {output}")
    else:
        output.mkdir(parents=True)
    return output


def write_deterministic_file(path: Path, size: int, seed: int, file_index: int) -> None:
    with path.open("xb") as output:
        remaining = size
        block_index = 0
        while remaining:
            count = min(STREAM_BLOCK_SIZE, remaining)
            key = f"{GENERATOR_VERSION}:{seed}:{file_index}:{block_index}".encode()
            output.write(hashlib.shake_256(key).digest(count))
            remaining -= count
            block_index += 1


def generate_large_files(output: Path, seed: int, logical_bytes: int) -> list[Path]:
    file_count = min(4, max(1, logical_bytes // (256 * 1024**2)))
    base_size, remainder = divmod(logical_bytes, file_count)
    files: list[Path] = []
    for file_index in range(file_count):
        path = output / f"large-{file_index:03d}.bin"
        size = base_size + (1 if file_index < remainder else 0)
        write_deterministic_file(path, size, seed, file_index)
        files.append(path)
    return files


def main() -> None:
    arguments = parse_arguments()
    output = prepare_output(arguments.output)
    profile = "large-files" if arguments.profile == "large" else arguments.profile
    logical_bytes = arguments.size_gib * GIB
    files = generate_large_files(output, arguments.seed, logical_bytes)
    manifest = {
        "generator_version": GENERATOR_VERSION,
        "seed": arguments.seed,
        "profile": profile,
        "file_count": len(files),
        "logical_bytes": logical_bytes,
    }
    with (output / "manifest.json").open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(manifest, stream, indent=2, sort_keys=True)
        stream.write("\n")


if __name__ == "__main__":
    main()

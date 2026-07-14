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
    parser.add_argument(
        "--profile", choices=("large-files", "large", "many-small"), required=True
    )
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--size-gib", type=int)
    parser.add_argument("--file-count", type=int)
    arguments = parser.parse_args()
    if arguments.profile == "many-small":
        if arguments.file_count is None or arguments.file_count <= 0:
            parser.error("--file-count must be positive for many-small")
        if arguments.size_gib is not None:
            parser.error("--size-gib is not used for many-small")
    else:
        if arguments.size_gib is None or arguments.size_gib <= 0:
            parser.error("--size-gib must be positive for large-files")
        if arguments.file_count is not None:
            parser.error("--file-count is not used for large-files")
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


def repeated_to_size(prefix: bytes, fill: bytes, suffix: bytes, size: int) -> bytes:
    if size == 0:
        return b""
    if size < len(prefix) + len(suffix):
        return (prefix + suffix)[:size].ljust(size, b" ")
    fill_size = size - len(prefix) - len(suffix)
    repeats = (fill_size + len(fill) - 1) // len(fill)
    return prefix + (fill * repeats)[:fill_size] + suffix


def many_small_payload(kind: int, size: int, seed: int, variant: int) -> bytes:
    if kind == 0:
        return repeated_to_size(
            b"LocalVault profile A\n", b"deterministic text line\n", b"", size
        )
    if kind == 1:
        return repeated_to_size(b'{"data":"', b"json-data-", b'"}', size)
    if kind == 2:
        return repeated_to_size(
            b"// LocalVault profile A\n", b"int value = 42;\n", b"", size
        )
    key = f"{GENERATOR_VERSION}:{seed}:many-small:{variant}:{size}".encode()
    return hashlib.shake_256(key).digest(size)


def generate_many_small(output: Path, seed: int, file_count: int) -> tuple[Path, int]:
    dataset = output / "dataset"
    dataset.mkdir()
    sizes = (0, 16, 64, 256, 1024, 4096, 8192, 16 * 1024)
    extensions = ("txt", "json", "cpp", "bin")
    logical_bytes = 0
    created_directories: set[Path] = set()
    for index in range(file_count):
        kind = index % len(extensions)
        size = sizes[(index // len(extensions)) % len(sizes)]
        wide = index % 64
        branch = (index // 64) % 16
        depth = (index // 1024) % 8
        directory = dataset / f"wide-{wide:02d}" / f"branch-{branch:02d}"
        for level in range(depth + 1):
            directory /= f"deep-{level}"
        directory /= extensions[kind]
        if directory not in created_directories:
            directory.mkdir(parents=True, exist_ok=True)
            created_directories.add(directory)
        path = directory / f"file-{index:05d}.{extensions[kind]}"
        variant = (index // (len(extensions) * len(sizes))) % 32
        path.write_bytes(many_small_payload(kind, size, seed, variant))
        logical_bytes += size
    return dataset, logical_bytes


def main() -> None:
    arguments = parse_arguments()
    output = prepare_output(arguments.output)
    profile = "large-files" if arguments.profile == "large" else arguments.profile
    if profile == "many-small":
        dataset, logical_bytes = generate_many_small(
            output, arguments.seed, arguments.file_count
        )
        file_count = arguments.file_count
        dataset_root = dataset.name
    else:
        logical_bytes = arguments.size_gib * GIB
        files = generate_large_files(output, arguments.seed, logical_bytes)
        file_count = len(files)
        dataset_root = "."
    manifest = {
        "generator_version": GENERATOR_VERSION,
        "seed": arguments.seed,
        "profile": profile,
        "file_count": file_count,
        "logical_bytes": logical_bytes,
        "dataset_root": dataset_root,
    }
    with (output / "manifest.json").open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(manifest, stream, indent=2, sort_keys=True)
        stream.write("\n")


if __name__ == "__main__":
    main()

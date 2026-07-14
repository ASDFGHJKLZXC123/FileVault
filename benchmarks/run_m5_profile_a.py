#!/usr/bin/env python3
"""Generate and run the fixed M5 profile-A acceptance dataset."""

from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--benchmark", type=Path, required=True)
    arguments = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="localvault-m5-profile-a-") as temporary:
        root = Path(temporary)
        generated = root / "generated"
        subprocess.run(
            [
                "python3",
                str(arguments.generator),
                "--output",
                str(generated),
                "--profile",
                "many-small",
                "--seed",
                "12345",
                "--file-count",
                "50000",
            ],
            check=True,
        )
        subprocess.run(
            [
                str(arguments.benchmark),
                str(generated / "dataset"),
                str(root / "repository"),
            ],
            check=True,
        )


if __name__ == "__main__":
    main()

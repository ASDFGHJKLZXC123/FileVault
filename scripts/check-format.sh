#!/usr/bin/env bash
set -euo pipefail

if ! command -v clang-format >/dev/null 2>&1; then
    echo "clang-format not found — install it (e.g. brew install clang-format)"
    exit 1
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

status=0
while IFS= read -r -d '' file; do
    clang-format --dry-run --Werror --style=file "$REPO_ROOT/$file" || status=1
done < <(git -C "$REPO_ROOT" ls-files -z -- '*.cpp' '*.hpp')

exit $status

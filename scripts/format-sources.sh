#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

find "$REPO_ROOT/include" "$REPO_ROOT/src" \
    -type f \( -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) \
    -print0 \
  | xargs -0 clang-format -i --style=file

echo "Formatting complete."

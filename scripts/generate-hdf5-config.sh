#!/bin/bash
# Generate an HDF5 driver config file with a specific PV name.
# Usage: ./scripts/generate-hdf5-config.sh <PV_NAME> [OUTPUT_PATH]
#
# This script reads config-hdf5-pv.yaml (which contains __PV_NAME__ as a placeholder),
# substitutes the actual PV name, and writes the result to the output path.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

PV_NAME="${1:-}"
if [ -z "$PV_NAME" ]; then
    echo "Usage: $0 <PV_NAME> [OUTPUT_PATH]" >&2
    echo "  PV_NAME: EPICS Process Variable name (e.g., TEST:VOLTAGE)" >&2
    echo "  OUTPUT_PATH: optional output file path (default: .vscode/config-hdf5-pv-<PV_NAME>.yaml)" >&2
    exit 1
fi

OUTPUT_PATH="${2:-.vscode/config-hdf5-pv-${PV_NAME}.yaml}"

# Ensure the output directory exists
mkdir -p "$(dirname "${OUTPUT_PATH}")"

# Read the template and substitute the PV name placeholder (case-insensitive placeholder matching)
sed "s|__PV_NAME__|${PV_NAME}|gI" "${ROOT_DIR}/config-hdf5-pv.yaml" > "${OUTPUT_PATH}"

echo "Generated config: ${OUTPUT_PATH} (PV: ${PV_NAME})"
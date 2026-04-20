#!/bin/bash
# Generate an HDF5 driver config file with a specific PV name and optional mode.
# Usage: ./scripts/generate-hdf5-config.sh <PV_NAME> [OPTIONS]
#
# Options:
#   --mode <default|slac-bsas-table>   Processing mode (default: default)
#   --ts-seconds <field>               Timestamp seconds column (required for slac-bsas-table)
#   --ts-nanos <field>                 Timestamp nanoseconds column (required for slac-bsas-table)
#   --output <path>                    Output file path (default: .vscode/config-hdf5-pv-<PV_NAME>.yaml)
#
# Examples:
#   ./scripts/generate-hdf5-config.sh TEST:VOLTAGE
#   ./scripts/generate-hdf5-config.sh CU-HXR --mode slac-bsas-table --ts-seconds secondsPastEpoch --ts-nanos nanoseconds

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

PV_NAME="${1:-}"
if [ -z "$PV_NAME" ]; then
    echo "Usage: $0 <PV_NAME> [--mode <default|slac-bsas-table>] [--ts-seconds <field>] [--ts-nanos <field>] [--output <path>]" >&2
    exit 1
fi
shift

MODE="default"
TS_SECONDS=""
TS_NANOS=""
OUTPUT_PATH=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)
            MODE="$2"; shift 2 ;;
        --ts-seconds)
            TS_SECONDS="$2"; shift 2 ;;
        --ts-nanos)
            TS_NANOS="$2"; shift 2 ;;
        --output)
            OUTPUT_PATH="$2"; shift 2 ;;
        *)
            echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [ -z "$OUTPUT_PATH" ]; then
    OUTPUT_PATH=".vscode/config-hdf5-pv-${PV_NAME}.yaml"
fi

# Validate mode
if [ "$MODE" != "default" ] && [ "$MODE" != "slac-bsas-table" ]; then
    echo "Error: --mode must be 'default' or 'slac-bsas-table'" >&2
    exit 1
fi

# Build the option block (indented to align under the PV name entry)
if [ "$MODE" = "slac-bsas-table" ]; then
    if [ -z "$TS_SECONDS" ] || [ -z "$TS_NANOS" ]; then
        echo "Error: --ts-seconds and --ts-nanos are required for mode 'slac-bsas-table'" >&2
        exit 1
    fi
    PV_OPTION="            option:
              type: \"slac-bsas-table\"
              tsSeconds: \"${TS_SECONDS}\"
              tsNanos: \"${TS_NANOS}\""
else
    PV_OPTION=""
fi

mkdir -p "$(dirname "${OUTPUT_PATH}")"

# Substitute placeholders
sed "s|__PV_NAME__|${PV_NAME}|g" "${ROOT_DIR}/config-hdf5-pv.yaml" \
    | awk -v option="${PV_OPTION}" '
        /^__PV_OPTION__$/ {
            if (option != "") print option
            next
        }
        { print }
    ' > "${OUTPUT_PATH}"

echo "Generated config: ${OUTPUT_PATH} (PV: ${PV_NAME}, mode: ${MODE})"

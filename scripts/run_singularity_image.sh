#!/usr/bin/env bash
# run_singularity_image.sh — Run a command inside the MLDP PVXS Driver Singularity image.
#
# Usage:
#   ./scripts/run_singularity_image.sh <image.sif> [workdir] [-- command args...]
#
# Arguments:
#   image.sif   Path to the Singularity/Apptainer .sif image  (required)
#   workdir     Host directory to bind-mount as /workspace     (default: current directory)
#
# If no command is given after --, an interactive bash shell is opened.
#
# Examples:
#   # Interactive shell, current dir as workspace
#   ./scripts/run_singularity_image.sh mldp-pvxs-driver-env.sif
#
#   # Interactive shell, explicit workdir
#   ./scripts/run_singularity_image.sh mldp-pvxs-driver-env.sif /path/to/source
#
#   # Build the driver
#   ./scripts/run_singularity_image.sh mldp-pvxs-driver-env.sif /path/to/source -- \
#     bash -c "cmake -S /workspace -B /workspace/build -G Ninja \
#              -DCMAKE_BUILD_TYPE=Release && cmake --build /workspace/build"
#
#   # Run tests
#   ./scripts/run_singularity_image.sh mldp-pvxs-driver-env.sif /path/to/source -- \
#     bash -c "cmake -S /workspace -B /workspace/build -G Ninja \
#              -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMLDP_PVXS_DRIVER_TESTS=ON \
#              && cmake --build /workspace/build \
#              && ctest --test-dir /workspace/build --output-on-failure"

set -euo pipefail

# ---- Parse positional args before -- ----------------------------------------
if [[ $# -lt 1 ]]; then
    echo "Usage: $(basename "$0") <image.sif> [workdir] [-- command...]" >&2
    exit 1
fi

IMAGE="$1"; shift

WORKDIR="$(pwd)"
if [[ $# -gt 0 && "$1" != "--" ]]; then
    WORKDIR="$1"; shift
fi

# Consume optional -- separator
if [[ $# -gt 0 && "$1" == "--" ]]; then
    shift
fi

# ---- Validate ---------------------------------------------------------------
if [[ ! -f "$IMAGE" ]]; then
    echo "error: image not found: $IMAGE" >&2; exit 1
fi
if [[ ! -d "$WORKDIR" ]]; then
    echo "error: workdir not found: $WORKDIR" >&2; exit 1
fi

# ---- Detect apptainer / singularity -----------------------------------------
if command -v apptainer >/dev/null 2>&1; then
    RUNNER=apptainer
elif command -v singularity >/dev/null 2>&1; then
    RUNNER=singularity
else
    echo "error: neither 'apptainer' nor 'singularity' found in PATH" >&2; exit 1
fi

WORKDIR="$(cd "$WORKDIR" && pwd)"  # absolute path

echo "Image   : $IMAGE"
echo "Workdir : $WORKDIR -> /workspace"
echo "Runner  : $RUNNER"
echo ""

# ---- Run --------------------------------------------------------------------
if [[ $# -eq 0 ]]; then
    # Interactive shell
    exec "${RUNNER}" run --bind "${WORKDIR}":/workspace "${IMAGE}"
else
    exec "${RUNNER}" run --bind "${WORKDIR}":/workspace "${IMAGE}" "$@"
fi

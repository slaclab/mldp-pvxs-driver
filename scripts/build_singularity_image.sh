#!/usr/bin/env bash
# build_singularity_image.sh — Build the MLDP PVXS Driver Singularity/Apptainer image.
#
# Produces a .sif file containing EPICS Base, PVXS, gRPC, protobuf, and all
# compile-time dependencies. No driver source is embedded.
#
# Usage:
#   ./scripts/build_singularity_image.sh [OPTIONS]
#
# Options:
#   -o, --output FILE       Output .sif path   (default: mldp-pvxs-driver-env.sif)
#   -e, --epics  VERSION    EPICS Base tag      (default: R7.0.8.1)
#   -p, --pvxs   VERSION    PVXS tag            (default: 1.4.1)
#   -c, --compiler gcc|llvm Compiler            (default: gcc)
#       --no-cache          Pass --no-cache to apptainer/singularity build
#   -h, --help              Show this help and exit
#
# Environment overrides (lower priority than CLI flags):
#   OUTPUT_SIF, EPICS_VERSION, PVXS_VERSION, COMPILER
#
# Requires: apptainer (or singularity) with --fakeroot or run as root.
#
# Examples:
#   ./scripts/build_singularity_image.sh
#   ./scripts/build_singularity_image.sh --output /tmp/my-env.sif --no-cache
#   ./scripts/build_singularity_image.sh --epics R7.0.7 --compiler llvm
#
# To compile the driver using the built image (source bind-mounted):
#
#   apptainer run --bind "$(pwd)":/workspace mldp-pvxs-driver-env.sif \
#     bash -c "cmake -S /workspace -B /workspace/build -G Ninja \
#              -DCMAKE_BUILD_TYPE=Release && cmake --build /workspace/build"

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEF_FILE="${REPO_ROOT}/singularity/mldp-pvxs-driver-env.def"

# ---- Defaults ---------------------------------------------------------------
OUTPUT_SIF="${OUTPUT_SIF:-mldp-pvxs-driver-env.sif}"
EPICS_VERSION="${EPICS_VERSION:-R7.0.8.1}"
PVXS_VERSION="${PVXS_VERSION:-1.4.1}"
COMPILER="${COMPILER:-gcc}"
NO_CACHE=""

# ---- Argument parsing -------------------------------------------------------
usage() {
    sed -n '/^# Usage:/,/^[^#]/{ /^#/s/^# \{0,2\}//p }' "$0"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -o|--output)    OUTPUT_SIF="$2";    shift 2 ;;
        -e|--epics)     EPICS_VERSION="$2"; shift 2 ;;
        -p|--pvxs)      PVXS_VERSION="$2";  shift 2 ;;
        -c|--compiler)  COMPILER="$2";      shift 2 ;;
        --no-cache)     NO_CACHE="--no-cache"; shift ;;
        -h|--help)      usage ;;
        *) echo "Unknown option: $1" >&2; usage ;;
    esac
done

# ---- Validate ---------------------------------------------------------------
if [[ "$COMPILER" != "gcc" && "$COMPILER" != "llvm" ]]; then
    echo "error: --compiler must be 'gcc' or 'llvm'" >&2; exit 1
fi

# ---- Detect apptainer / singularity -----------------------------------------
if command -v apptainer >/dev/null 2>&1; then
    BUILDER=apptainer
elif command -v singularity >/dev/null 2>&1; then
    BUILDER=singularity
else
    echo "error: neither 'apptainer' nor 'singularity' found in PATH" >&2
    exit 1
fi
echo "Using builder: $BUILDER ($(${BUILDER} --version))"

# ---- Summary ----------------------------------------------------------------
echo "============================================================"
echo " Building MLDP PVXS Driver Singularity image"
echo "------------------------------------------------------------"
echo "  Output       : ${OUTPUT_SIF}"
echo "  Definition   : ${DEF_FILE}"
echo "  EPICS version: ${EPICS_VERSION}"
echo "  PVXS version : ${PVXS_VERSION}"
echo "  Compiler     : ${COMPILER}"
echo "  No-cache     : ${NO_CACHE:-false}"
echo "============================================================"

# ---- Build ------------------------------------------------------------------
${BUILDER} build \
    ${NO_CACHE} \
    --fakeroot \
    --build-arg "EPICS_VERSION=${EPICS_VERSION}" \
    --build-arg "PVXS_VERSION=${PVXS_VERSION}" \
    --build-arg "COMPILER=${COMPILER}" \
    "${OUTPUT_SIF}" \
    "${DEF_FILE}"

echo ""
echo "Image built: ${OUTPUT_SIF}"
echo ""
echo "Compile the driver (source bind-mounted, not embedded):"
echo ""
echo "  ${BUILDER} run --bind \"\$(pwd)\":/workspace ${OUTPUT_SIF} \\"
echo "    bash -c 'cmake -S /workspace -B /workspace/build -G Ninja \\"
echo "             -DCMAKE_BUILD_TYPE=Release && cmake --build /workspace/build'"

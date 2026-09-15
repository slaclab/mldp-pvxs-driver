#!/usr/bin/env bash
# run_singularity_image.sh — Run a command inside the MLDP PVXS Driver Singularity image.
#
# Usage:
#   ./scripts/run_singularity_image.sh [-b SRC[:DST[:OPTS]]]... <image.sif> [workdir] [-- command args...]
#
# Options:
#   -b, --bind SRC[:DST[:OPTS]]   Extra host path to bind-mount. Repeatable.
#                                 Default mode is read-only (:ro) — appended
#                                 automatically when OPTS is omitted.
#                                 Pass ':rw' explicitly to allow writes:
#                                   -b /host/out:/out:rw
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
#   # Interactive shell with two extra host paths mounted
#   ./scripts/run_singularity_image.sh -b /tmp/data:/data -b /opt/tools:/tools \
#     mldp-pvxs-driver-env.sif /path/to/source
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

# ---- Parse optional flags ---------------------------------------------------
EXTRA_BINDS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--bind)
            if [[ $# -lt 2 ]]; then
                echo "error: $1 requires a value" >&2; exit 1
            fi
            EXTRA_BINDS+=("$2"); shift 2 ;;
        --bind=*)
            EXTRA_BINDS+=("${1#--bind=}"); shift ;;
        -h|--help)
            sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) break ;;
    esac
done

# ---- Parse positional args before -- ----------------------------------------
if [[ $# -lt 1 ]]; then
    echo "Usage: $(basename "$0") [-b SRC[:DST[:OPTS]]]... <image.sif> [workdir] [-- command...]" >&2
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

for b in "${EXTRA_BINDS[@]}"; do
    src="${b%%:*}"
    if [[ ! -e "$src" ]]; then
        echo "error: bind source not found: $src (from --bind $b)" >&2; exit 1
    fi
done

# ---- Detect apptainer / singularity -----------------------------------------
if command -v apptainer >/dev/null 2>&1; then
    RUNNER=apptainer
elif command -v singularity >/dev/null 2>&1; then
    RUNNER=singularity
else
    echo "error: neither 'apptainer' nor 'singularity' found in PATH" >&2; exit 1
fi

WORKDIR="$(cd "$WORKDIR" && pwd)"  # absolute path

# ---- Assemble bind args -----------------------------------------------------
# Extra binds default to read-only. Normalize each entry so it ends in :ro
# unless caller supplied explicit OPTS (:ro or :rw).
#   SRC                -> SRC:SRC:ro
#   SRC:DST            -> SRC:DST:ro
#   SRC:DST:ro|rw|...  -> unchanged
BIND_ARGS=(--bind "${WORKDIR}:/workspace")
NORMALIZED_BINDS=()
for b in "${EXTRA_BINDS[@]}"; do
    colons="${b//[^:]/}"
    case ${#colons} in
        0) b="${b}:${b}:ro" ;;
        1) b="${b}:ro" ;;
        *) : ;;  # OPTS already present
    esac
    BIND_ARGS+=(--bind "$b")
    NORMALIZED_BINDS+=("$b")
done

echo "Image   : $IMAGE"
echo "Workdir : $WORKDIR -> /workspace"
for b in "${NORMALIZED_BINDS[@]}"; do
    echo "Bind    : $b"
done
echo "Runner  : $RUNNER"
echo ""

# ---- Run --------------------------------------------------------------------
if [[ $# -eq 0 ]]; then
    # Interactive shell
    exec "${RUNNER}" run "${BIND_ARGS[@]}" "${IMAGE}"
else
    exec "${RUNNER}" run "${BIND_ARGS[@]}" "${IMAGE}" "$@"
fi

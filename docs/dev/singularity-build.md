# Building with Singularity / Apptainer

For HPC environments where Docker is unavailable, the repository ships a
Singularity definition file that builds a self-contained build-environment image
containing EPICS Base, PVXS, gRPC, protobuf, and all compile-time dependencies.
No driver source is embedded in the image — the source tree is always bind-mounted
at runtime.

## Files

| File | Purpose |
|------|---------|
| `singularity/mldp-pvxs-driver-env.def` | Singularity definition (Rocky Linux 9.3 base) |
| `scripts/build_singularity_image.sh` | Wrapper script — builds the `.sif` and prints usage |

## Building the Image

```bash
# Default (EPICS R7.0.8.1, PVXS 1.4.1, gcc)
./scripts/build_singularity_image.sh

# Custom output path
./scripts/build_singularity_image.sh --output /scratch/mldp-env.sif

# Different EPICS or compiler
./scripts/build_singularity_image.sh --epics R7.0.7 --compiler llvm --no-cache
```

### Options

| Flag | Default | Description |
|------|---------|-------------|
| `-o, --output FILE` | `mldp-pvxs-driver-env.sif` | Output `.sif` path |
| `-e, --epics VERSION` | `R7.0.8.1` | EPICS Base git tag |
| `-p, --pvxs VERSION` | `1.4.1` | PVXS git tag |
| `-c, --compiler gcc\|llvm` | `gcc` | Compiler inside the image |
| `--no-cache` | off | Pass `--no-cache` to apptainer/singularity build |

Environment variables `EPICS_VERSION`, `PVXS_VERSION`, `COMPILER`, `OUTPUT_SIF` can be
set as overrides (CLI flags take precedence).

> **Note:** Building requires `apptainer` or `singularity` with `--fakeroot` support, or
> root access. Ask your HPC sysadmin to enable fakeroot if needed.

## Compiling the Driver

Mount the source tree and run CMake inside the image:

```bash
apptainer run --bind "$(pwd)":/workspace mldp-pvxs-driver-env.sif \
  bash -c "cmake -S /workspace -B /workspace/build -G Ninja \
           -DCMAKE_BUILD_TYPE=Release \
           && cmake --build /workspace/build"
```

The build output lands in `./build/` on the host (bind-mounted path).

## Running Tests

```bash
apptainer run --bind "$(pwd)":/workspace mldp-pvxs-driver-env.sif \
  bash -c "cmake -S /workspace -B /workspace/build -G Ninja \
           -DCMAKE_BUILD_TYPE=RelWithDebInfo \
           -DMLDP_PVXS_DRIVER_TESTS=ON \
           && cmake --build /workspace/build \
           && ctest --test-dir /workspace/build --output-on-failure"
```

## See Also

- [Architecture Overview](../reference/architecture.md)
- [Configuration Reference](../guides/configuration.md)

# Contributing to `mldp-pvxs-driver`

This file is the contributor workflow guide.
For system behavior and feature details, use the topic links below.

## Start Here: Documentation Map

| If you need...                                                                         | Read...                                                          |
|----------------------------------------------------------------------------------------|------------------------------------------------------------------|
| Product/runtime overview, configuration, CLI, release artifacts, external dependencies | [README.md](README.md)                                           |
| Architecture, data flow, threading model, reader abstraction internals                 | [docs/architecture.md](docs/architecture.md)                     |
| How to configure and use built-in readers (`epics-base`, `epics-pvxs`)                 | [docs/readers.md](docs/readers.md)                               |
| How to implement a new/custom reader type                                              | [docs/readers-implementation.md](docs/readers-implementation.md) |
| Metrics export format and runtime metrics dump behavior                                | [docs/metrics-export-guide.md](docs/metrics-export-guide.md)     |

## Local Development Workflow

1. Create a branch from `main`.
2. Keep changes focused and atomic.
3. Run relevant builds/tests locally.
4. Open a PR to `main` with rationale and verification steps.

## Build and Test Before PR

Prerequisites and environment details are in [README.md](README.md).

```bash
# configure + build
cmake -S . -B build \
  -DPROTO_PATH=/path/to/protos \
  -DPVXS_BASE=/path/to/pvxs \
  -DEPICS_BASE=/path/to/epics \
  -DEPICS_HOST_ARCH=linux-x86_64
cmake --build build --parallel

# enable tests and run
cmake -S . -B build \
  -DMLDP_PVXS_DRIVER_TESTS=ON \
  -DPROTO_PATH=/path/to/protos \
  -DPVXS_BASE=/path/to/pvxs
cmake --build build --target mldp_pvxs_driver_test
ctest --test-dir build --output-on-failure
```

## Contribution Expectations

- Add or update tests for behavior changes where feasible.
- Keep config/schema/API changes explicit in the PR description.
- If architecture-affecting, reference the relevant section in [docs/architecture.md](docs/architecture.md).
- If reader behavior/config changes, update [docs/readers.md](docs/readers.md).
- If adding/extending a reader type, update [docs/readers-implementation.md](docs/readers-implementation.md).
- If metrics behavior/output changes, update [docs/metrics-export-guide.md](docs/metrics-export-guide.md).

## License Header Workflow

When adding new headers/sources or before opening a PR, run:

```bash
./scripts/add-licenses-include-h.sh
```

Notes:

- Script is idempotent and safe to rerun.
- It uses `.nwa-config.yaml` and `/usr/local/bin/nwa`.

## Quick Checklist

- [ ] Branch created from `main`
- [ ] Build succeeds
- [ ] Relevant tests pass
- [ ] Docs updated in the correct target file(s)
- [ ] License headers applied where required
- [ ] PR description includes rationale + verification

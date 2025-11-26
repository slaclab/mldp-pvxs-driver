# Developer Guide

## Building

`PROTO_PATH` and `PVXS_BASE` are required to either be set as environment variables or passed to the CMake configuration
step. The former should be the path to the parent directory of MLDP's protobuf definitions. The latter should be the
directory containing the pvxs library.

EPICS Base and pvxs are expected to be available in the development container under `/opt/local/lib/linux-<arch>`, but
can be overridden explicitly at configure time:

```
cmake -S . -B build \
  -DPROTO_PATH=/workspace/protos \
  -DEPICS_BASE=/opt/local \
  -DEPICS_HOST_ARCH=linux-aarch64 \
  -DPVXS_BASE=/opt/local   # or /opt/pvxs if you only built pvxs without installing
cmake --build build
```

## Debugging

When debugging inside the dev container, the LLVM `lldb-dap` adapter from the bundled extension needs an explicit path.
Set **LLDB DAP › Executable: Path** in VS Code to `/usr/bin/lldb-dap-18` (Preferences → Settings → Extensions → LLDB DAP).
Alternatively, add the following to `.vscode/settings.json` within this repository:

```jsonc
{
  "lldb.executable": "/usr/bin/lldb-dap-18"
}
```

To make this automatic for everyone using the dev container, you can bake the setting into `.devcontainer/devcontainer.json`:

```jsonc
{
  "customizations": {
    "vscode": {
      "settings": {
        "lldb.executable": "/usr/bin/lldb-dap-18"
      }
    }
  }
}
```

Rebuild the dev container after changing the configuration so the setting is applied on startup.

## Workflow
- Branch off of `main` whenever you start work on a feature, bug fix, or any other change set so the base line of history matches what you will eventually merge back.
- Keep each branch focused on a logical piece of functionality, then open a pull request back into `main` when the work is ready for review.
- Ensure the PR clearly describes the change, includes any relevant testing steps, and waits for approval before merging; rebasing on `main` to resolve conflicts keeps the merge clean.

## Building and Testing
- The project is configured with CMake. Create a build directory, point CMake at the protobuf (`PROTO_PATH`) and PVXS (`PVXS_BASE`) roots, and optionally override `EPICS_BASE`/`EPICS_HOST_ARCH` if the defaults in the container do not match your environment.

  ```sh
  cmake -S . -B build \
    -DPROTO_PATH=/workspace/protos \
    -DEPICS_BASE=/opt/local \
    -DEPICS_HOST_ARCH=linux-aarch64 \
    -DPVXS_BASE=/opt/local
  cmake --build build
  ```

- Run the available CTest targets via `cmake --build build --target test` or `ctest --output-on-failure` from the build directory once the build succeeds so regressions are detected early.

## Code Style
- A `.clang-format` file at the project root defines the LLVM-derived style used in the repository. Make your edits and run `clang-format -i path/to/files` to keep the formatting consistent before committing; IDE integrations or `git clang-format` can automate this step.
- Prefer the existing include order, naming, and spacing patterns already present in `src/`, `include/`, and the CMake files to keep the history readable.

## Tips
- If you hit linker or dependency issues, double-check that `PROTO_PATH` and `PVXS_BASE` correctly point to the same SDK versions referenced in this repository’s Docker/dev container setup.
- Keep configuration changes (e.g., YAML driver configs) separate from code logic in their own commits so reviewers can focus on one area per review pass.

# Phase 0 — Dependencies + Build System

← [Back to main plan](query-client-impl.md)

## Tasks

- [ ] Add `arrow` and `arrow_flight` to CMake (`find_package(Arrow)` / `FetchContent`); verify Rocky Linux 9 GCC build (no Clang C++20 issues)
- [ ] Add `arrow::fs::LocalFileSystem`, `arrow::ipc`, `arrow::compute` targets to CMakeLists link sets
- [ ] Add `arrow::fs::MockFileSystem` to test targets only
- [ ] Verify `arrow::MemoryPool` linkage; confirm `arrow::default_memory_pool()` available

## Notes

- Build must stay GCC-only — Rocky Linux 9 Clang has incomplete C++20 `<format>`
- Arrow C++ version must match what is available in the Rocky Linux 9 GCC toolchain or be fetched via `FetchContent`
- `arrow_flight` only needed for Phase 7 — link it optionally so it doesn't bloat the default binary

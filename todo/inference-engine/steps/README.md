# Inference Engine — Execution Steps

Each step is self-contained: compiles, links, and all tests pass before moving to the next.
Steps must be executed in order unless marked independent.

| Step | File | What | Depends On | New Tests | Status |
|---|---|---|---|---|---|
| 01 | [step-01-enums-and-snapshot.md](./step-01-enums-and-snapshot.md) | AlignmentPolicy, TriggerPolicy, AlignedSnapshot, AlgorithmOutput headers | — | none | completed |
| 02 | [step-02-ialgorithm-and-config.md](./step-02-ialgorithm-and-config.md) | IAlgorithm interface + MLDPChannelProcessorConfig parser | 01 | MLDPChannelProcessorConfigTest (11 cases) | completed |
| 03 | [step-03-input-buffer.md](./step-03-input-buffer.md) | InputBuffer: slots, freshness, AnyUpdate/AllUpdated/Interval snapshots | 01 | InputBufferTest (10 cases) | completed |
| 04 | [step-04-ichannelprocessor-and-factory.md](./step-04-ichannelprocessor-and-factory.md) | IChannelProcessor interface + ChannelProcessorFactory + REGISTER_ALGORITHM macro | 02 | none (compile-time verification) | completed |
| 05 | [step-05-channel-processor.md](./step-05-channel-processor.md) | ChannelProcessor base: push, fireCompute, AnyUpdate/AllUpdated | 01–04 | ChannelProcessorTest (9 cases) | completed |
| 06 | [step-06-linear-transform-algorithm.md](./step-06-linear-transform-algorithm.md) | LinearTransformAlgorithm + controller processors: wiring (MLDPPVXSControllerConfig + MLDPPVXSController) | 01–05 | LinearTransformAlgorithmTest (6) + integration test | completed |
| 07 | [step-07-moving-average-algorithm.md](./step-07-moving-average-algorithm.md) | MovingAverageAlgorithm + IAlgorithm::reset() | 01–06 | MovingAverageAlgorithmTest (8 cases) | completed |
| 08 | [step-08-interval-trigger.md](./step-08-interval-trigger.md) | Interval trigger worker thread in ChannelProcessor | 01–07 | ChannelProcessorIntervalTest (5 cases) |
| 09 | [step-09-interpolate-alignment.md](./step-09-interpolate-alignment.md) | Interpolate alignment policy in InputBuffer | 03 (independent of 06–08) | 7 new InputBuffer tests |
| 10 | [step-10-echo-algorithm.md](./step-10-echo-algorithm.md) | EchoAlgorithm (BUILD_ECHO_PROCESSOR=ON, optional) | 01–06 | EchoAlgorithmTest (4 cases, gated) | completed |
| 11 | [step-11-python-processor.md](./step-11-python-processor.md) | PythonAlgorithm + PythonScriptDirectoryLoader (BUILD_PYTHON_PROCESSOR=ON) | 01–06 | PythonAlgorithmTest (8) + PythonScriptDirectoryLoaderTest (5), gated | completed |
| 12 | [step-12-hardening.md](./step-12-hardening.md) | Metrics, back-pressure, output-source collision check, cycle detection | 01–11 | various (see step) |

## Lua Processor

Lua implementation is deferred. See [lua-processor-plan.md](./lua-processor-plan.md) when ready.
Follows same directory-loader pattern as Python (Step 11).

## Architecture Reference

- [implementation-plan.md](./implementation-plan.md) — full architecture, class diagrams, YAML examples
- [python-processor-plan.md](./python-processor-plan.md) — Python processor detail

## Key Post-Refactor Rules (applies to all steps)

- `EventBatchStruct` has **no** `root_source` field. Identity lives inside payload variant:
  - `TimeSeriesPayload::root_source_name`
  - `SourceMetadataPayload::root_source_name`
  - `ConfigurationPayload::root_source_name`
  - `ConfigurationActivationPayload::configuration_name`
- `IWriter` has **no** `acceptsSource(string)`. Source filtering = `RouteTable::acceptsSource()`.
- Use `getRootSourceName(batch)` helper from `IDataBus.h` for generic source extraction.
- Algorithm `compute()` must set `root_source_name` (or `configuration_name`) inside returned payload.

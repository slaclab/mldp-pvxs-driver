# TODO: Missing Tests — feature/pv-metadata Branch

Tests left over from work done in this branch. Grouped by component.

---

## 1. MLDPAnnotationWriter

**No test file exists.** Create `test/writer/mldp_annotation/mldp_annotation_writer_test.cpp`.

| Test case | What to verify |
|---|---|
| `WriterFactoryCreatesAnnotationWriter` | `WriterFactory::create("mldp-annotation", cfg, nullptr)` returns non-null |
| `AcceptsOnlySourceMetadataPayload` | `acceptsPayload(SourceMetadataPayload{})` → true; false for `TimeSeriesPayload`, `ConfigurationPayload`, `ConfigurationActivationPayload` |
| `PushSourceMetadataCallsSavePvMetadata` | Start real gRPC server; push batch with `SourceMetadataPayload{{"MY:PV", {...}}}` ; verify `savePvMetadata` RPC called with correct source name and fields |
| `PushNonMetadataPayloadIsIgnored` | Push `TimeSeriesPayload` batch → returns true, no RPC call |
| `GracefulOnUnreachableEndpoint` | Construct with unreachable URL; `push()` does not throw or crash |
| `StartStopLifecycle` | `start()` then `stop()` without push — no crash, `isHealthy()` transitions correctly |

**Key files:** `include/writer/mldp_annotation/MLDPAnnotationWriter.h`, `src/writer/mldp_annotation/MLDPAnnotationWriter.cpp`
**gRPC call:** `stub->savePvMetadata()`

---

## 2. MLDPConfigurationWriter

**No test file exists.** Create `test/writer/mldp_configuration/mldp_configuration_writer_test.cpp`.

| Test case | What to verify |
|---|---|
| `WriterFactoryCreatesConfigurationWriter` | `WriterFactory::create("mldp-configuration", cfg, nullptr)` returns non-null |
| `AcceptsConfigurationAndActivationPayloads` | `acceptsPayload(ConfigurationPayload{})` → true; `acceptsPayload(ConfigurationActivationPayload{})` → true; false for `TimeSeriesPayload`, `SourceMetadataPayload` |
| `PushConfigurationPayloadCallsSaveConfiguration` | Start real gRPC server; push batch with `ConfigurationPayload`; verify `saveConfiguration` RPC called with correct fields |
| `PushActivationPayloadCallsSaveConfigurationActivation` | Push batch with `ConfigurationActivationPayload`; verify `saveConfigurationActivation` RPC called |
| `PushNonConfigurationPayloadIsIgnored` | Push `TimeSeriesPayload` → returns true, no RPC call |
| `GracefulOnUnreachableEndpoint` | Construct with unreachable URL; `push()` does not throw or crash |
| `StartStopLifecycle` | `start()` then `stop()` — no crash |

**Key files:** `include/writer/mldp_configuration/MLDPConfigurationWriter.h`, `src/writer/mldp_configuration/MLDPConfigurationWriter.cpp`
**gRPC calls:** `stub->saveConfiguration()`, `stub->saveConfigurationActivation()`

---

## 3. MLDPWriter — Gaps in Existing Tests

File: `test/writer/mldp/mldp_writer_integration_test.cpp`

| Test case | What to verify |
|---|---|
| `AcceptsOnlyTimeSeriesPayload` | `acceptsPayload(TimeSeriesPayload{})` → true; false for `SourceMetadataPayload`, `ConfigurationPayload`, `ConfigurationActivationPayload` |
| `BatchMetadataAppearsAsColumnAttributes` | Push batch with `metadata={facility:lcls, signal_type:scalar}`; verify captured gRPC `doublecolumns[0].metadata.attributes` contains both pairs (currently only in controller integration test, not in MLDPWriter unit tests) |

---

## 4. Controller — Payload Routing via `acceptsPayload`

File: `test/controller/mldppvxs_controller_mldp_writer_integration_test.cpp`

| Test case | What to verify |
|---|---|
| `AnnotationPayloadRoutedOnlyToAnnotationWriter` | Add both an `mldp` writer and an `mldp-annotation` writer to the controller; push a `SourceMetadataPayload` batch; verify only the annotation writer receives it (mldp writer gets no RPC call) |
| `ConfigurationPayloadRoutedOnlyToConfigurationWriter` | Same setup with `mldp-configuration` writer; push `ConfigurationPayload`; verify only configuration writer's `saveConfiguration` called |
| `TimeSeriesPayloadNotRoutedToAnnotationWriter` | Push `TimeSeriesPayload`; verify annotation writer's `savePvMetadata` is NOT called |

---

## 5. Config Parsing — New Writer Types

File: `test/config/mldppvxs_controller_config_test.cpp`

| Test case | What to verify |
|---|---|
| `ParsesAnnotationWriterConfig` | YAML with `writer.mldp-annotation[].mldp-annotation-pool` parses without error; `annotation-url`, `min-conn`, `max-conn` extracted correctly |
| `ParsesConfigurationWriterConfig` | YAML with `writer.mldp-configuration[].mldp-annotation-pool` parses without error |
| `ThrowsWhenAnnotationUrlMissing` | `annotation-url` absent → validation error |

---

## 6. Reader Metadata — epics-base and epics-archiver

The PVXS reader has `StaticAndPerPvMetadataMergedIntoEventBatch` (added this branch). Equivalent tests missing for the other two reader types.

### epics-base reader
File: `test/reader/impl/epics/base/epics_base_reader_test.cpp`

| Test case | What to verify |
|---|---|
| `StaticAndPerPvMetadataMergedIntoEventBatch` | Same as PVXS variant: reader-level `metadata` + per-PV `metadata` merged; per-PV wins on conflict |

### epics-archiver reader
File: `test/reader/impl/epics_archiver/epics_archiver_reader_http_integration_test.cpp`

| Test case | What to verify |
|---|---|
| `StaticAndPerPvMetadataMergedIntoEventBatch` | Push batch via mock HTTP server; verify `batch.metadata` contains merged reader + per-PV metadata with correct override semantics |

---

## Reference

- Payload variant types: `include/util/bus/IDataBus.h`
- `acceptsPayload` contract: `include/writer/IWriter.h`
- Writer implementations: `src/writer/mldp_annotation/`, `src/writer/mldp_configuration/`
- Architecture: `docs/reference/architecture.md`

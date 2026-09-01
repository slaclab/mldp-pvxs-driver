
  # Epics Archiver Reader Batching Plan

  ## Summary

  Update `EpicsArchiverReader` so samples from multiple PVs can remain in memory and be submitted to the data bus as grouped batches.

  A batch is flushed when either:

  - It contains the configured number of samples per PV.
  - It has been pending for the configured flush interval.

  The flush operation must attempt to submit every currently available pending batch.

  ## Configuration

  Add these reader configuration values:

  - `pv_samples_per_batch`: maximum number of samples collected for each PV in one batch.
  - `batch_flush_interval_ms`: maximum time, in milliseconds, that an incomplete batch may remain pending.

  The existing reader configuration and behavior should remain compatible where possible. Invalid or non-positive values should be rejected during
  configuration parsing or normalized to safe defaults according to existing project conventions.

  ## Batching Behavior

  Maintain pending batches in memory, keyed by PV.

  For each PV returned by the archiver:

  1. Add newly received samples to that PV’s pending batch.
  2. When the pending batch reaches `pv_samples_per_batch`, submit it to the bus.
  3. Remove the submitted batch from the pending collection.
  4. Keep any remaining samples as the next pending batch for that PV.

  A single reader may manage multiple PVs concurrently. Each PV must have independent pending-batch state so samples from different PVs are not
  mixed together.

  ## Timed Flush

  Track when each pending batch was created or last flushed.

  When the configured flush interval expires:

  1. Iterate over all pending PV batches.
  2. Submit every non-empty batch that is ready to flush.
  3. Remove successfully submitted batches from the pending collection.
  4. Preserve batches when submission fails, so they can be retried according to existing bus/error-handling behavior.

  The flush operation must process all available PV batches in one invocation rather than flushing only the first eligible batch.

  The reader shutdown path must flush remaining non-empty batches before releasing resources, subject to the existing stop/error semantics.

  ## Thread Safety and Lifecycle

  Protect pending-batch state and flush timing consistently with the reader’s existing threading model.

  Ensure that:

  - Sample collection and timed flushing cannot lose or duplicate samples.
  - A batch cannot be submitted concurrently by both the size-triggered and timer-triggered paths.
  - Timer or background-flush activity stops cleanly when the reader stops.
  - Pending batches do not outlive the reader or reference invalid configuration/state.

  ## Tests

  Add or update tests covering:

  - Samples reaching `pv_samples_per_batch` trigger immediate submission.
  - Incomplete batches remain in memory.
  - The flush interval submits incomplete batches.
  - One flush submits incomplete batches for multiple PVs.
  - Samples from different PVs remain separated.
  - Multiple full batches from one PV are submitted correctly.
  - Samples remaining after a full batch are retained for the next batch.
  - Shutdown flushes pending samples.
  - Bus submission failures preserve pending data according to existing error semantics.
  - Configuration parsing accepts the new fields and handles invalid values consistently.

  ## Verification

  Run the focused Epics Archiver reader tests first, then the full test suite:

  ```bash
  cmake --build build --target mldp_pvxs_driver_test --parallel
  ctest --test-dir build -R "EpicsArchiverReader" --output-on-failure
  ctest --test-dir build --output-on-failure

  If the local dependencies are unavailable, run the same commands inside the project devcontainer as documented in AGENTS.md.

  ## Assumptions

  - Batches are grouped independently per PV.
  - pv_samples_per_batch limits the number of samples for one PV, not the total number of samples across all PVs.
  - The flush interval applies to incomplete pending batches.
  - The existing EventBatch and IDataBus interfaces remain unchanged unless implementation inspection shows a required compatibility adjustment.
//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <util/bus/IDataBus.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace mldp_pvxs_driver::writer {

/**
 * @brief Pure abstract interface that every output destination must implement.
 *
 * Writers are the sink side of the driver: they receive batches of ingestion
 * events produced by readers and forward them to a concrete back-end (gRPC,
 * HDF5, etc.).  The interface mirrors the reader-side pattern under
 * `include/reader/`.
 *
 * Lifecycle contract:
 * 1. Construct the concrete writer (config-only, no I/O).
 * 2. Call @ref start — may throw `std::runtime_error` on a hard failure
 *    (e.g. unable to connect or create output directory).
 * 3. Call @ref push from reader/bus threads as data arrives.
 * 4. Call @ref stop (noexcept) to drain and tear down resources.
 *
 * Threading:
 * - @ref push must be safe to call concurrently from multiple threads.
 * - @ref start and @ref stop are called from a single owner thread.
 */
class IWriter {
public:
    virtual ~IWriter() = default;

    /**
     * @brief Human-readable name of this writer (e.g. "grpc", "hdf5").
     */
    virtual std::string name() const = 0;

    /**
     * @brief Start the writer and allocate runtime resources.
     *
     * @throws std::runtime_error when the writer cannot be initialised
     *         (e.g. cannot reach a server or create output directories).
     */
    virtual void start() = 0;

    /**
     * @brief Accept a batch of events for delivery.
     *
     * Implementations must not throw; internal errors should be logged and
     * reported via metrics.  Returning `false` indicates back-pressure
     * (e.g. internal queue full) and the caller may log/count the drop.
     *
     * @param batch Event batch to deliver.  Ownership is transferred on
     *              the last write; for all-but-last writers the caller
     *              passes a copy so each writer receives its own data.
     * @return true  if the batch was accepted for delivery.
     * @return false if the batch was dropped (queue full, writer stopped, …).
     */
    virtual bool push(util::bus::IDataBus::EventBatch batch) noexcept = 0;

    /**
     * @brief Stop the writer, drain pending work, and release resources.
     *
     * Must be idempotent and noexcept.
     */
    virtual void stop() noexcept = 0;

    /**
     * @brief Optional health probe.
     *
     * @return true (default) — healthy; override to expose deeper state.
     */
    virtual bool isHealthy() const noexcept { return true; }
};

/// Convenience alias for unique ownership of a writer.
using IWriterUPtr = std::unique_ptr<IWriter>;

} // namespace mldp_pvxs_driver::writer

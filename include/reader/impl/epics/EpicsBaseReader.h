//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/**
 * @file EpicsBaseReader.h
 * @brief EPICS Base (pvaClient) reader implementation.
 *
 * This header provides the EpicsBaseReader class, which implements EPICS PV
 * monitoring using the EPICS Base pvaClient library with a polling-based
 * approach. This reader is an alternative to EpicsPVXSReader for environments
 * where EPICS Base compatibility is required.
 */

#pragma once

#include <reader/ReaderFactory.h>
#include <reader/impl/epics/EpicsBaseMonitorPoller.h>
#include <reader/impl/epics/EpicsReaderBase.h>

#include <mutex>

namespace mldp_pvxs_driver::reader::impl::epics {

/**
 * @brief EPICS reader implementation using EPICS Base pvaClient library.
 *
 * This class implements the Reader interface for monitoring EPICS Process
 * Variables using the EPICS Base pvaClient library. It uses a polling-based
 * monitoring approach via EpicsBaseMonitorPoller, which can be advantageous
 * in high-throughput scenarios.
 *
 * The reader supports both standard scalar/array PVs and SLAC BSAS NTTable
 * structures with per-row timestamps, configured through the reader's YAML
 * configuration.  In BSAS mode each NTTable column is a PV name; two fixed
 * columns hold the per-row timestamps.  See @c docs/readers/slac-bsas-table.md
 * for the full structure description and a concrete annotated example.
 *
 * Configuration example:
 * @code{.yaml}
 * readers:
 *   - type: epics-base
 *     name: my-base-reader
 *     pvs:
 *       - name: "MY:PV:NAME"
 *       - name: "MY:BSAS:TABLE"
 *         option:
 *           type: slac-bsas-table
 * @endcode
 *
 * @note For PVXS-based monitoring, use EpicsPVXSReader instead.
 *
 * @see EpicsReaderBase for common reader functionality
 * @see EpicsBaseMonitorPoller for the underlying polling mechanism
 * @see EpicsPVXSReader for the PVXS-based alternative
 */
class EpicsBaseReader : public EpicsReaderBase
{
public:
    /**
     * @brief Construct an EPICS Base reader from configuration.
     *
     * Initializes the reader with the specified event bus, metrics collector,
     * and configuration. Starts monitoring all PVs specified in the configuration.
     *
     * @param bus Event bus for publishing converted PV data.
     * @param metrics Metrics collector for instrumentation (may be null).
     * @param cfg Reader configuration containing name and PV definitions.
     *
     * @throws EpicsReaderConfig::Error if configuration is invalid.
     */
    EpicsBaseReader(std::shared_ptr<util::bus::IDataBus> bus,
                    std::shared_ptr<metrics::Metrics>         metrics,
                    const config::Config&                     cfg);

    /**
     * @brief Destructor - stops monitoring and releases resources.
     */
    ~EpicsBaseReader() override;

private:
    /**
     * @brief Add PVs to the monitor.
     *
     * Creates subscriptions for the specified PV names through the
     * underlying EpicsBaseMonitorPoller.
     *
     * @param pvNames Set of PV names to monitor.
     */
    void addPV(const PVSet& pvNames);

    /**
     * @brief Drain queued updates from the poller.
     *
     * Called when the poller signals new data availability. Retrieves all
     * queued updates and dispatches them for processing.
     */
    void drainEpicsBaseQueue();

    /**
     * @brief Process a single PV update event.
     *
     * Looks up the runtime mode for @p pvName and dispatches to
     * processDefaultMode() or processSlacBsasTableMode(). Records
     * processing-time and event-count metrics and catches exceptions so
     * that a single bad update cannot disrupt the monitoring loop.
     *
     * @param pvName      Name of the PV that produced the update.
     * @param epics_value The pvData structure containing the update.
     */
    void processEvent(std::string pvName, ::epics::pvData::PVStructurePtr epics_value);

    /**
     * @brief Handle a PV update in Default (scalar/array) mode.
     *
     * Extracts the "value" and "timeStamp" sub-fields from @p epicsValue.
     * Falls back to wall-clock time when the timestamp is absent.
     * Compound (struct) value fields are rejected with a warning.
     * On success one EventBatch is pushed to the bus and @p emitted is set to 1.
     *
     * @param pvName      Name of the source PV.
     * @param epicsValue  Raw pvData structure received from EPICS Base.
     * @param emitted     Set to 1 if an event was published, 0 otherwise.
     */
    void processDefaultMode(const std::string&                     pvName,
                            const ::epics::pvData::PVStructurePtr& epicsValue,
                            std::size_t&                           emitted);

    /**
     * @brief Handle a PV update in SlacBsasTable (NTTable row-timestamp) mode.
     *
     * Delegates conversion to EpicsPVDataConversion::tryBuildNtTableRowTsBatch.
     * Columns are flushed to the bus in batches of at most
     * config_.columnBatchSize() entries to bound memory usage for wide tables.
     * @p emitted receives the total number of data rows published.
     *
     * @param pvName      Name of the source NTTable PV.
     * @param epicsValue  Raw pvData structure received from EPICS Base.
     * @param runtimeCfg  Per-PV runtime config supplying timestamp field names
     *                    (may be null, in which case defaults are used).
     * @param emitted     Accumulates the number of rows emitted.
     */
    void processSlacBsasTableMode(const std::string&                     pvName,
                                  const ::epics::pvData::PVStructurePtr& epicsValue,
                                  const PVRuntimeConfig*                 runtimeCfg,
                                  std::size_t&                           emitted);

    std::unique_ptr<EpicsBaseMonitorPoller> epics_base_poller_;    ///< Underlying polling monitor.
    std::mutex                              epics_base_drain_mutex_; ///< Serializes drain operations.

    REGISTER_READER("epics-base", EpicsBaseReader)
};

} // namespace mldp_pvxs_driver::reader::impl::epics

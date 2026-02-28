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
 * structures with per-row timestamps, configured through the reader's
 * YAML configuration.
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
     * Converts the EPICS pvData structure to MLDP format and publishes
     * it to the event bus. Handles both standard PVs and NTTable structures
     * based on the PV's runtime configuration.
     *
     * @param pvName Name of the PV that produced the update.
     * @param epics_value The pvData structure containing the update.
     */
    void processEvent(std::string pvName, ::epics::pvData::PVStructurePtr epics_value);

    std::unique_ptr<EpicsBaseMonitorPoller> epics_base_poller_;    ///< Underlying polling monitor.
    std::mutex                              epics_base_drain_mutex_; ///< Serializes drain operations.

    REGISTER_READER("epics-base", EpicsBaseReader)
};

} // namespace mldp_pvxs_driver::reader::impl::epics

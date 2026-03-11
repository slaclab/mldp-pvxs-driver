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
 * @file EpicsPVXSReader.h
 * @brief PVXS-based EPICS reader implementation.
 *
 * This header provides the EpicsPVXSReader class, which implements EPICS PV
 * monitoring using the modern PVXS library. This is the recommended reader
 * implementation for new deployments due to PVXS's improved performance and
 * cleaner API compared to EPICS Base pvaClient.
 */

#pragma once

#include <reader/ReaderFactory.h>
#include <reader/impl/epics/shared/EpicsReaderBase.h>

#include <pvxs/client.h>

#include <prometheus/labels.h>

#include <mutex>
#include <string_view>

namespace mldp_pvxs_driver::reader::impl::epics {

/**
 * @brief EPICS reader implementation using the PVXS library.
 *
 * This class implements the Reader interface for monitoring EPICS Process
 * Variables using the PVXS library. It uses event-driven subscriptions that
 * trigger callbacks when PV values change, providing efficient and responsive
 * data acquisition.
 *
 * The reader supports two processing modes:
 * - **Default mode**: Standard scalar/array PVs with structure-level timestamps
 * - **SlacBsasTable mode**: SLAC BSAS NTTable structures with per-row timestamps.
 *   Each NTTable column is a PV name; two fixed columns hold the per-row
 *   timestamps.  See @c docs/readers/slac-bsas-table.md for the full structure
 *   description and a concrete annotated example.
 *
 * Events are processed asynchronously using a thread pool to avoid blocking
 * the PVXS event loop. Metrics are collected for monitoring throughput,
 * latency, and error rates.
 *
 * Configuration example:
 * @code{.yaml}
 * readers:
 *   - type: epics-pvxs
 *     name: my-pvxs-reader
 *     threadPoolSize: 4
 *     pvs:
 *       - name: "MY:PV:NAME"
 *       - name: "MY:BSAS:TABLE"
 *         option:
 *           type: slac-bsas-table
 *           tsSeconds: secondsPastEpoch
 *           tsNanos: nanoseconds
 * @endcode
 *
 * @note This reader requires PVXS library support. For environments requiring
 *       EPICS Base compatibility, use EpicsBaseReader instead.
 *
 * @see EpicsReaderBase for common reader functionality
 * @see EpicsBaseReader for EPICS Base pvaClient alternative
 * @see EpicsMLDPConversion for value conversion details
 */
class EpicsPVXSReader : public EpicsReaderBase
{
public:
    /**
     * @brief Construct a PVXS reader from configuration.
     *
     * Initializes the PVXS client context from environment variables and
     * starts monitoring all PVs specified in the configuration.
     *
     * @param bus Event bus for publishing converted PV data.
     * @param metrics Metrics collector for instrumentation (may be null).
     * @param cfg Reader configuration containing name and PV definitions.
     *
     * @throws EpicsReaderConfig::Error if configuration is invalid.
     */
    EpicsPVXSReader(std::shared_ptr<util::bus::IDataBus> bus,
                    std::shared_ptr<metrics::Metrics>         metrics,
                    const config::Config&                     cfg);

    /**
     * @brief Add PVs to the monitor.
     *
     * Creates PVXS subscriptions for the specified PV names. This method
     * is thread-safe and can be called after construction to dynamically
     * add new PVs to the reader.
     *
     * @param pvNames Set of PV names to monitor.
     *
     * @note Subscriptions persist until the reader is destroyed; there is
     *       currently no mechanism to remove individual PVs.
     */
    void addPV(const PVSet& pvNames);

private:
    /** @brief Default PVXS monitor request string requesting value, alarm, and timestamp. */
    static constexpr std::string_view kPVXSDefaultMonitorRequest = "field(value,alarm,timeStamp)";

    /** @name EPICS Alarm Severity Constants
     *  Standard EPICS alarm severity values.
     *  @{
     */
    static constexpr int kAlarmSeverityNone    = 0; ///< No alarm.
    static constexpr int kAlarmSeverityMinor   = 1; ///< Minor alarm.
    static constexpr int kAlarmSeverityMajor   = 2; ///< Major alarm.
    static constexpr int kAlarmSeverityInvalid = 3; ///< Invalid/disconnected.
    /** @} */

    /**
     * @brief Log an error message and increment the error metric.
     *
     * Convenience method that combines error logging with metrics recording
     * to ensure consistent error handling throughout the reader.
     *
     * @param message The error message to log.
     * @param tags Prometheus labels for the error metric.
     */
    void logAndRecordError(const std::string& message, const prometheus::Labels& tags);

    /**
     * @brief Process a single PV update event.
     *
     * Main entry point for handling PVXS monitor callbacks. Determines the
     * processing mode for the PV and delegates to the appropriate handler.
     *
     * @param pvName Name of the PV that produced the update.
     * @param epics_value The PVXS Value containing the update data.
     */
    void processEvent(std::string pvName, pvxs::Value epics_value);

    /**
     * @brief Process a PV update in default mode.
     *
     * Handles standard scalar/array PVs by extracting the value, alarm,
     * and timestamp fields and converting them to an MLDP EventBatch.
     *
     * @param pvName Name of the PV.
     * @param epicsValue The PVXS Value to process.
     * @param[out] emitted Set to 1 if an event was successfully emitted, 0 otherwise.
     */
    void processDefaultMode(const std::string& pvName, const pvxs::Value& epicsValue, std::size_t& emitted);

    /**
     * @brief Process a PV update in SLAC BSAS table mode.
     *
     * Handles SLAC BSAS NTTable structures where each row has its own
     * timestamp. Extracts per-row timestamps and converts each data column
     * into a series of timestamped events.
     *
     * @param pvName Name of the PV.
     * @param epicsValue The PVXS Value containing the NTTable.
     * @param runtimeCfg Runtime configuration with timestamp field names.
     * @param[out] emitted Number of events successfully emitted.
     */
    void processSlacBsasTableMode(const std::string&     pvName,
                                  const pvxs::Value&     epicsValue,
                                  const PVRuntimeConfig* runtimeCfg,
                                  std::size_t&           emitted);

    mutable std::mutex                                          subscriptions_mutex_; ///< Protects subscription operations.
    pvxs::client::Context                                       pva_context_;         ///< PVXS client context.
    pvxs::MPMCFIFO<std::shared_ptr<pvxs::client::Subscription>> m_pva_subscriptions;  ///< Active subscriptions.

    REGISTER_READER("epics-pvxs", EpicsPVXSReader)
};

} // namespace mldp_pvxs_driver::reader::impl::epics

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
#include <reader/impl/epics/pvxs/EpicsPVXSReaderConfig.h>
#include <reader/impl/epics/shared/EpicsReaderBase.h>

#include <pvxs/client.h>

#include <prometheus/labels.h>

#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mldp_pvxs_driver::reader::impl::epics {

/**
 * @brief EPICS reader implementation using the PVXS library.
 *
 * This class implements the Reader interface for monitoring EPICS Process
 * Variables using the PVXS library. It uses event-driven subscriptions that
 * trigger callbacks when PV values change, providing efficient and responsive
 * data acquisition.
 *
 * The `epics-pvxs` reader handles plain scalar and scalar-array PVs whose
 * structure carries a single `timeStamp`.  SLAC BSAS NTTables are handled by
 * the dedicated `epics-pvxs-bsas-gen1` / `epics-pvxs-bsas-gen2` readers, which
 * derive from this class (see EpicsPVXSBsasTableReaderBase).  Configuring
 * `option.type: slac-bsas-table` on an `epics-pvxs` PV is a configuration
 * error.
 *
 * Events are processed asynchronously using a thread pool to avoid blocking
 * the PVXS event loop. Metrics are collected for monitoring throughput,
 * latency, and error rates.
 *
 * Configuration example:
 * @code{.yaml}
 * readers:
 *   - epics-pvxs:
 *       - name: my-pvxs-reader
 *         thread-pool: 4
 *         pvs:
 *           - name: "MY:PV:NAME"
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
     * @throws EpicsReaderConfig::Error if configuration is invalid, including
     *         when a PV selects `option.type: slac-bsas-table`.
     */
    EpicsPVXSReader(std::shared_ptr<util::bus::IDataBus> bus,
                    std::shared_ptr<metrics::Metrics>    metrics,
                    const config::Config&                cfg);

    ~EpicsPVXSReader() override;

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

protected:
    /** @brief Default PVXS monitor request string requesting value, alarm, and timestamp. */
    static constexpr std::string_view kPVXSDefaultMonitorRequest = "field(value,alarm,timeStamp)";

    /**
     * @brief Construct the reader without starting any monitor.
     *
     * Used by derived readers that bring their own configuration view.  The
     * most-derived constructor must call addPV(pvNames()) once the object is
     * fully built, so that no event is dispatched into a partly constructed
     * object.
     *
     * @param bus Event bus for publishing converted PV data.
     * @param metrics Metrics collector (may be null).
     * @param readerCfg Parsed reader configuration.
     * @param rawCfg Raw reader configuration node (PVXS client settings).
     * @param logger Logger for this reader.
     * @param clientTag Reader type used in PVXS client config messages.
     * @param monitorRequest PVXS pvRequest used for every subscription.
     */
    EpicsPVXSReader(std::shared_ptr<util::bus::IDataBus> bus,
                    std::shared_ptr<metrics::Metrics>    metrics,
                    const EpicsReaderConfig&             readerCfg,
                    const config::Config&                rawCfg,
                    std::shared_ptr<util::log::ILogger>  logger,
                    std::string_view                     clientTag,
                    std::string_view                     monitorRequest);

    /**
     * @brief Stop event processing and drain the reader pool.
     *
     * Derived readers whose processEvent() override uses their own members
     * must call this from their destructor, so that no queued event runs
     * against a partly destroyed object.
     */
    void stopProcessing();

    /**
     * @brief Log an error message and increment the error metric.
     *
     * @param message The error message to log.
     * @param tags Prometheus labels for the error metric.
     */
    void logAndRecordError(const std::string& message, const prometheus::Labels& tags);

    /**
     * @brief Process a single PV update event.
     *
     * Called from the reader thread pool for every monitor update.  The base
     * implementation handles scalar / scalar-array PVs (Default mode).
     *
     * @param pvName Name of the PV that produced the update.
     * @param epics_value The PVXS Value containing the update data.
     */
    virtual void processEvent(std::string pvName, pvxs::Value epics_value);

    /**
     * @brief Record per-event pool depth gauge.
     */
    void recordPoolDepth();

    /**
     * @brief Record the per-event processing metrics.
     *
     * Records processing time, and when @p emitted is non-zero the event
     * count, total bytes and bytes/s (based on the previous event time for
     * @p pvName).
     *
     * @param pvName PV that produced the update.
     * @param sourceTag Prometheus labels for the PV.
     * @param dataBytes Estimated payload size of the update.
     * @param emitted Number of values emitted to the bus.
     * @param processingStart Time processing of the update started.
     */
    void recordEventMetrics(const std::string&                    pvName,
                            const prometheus::Labels&             sourceTag,
                            std::size_t                           dataBytes,
                            std::size_t                           emitted,
                            std::chrono::steady_clock::time_point processingStart);

    /**
     * @brief Estimate the in-memory payload size of a PVXS value, in bytes.
     */
    static std::size_t estimatePvxsValueBytes(const pvxs::Value& v);

    /**
     * @brief Return true when @p batch has timestamps and at least one
     *        non-NaN value (non floating-point columns always count as valid).
     */
    static bool hasValidData(const util::bus::DataBatch& batch);

private:
    /**
     * @brief Parse @p cfg for the scalar `epics-pvxs` reader.
     * @throws EpicsReaderConfig::Error when a PV selects a BSAS table type.
     */
    static EpicsPVXSReaderConfig makeScalarConfig(const config::Config& cfg);

    /** @name EPICS Alarm Severity Constants
     *  Standard EPICS alarm severity values.
     *  @{
     */
    static constexpr int kAlarmSeverityNone = 0;    ///< No alarm.
    static constexpr int kAlarmSeverityMinor = 1;   ///< Minor alarm.
    static constexpr int kAlarmSeverityMajor = 2;   ///< Major alarm.
    static constexpr int kAlarmSeverityInvalid = 3; ///< Invalid/disconnected.
    /** @} */

    /**
     * @brief Process a PV update in default mode.
     *
     * Handles standard scalar/array PVs by extracting the value, alarm,
     * and timestamp fields and converting them to an MLDP EventBatch.
     *
     * @param pvName Name of the PV.
     * @param epicsValue The PVXS Value to process.
     * @param sourceTag Prometheus labels for metrics.
     * @param[out] emitted Set to 1 if an event was successfully emitted, 0 otherwise.
     */
    void processDefaultMode(const std::string& pvName, const pvxs::Value& epicsValue,
                            const prometheus::Labels& sourceTag, std::size_t& emitted);

    std::string                                                 monitor_request_;     ///< PVXS pvRequest for subscriptions.
    mutable std::mutex                                          subscriptions_mutex_; ///< Protects subscription operations.
    pvxs::client::Context                                       pva_context_;         ///< PVXS client context.
    pvxs::MPMCFIFO<std::shared_ptr<pvxs::client::Subscription>> m_pva_subscriptions;  ///< Active subscriptions.
    std::mutex                                                   last_event_time_mutex_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_event_time_;

    REGISTER_READER("epics-pvxs", EpicsPVXSReader)
};

} // namespace mldp_pvxs_driver::reader::impl::epics

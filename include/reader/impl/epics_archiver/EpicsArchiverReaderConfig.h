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
 * @file EpicsArchiverReaderConfig.h
 * @brief Configuration parser for EPICS Archiver Appliance reader instances.
 *
 * This header provides the EpicsArchiverReaderConfig class for parsing and
 * validating Archiver Appliance reader configuration from YAML. It handles
 * reader-level settings (name, archiver hostname/URL) and the list of PVs
 * to retrieve from the archiver.
 */

#pragma once

#include <config/Config.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::reader::impl::epics_archiver {

/**
 * @brief Configuration parser for EPICS Archiver Appliance reader.
 *
 * Parses and validates YAML configuration for the archiver reader, extracting
 * the archiver service hostname/URL and the list of PV names to retrieve.
 *
 * Configuration example:
 * @code{.yaml}
 * readers:
 *   - type: epics-archiver
 *     name: my-archiver-reader
 *     hostname: "archiver.slac.stanford.edu:11200"
 *     mode: "historical_once" # optional, default
 *     start_date: "2026-01-01T00:00:00Z" # required in historical_once mode
 *     end_date: "2026-01-02T00:00:00Z"   # optional
 *     connect_timeout_sec: 30 # optional, default: 30 seconds
 *     total_timeout_sec: 300  # optional, default: 300 seconds (5 minutes)
 *     batch_duration_sec: 1   # optional, default: 1 second (historical sample-time window)
 *     tls_verify_peer: true   # optional, default: true
 *     tls_verify_host: true   # optional, default: true
 *     pvs:
 *       - name: "SLAC:GUNB:ELEC:LTU1:630:EPICS_PV"
 *       - name: "FACET:DL1:SBEN:1:BDES"
 *
 *   - type: epics-archiver
 *     name: my-archiver-tail-reader
 *     hostname: "archiver.slac.stanford.edu:11200"
 *     mode: "periodic_tail"
 *     poll_interval_sec: 5  # required in periodic_tail mode
 *     lookback_sec: 5       # optional, defaults to poll_interval_sec, must be <= poll_interval_sec
 *     pvs:
 *       - name: "FACET:DL1:SBEN:1:BDES"
 * @endcode
 */
class EpicsArchiverReaderConfig
{
public:
    enum class FetchMode
    {
        HistoricalOnce,
        PeriodicTail
    };

    /**
     * @brief Exception thrown when the reader configuration cannot be parsed.
     *
     * The message is descriptive (missing fields, wrong types, etc.) so callers
     * can surface errors directly to the control plane or logs.
     */
    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    /**
     * @brief Definition of a single PV entry for archiver retrieval.
     */
    struct PVConfig
    {
        std::string name; ///< PV name to retrieve from archiver.
    };

    /**
     * @brief Default constructor for an empty config.
     */
    EpicsArchiverReaderConfig();

    /**
     * @brief Build a typed view over the provided YAML node.
     *
     * @param readerEntry YAML configuration node for this reader.
     * @throws Error when any required fields for the selected mode are missing
     *         or malformed (e.g. historical start_date/startDate, periodic
     *         poll_interval_sec, or pvs).
     */
    explicit EpicsArchiverReaderConfig(const ::mldp_pvxs_driver::config::Config& readerEntry);

    /**
     * @brief Check whether the wrapped YAML node passed validation.
     *
     * @return true if configuration is valid; false otherwise.
     */
    bool valid() const;

    /**
     * @brief Get the configured reader name.
     *
     * @return Reader name as specified in YAML config.
     */
    const std::string& name() const;

    /**
     * @brief Get the Archiver Appliance service hostname/URL.
     *
     * @return Hostname or URL where the archiver service is accessible
     *         (e.g., "archiver.slac.stanford.edu:11200").
     */
    const std::string& hostname() const;

    /**
     * @brief Get the start of the requested archiver time window.
     *
     * @return Start date/time string as configured in YAML.
     */
    const std::string& startDate() const;

    /**
     * @brief Get the optional end of the requested archiver time window.
     *
     * @return End date/time string when configured; std::nullopt otherwise.
     */
    const std::optional<std::string>& endDate() const;
    /**
     * @brief Get the configured fetch mode (one-shot historical or periodic tail polling).
     */
    FetchMode                         fetchMode() const;

    /**
     * @brief Get the ordered list of PV entries to retrieve.
     *
     * @return Vector of PVConfig objects, each containing a PV name.
     */
    const std::vector<PVConfig>& pvs() const;

    /**
     * @brief Get just the PV names for convenience.
     *
     * @return Vector of PV name strings in the same order as pvs().
     */
    const std::vector<std::string>& pvNames() const;

    /**
     * @brief Get the connection timeout for HTTP requests to the archiver.
     *
     * @return Connection timeout in seconds (default: 30).
     */
    long connectTimeoutSec() const;

    /**
     * @brief Get the total timeout for HTTP requests to the archiver.
     *
     * @return Total operation timeout in seconds (default: 300).
     *         Special value 0 means infinite timeout (useful for long streaming sessions).
     *         Low-speed detection (1KB/sec for 60s) will still catch stalled connections.
     */
    long totalTimeoutSec() const;

    /**
     * @brief Get the max historical sample-time span for one published batch.
     *
     * Batches are split using archiver sample timestamps (not wall-clock read time).
     *
     * @return Batch duration threshold in seconds (default: 1).
     */
    long batchDurationSec() const;
    /**
     * @brief Get periodic tail poll interval in seconds.
     *
     * Valid only when mode is @ref FetchMode::PeriodicTail.
     */
    long pollIntervalSec() const;
    /**
     * @brief Get periodic tail lookback window in seconds.
     *
     * Valid only when mode is @ref FetchMode::PeriodicTail. Defaults to
     * @ref pollIntervalSec() when not explicitly configured.
     */
    long lookbackSec() const;

    /**
     * @brief Whether to verify the server TLS certificate chain.
     *
     * @return true to enable TLS peer verification (default); false to disable.
     */
    bool tlsVerifyPeer() const;

    /**
     * @brief Whether to verify the server hostname against the TLS certificate.
     *
     * @return true to enable hostname verification (default); false to disable.
     */
    bool tlsVerifyHost() const;

private:
    /**
     * @brief Populate the typed fields from the raw YAML node.
     *
     * @param readerEntry YAML configuration node to parse.
     * @throws Error if parsing fails.
     */
    void parse(const ::mldp_pvxs_driver::config::Config& readerEntry);

    bool                       valid_ = false;
    std::string                name_;
    std::string                hostname_;
    FetchMode                  fetch_mode_ = FetchMode::HistoricalOnce;
    std::string                start_date_;
    std::optional<std::string> end_date_;
    std::vector<PVConfig>      pvs_;
    std::vector<std::string>   pvNames_;
    long                       connect_timeout_sec_ = 30L; ///< Connection timeout in seconds
    long                       total_timeout_sec_ = 300L;  ///< Total operation timeout in seconds
    long                       batch_duration_sec_ = 1L;   ///< Max historical sample-time span per output batch.
    long                       poll_interval_sec_ = 0L;    ///< Periodic tail poll interval (seconds).
    long                       lookback_sec_ = 0L;         ///< Periodic tail lookback window (seconds).
    bool                       tls_verify_peer_ = true;    ///< Verify TLS certificate chain.
    bool                       tls_verify_host_ = true;    ///< Verify TLS host name.
};

} // namespace mldp_pvxs_driver::reader::impl::epics_archiver

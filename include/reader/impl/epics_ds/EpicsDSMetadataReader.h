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
 * @file   EpicsDSMetadataReader.h
 * @brief  PVXS RPC-based reader that fetches PV metadata from an EPICS Directory Service.
 * @author SLAC MLDP Team
 * @date   2025-01-01
 * @copyright Copyright (c) 2025 SLAC National Accelerator Laboratory
 *
 * Issues an NTURI RPC call to a PVA Directory Service endpoint and publishes
 * the resulting NTTable as a SourceMetadataPayload on the driver bus.
 * Supports optional periodic re-fetch via rescan-interval-sec.
 * Supports configurable thread count: worker-thread-count=1 runs inline (single
 * jthread); worker-thread-count=N>1 uses 1 producer + (N-1) consumer jthreads.
 */

#pragma once

#include <config/Config.h>
#include <reader/IReader.h>
#include <reader/ReaderFactory.h>
#include <reader/impl/epics_ds/EpicsDSMetadataReaderConfig.h>
#include <util/bus/IDataBus.h>
#include <util/log/ILog.h>

#include <pvxs/client.h>
#include <pvxs/data.h>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mldp_pvxs_driver::metrics {
class Metrics;
}

namespace mldp_pvxs_driver::reader::impl::epics_ds {

/**
 * @class  EpicsDSMetadataReader
 * @brief  Reader that queries an EPICS Directory Service via PVA RPC and
 *         publishes PV metadata onto the bus as SourceMetadataPayload.
 * @details
 *   Constructs an NTURI request, executes an RPC call against the configured
 *   service name, parses the NTTable response, and pushes the resulting
 *   SourceMetadataPayload via the bus.  When @c rescan-interval-sec > 0 the
 *   fetch repeats at that interval until the reader is destroyed.
 *
 *   When @c worker-thread-count=1 (default) the entire fetch/parse/push cycle
 *   runs in a single jthread.  When @c worker-thread-count=N>1, one producer
 *   jthread issues RPC calls and N-1 consumer jthreads parse and push results.
 *
 *   Supported configuration keys:
 *   | Key                   | Type   | Default        | Description |
 *   |-----------------------|--------|----------------|-------------|
 *   | name                  | string | (required)     | Reader instance name |
 *   | service               | string | `ds`           | PVA channel to call via RPC |
 *   | query                 | string | `%`            | NTURI query.name wildcard |
 *   | timeout-sec           | double | `5.0`          | RPC call timeout (must be > 0) |
 *   | source-name-column    | string | `channelName`  | NTTable column holding PV name |
 *   | tags-column           | string | `""`           | NTTable column holding tags (disabled when empty) |
 *   | show-columns          | string | `""`           | Comma-separated DS columns for `show=` param (all columns when empty) |
 *   | rescan-interval-sec   | double | `0.0`          | Re-fetch period; 0 = run once and auto-closes when all readers complete; must be >= 0 |
 *   | worker-thread-count   | int    | `1`            | 1 = inline; N>1 = 1 producer + (N-1) consumers |
 *   | max-queue-depth       | int    | `16`           | Bounded queue size (producer/consumer mode only) |
 *   | pvs                   | list   | (required)     | Per-PV enrichment entries; at least one entry required |
 *   | pvs[].name            | string | (required)     | Exact PV name to enrich |
 *   | pvs[].metadata        | map    | `{}`           | Static key/value attributes merged into the entry |
 *   | pv-show-columns       | string | `dname,ename,etype,lname,ioc,scheme,z` | Comma-separated DS `show=` columns for PV-list mode; empty string reverts to default |
 *
 * @code{.yaml}
 * readers:
 *   - type: epics-ds-metadata
 *     name: my-ds-reader
 *     service: ds
 *     query: "%"
 *     timeout-sec: 5.0
 *     source-name-column: channelName
 *     tags-column: tags
 *     show-columns: "channelName,hostName,iocName"
 *     rescan-interval-sec: 300.0
 *     worker-thread-count: 4
 *     max-queue-depth: 16
 *     pvs:
 *       - name: BPMS:LI20:2445:X
 *         metadata:
 *           system: bpm
 *           area: li20
 *       - name: QUAD:LI21:221:BACT
 *     pv-show-columns: "dname,ename,etype,lname,ioc,scheme,z"
 * @endcode
 */
class EpicsDSMetadataReader final : public reader::Reader
{
    REGISTER_READER("epics-ds-metadata", EpicsDSMetadataReader)

public:
    /**
     * @brief Construct and start the reader.
     *
     * @param bus     Event bus for publishing SourceMetadataPayload batches.
     * @param metrics Metrics collector (may be null).
     * @param cfg     Reader configuration node.
     * @throws EpicsDSMetadataReaderConfig::Error if configuration is invalid.
     */
    EpicsDSMetadataReader(std::shared_ptr<util::bus::IDataBus> bus,
                          std::shared_ptr<metrics::Metrics>    metrics,
                          const config::Config&                cfg);

    ~EpicsDSMetadataReader() override;

    EpicsDSMetadataReader(const EpicsDSMetadataReader&)            = delete;
    EpicsDSMetadataReader& operator=(const EpicsDSMetadataReader&) = delete;
    EpicsDSMetadataReader(EpicsDSMetadataReader&&)                 = delete;
    EpicsDSMetadataReader& operator=(EpicsDSMetadataReader&&)      = delete;

    std::string name() const override { return config_.name(); }

private:
    /**
     * @brief Bounded concurrent queue for pvxs::Value RPC results.
     *
     * Used only in producer/consumer mode (worker-thread-count > 1).
     * std::condition_variable_any is required for the C++20 stop_token-aware
     * wait overload.
     */
    class RpcResultQueue {
    public:
        explicit RpcResultQueue(std::size_t max_depth) : max_depth_(max_depth) {}

        /** Producer: enqueue item. Returns false if queue was closed. */
        bool push(pvxs::Value item, std::stop_token st);

        /** Consumer: dequeue item. Returns nullopt when closed and empty. */
        std::optional<pvxs::Value> pop(std::stop_token st);

        /** Signal shutdown; unblocks all waiters. */
        void close();

    private:
        std::size_t             max_depth_;
        std::queue<pvxs::Value> queue_;
        std::mutex              mu_;
        std::condition_variable_any not_full_;
        std::condition_variable_any not_empty_;
        bool                    closed_{false};
    };

    /**
     * Single worker loop: fetch RPC, then call dispatch_fn_ with the result.
     * dispatch_fn_ is set at construction — points to processResult (N=1)
     * or queueResult (N>1). No code duplication between modes.
     */
    void runWorker(std::stop_token st);

    /** Consumer/consumer mode: pop from result_queue_, parse, push to bus. */
    void runConsumer(std::stop_token st);

    /** Parse + bus push. dispatch_fn_ target in single-thread mode. */
    void processResult(pvxs::Value result);

    /** Enqueue into result_queue_. dispatch_fn_ target in multi-thread mode. */
    bool queueResult(pvxs::Value result, std::stop_token st);

    /** Build the NTURI pvxs::Value from config. */
    pvxs::Value buildNTURI() const;

    /** Build an NTURI for one exact PV and one DS `show=` column. */
    pvxs::Value buildNTURIForPV(const std::string& pvName,
                                const std::string& showCol) const;

    /** Query and merge DS attributes for one configured PV. */
    std::unordered_map<std::string, std::string>
    queryPVAttributes(const EpicsDSMetadataReaderConfig::PVEntry& pv);

    /** Run targeted per-PV enrichment sweep and publish one batch per PV. */
    void runPVListSweep(std::stop_token st) noexcept;

    /** Close the result queue and wait for all queued wildcard-query results to be published. */
    void drainResultConsumers();

    /**
     * @brief Parse an NTTable PVXS Value into a SourceMetadataPayload.
     */
    util::bus::SourceMetadataPayload parseNTTable(const pvxs::Value& result) const;

    // Member declaration order governs RAII destruction (reverse order).
    // worker_thread_ declared last → destroyed first.
    EpicsDSMetadataReaderConfig         config_;
    std::shared_ptr<util::log::ILogger> logger_;
    pvxs::client::Context               pva_context_;
    std::optional<RpcResultQueue>       result_queue_;    // present only when N > 1
    // dispatch_fn_: set once at construction; called by runWorker after each RPC.
    // N=1: calls processResult directly.
    // N>1: calls queueResult (push to result_queue_).
    std::function<bool(pvxs::Value, std::stop_token)> dispatch_fn_;
    std::condition_variable_any         sleep_cv_;        // interruptible sleep
    std::mutex                          sleep_mutex_;
    std::vector<std::jthread>           consumer_threads_; // empty in single-thread mode
    std::jthread                        worker_thread_;   // single producer thread
};

} // namespace mldp_pvxs_driver::reader::impl::epics_ds

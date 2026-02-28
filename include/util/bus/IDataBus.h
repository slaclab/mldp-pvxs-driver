//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file
 * @brief Interface describing the minimal API to push events into the driver bus.
 */

#pragma once
#include <chrono>
#include <cstdint>
#include <ingestion.grpc.pb.h>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace mldp_pvxs_driver::util::bus {

/**
 * @brief Timestamped ingestion payload shared across the bus.
 *
 * Instances carry both coarse and fine grained timestamps along with the
 * generated @ref DataValue, allowing transport layers to forward the payload
 * without copying.
 */
struct EventValueStruct
{
    uint64_t  epoch_seconds; ///< Unix epoch seconds for the sample.
    uint64_t  nanoseconds;   ///< Additional nanoseconds precision.
    DataValue data_value;    ///< Protobuf payload (by value to avoid heap allocation).
};

/**
 * @brief Container describing a batch of events to ingest.
 *
 * Tags provide additional metadata that should accompany the entire batch,
 * while @ref values holds the individual payloads grouped by their signal name.
 */
struct EventBatchStruct
{
    std::string                                                           root_source; ///< root source identifier for the batch.
    std::vector<std::string>                                              tags;        ///< Optional metadata attached to the batch.
    std::map<std::string, std::vector<std::shared_ptr<EventValueStruct>>> values;      ///< Payloads grouped per source ID.
};

/**
 * @brief Normalized timestamp payload returned by source-metadata queries.
 */
struct SourceTimestamp
{
    uint64_t epoch_seconds{0}; ///< Unix epoch seconds.
    uint64_t nanoseconds{0};   ///< Nanoseconds fraction.
};

/**
 * @brief Metadata summary for one source/PV as returned by MLDP query services.
 *
 * Most fields are optional because upstream providers may not populate all
 * metadata dimensions for every source.
 */
struct SourceInfoStruct
{
    std::string                    source_name;                      ///< Source/PV identifier.
    std::optional<SourceTimestamp> first_timestamp;                 ///< Earliest known data timestamp.
    std::optional<SourceTimestamp> last_timestamp;                  ///< Latest known data timestamp.
    std::optional<std::string>     last_provider_id;                ///< Last provider ID that wrote the source.
    std::optional<std::string>     last_provider_name;              ///< Last provider name that wrote the source.
    std::optional<std::string>     last_bucket_id;                  ///< Backing storage bucket identifier.
    std::optional<std::string>     last_bucket_data_type;           ///< Data type recorded in last bucket.
    std::optional<std::string>     last_bucket_data_timestamps_type; ///< Timestamp encoding used in last bucket.
    std::optional<uint64_t>        last_bucket_sample_period;       ///< Sampling period in nanoseconds (if known).
    std::optional<uint32_t>        last_bucket_sample_count;        ///< Number of samples in last bucket.
    std::optional<int32_t>         num_buckets;                     ///< Total bucket count for the source.
};

/**
 * @brief Tuning options for source data queries.
 *
 * Grouping these parameters avoids API churn when query knobs evolve.
 */
struct QuerySourcesDataOptions
{
    std::chrono::milliseconds timeout{std::chrono::seconds(5)};        ///< Total polling budget.
    std::chrono::seconds      lookback_window{std::chrono::seconds(30)}; ///< beginTime offset from now.
    std::chrono::seconds      forward_window{std::chrono::seconds(1)};   ///< endTime offset from now.
    std::chrono::seconds      rpc_deadline{std::chrono::seconds(5)};     ///< Per-RPC deadline.
};

/**
 * @brief Minimal API contract for pushing events on the driver bus.
 *
 * Implementations are expected to forward serialized ingestion events to the
 * rest of the system (e.g. over gRPC or PVXS) while honoring the ownership
 * semantics of the provided payloads.
 */
class IDataBus
{
public:
    /// Shared ownership wrapper around the generated ingestion payload.
    using EventValue = std::shared_ptr<EventValueStruct>;
    /// Batch of values grouped per source identifier with optional tags.
    using EventBatch = EventBatchStruct;
    /// Metadata describing one source/PV from MLDP query APIs.
    using SourceInfo = SourceInfoStruct;

    /**
     * @brief Helper factory that returns an empty event payload.
     * @return Shared pointer users can populate before invoking @ref push.
     */
    static EventValue MakeEventValue(uint64_t epoch_seconds = 0, uint64_t nanoseconds = 0)
    {
        // Construct a temporary aggregate explicitly to avoid overload
        // resolution issues with braced-init-lists and make_shared.
        return std::make_shared<EventValueStruct>(
            EventValueStruct{
                epoch_seconds,
                nanoseconds,
                DataValue{}});
    }

    virtual ~IDataBus() = default;

    /**
     * @brief Pushes a batch of populated ingestion events into the bus.
     *
     * Each entry in @p batch_values.values represents a single source whose
     * vector aggregates one or more payloads. Implementations may forward all
     * entries in a single call to the back-end to minimize network round-trips.
     * When @p batch_values.tags is empty, implementations may add their own
     * default tags (such as each source name) before forwarding the batch.
     *
     * @param batch_values Aggregated batch describing tags and payloads. Each
     *                     payload is shared with the bus implementation.
     * @return true if the batch was accepted for delivery.
     */
    virtual bool push(EventBatch batch_values) = 0;

    /**
     * @brief Query MLDP metadata for a set of source identifiers.
     *
     * The default implementation returns an empty list; concrete backends may
     * override it when they can query metadata services (for example
     * `DpQueryService::queryPvMetadata`).
     *
     * @param source_names Source/PV identifiers to query.
     * @return Metadata rows for the sources known to the backend.
     */
    virtual std::vector<SourceInfo> querySourcesInfo(const std::set<std::string>& source_names)
    {
        (void)source_names;
        return {};
    }

    /**
     * @brief Query MLDP data columns for sources over a relative time window.
     *
     * This mirrors the query shape used by integration tests: `queryData`
     * with `pvNames`, `beginTime = now - lookback_window`, and
     * `endTime = now + forward_window`.
     *
     * @param source_names Source/PV identifiers to query.
     * @param options Query tuning options (timeouts and relative query window).
     * @return Map keyed by source name containing returned DataColumn payloads.
     *         Returns std::nullopt on transport/protocol failures.
     */
    virtual std::optional<std::unordered_map<std::string, DataColumn>> querySourcesData(
        const std::set<std::string>&   source_names,
        const QuerySourcesDataOptions& options = QuerySourcesDataOptions{})
    {
        (void)source_names;
        (void)options;
        return std::nullopt;
    }
};

} // namespace mldp_pvxs_driver::util::bus

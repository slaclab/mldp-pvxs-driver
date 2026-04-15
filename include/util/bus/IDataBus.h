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
#include "common.pb.h"
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace mldp_pvxs_driver::util::bus {

/**
 * @brief Container describing a batch of events to ingest.
 *
 * Tags provide additional metadata that should accompany the entire batch,
 * while @ref values holds the individual payloads grouped by their signal name.
 */
struct EventBatchStruct
{
    std::string                                 root_source; ///< Root PV identifier used for batch-level metrics/correlation.
    std::vector<std::string>                    tags;        ///< Optional metadata attached to the batch.
    std::vector<dp::service::common::DataFrame> frames;      ///< One frame per ingestion payload; each frame must include timestamps.
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
    std::optional<SourceTimestamp> first_timestamp;                  ///< Earliest known data timestamp.
    std::optional<SourceTimestamp> last_timestamp;                   ///< Latest known data timestamp.
    std::optional<std::string>     last_provider_id;                 ///< Last provider ID that wrote the source.
    std::optional<std::string>     last_provider_name;               ///< Last provider name that wrote the source.
    std::optional<std::string>     last_bucket_id;                   ///< Backing storage bucket identifier.
    std::optional<std::string>     last_bucket_data_type;            ///< Data type recorded in last bucket.
    std::optional<std::string>     last_bucket_data_timestamps_type; ///< Timestamp encoding used in last bucket.
    std::optional<uint64_t>        last_bucket_sample_period;        ///< Sampling period in nanoseconds (if known).
    std::optional<uint32_t>        last_bucket_sample_count;         ///< Number of samples in last bucket.
    std::optional<int32_t>         num_buckets;                      ///< Total bucket count for the source.
};

/**
 * @brief Tuning options for source data queries.
 *
 * Grouping these parameters avoids API churn when query knobs evolve.
 */
struct QuerySourcesDataOptions
{
    std::chrono::milliseconds timeout{std::chrono::seconds(5)};          ///< Total polling budget.
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
    /// Batch of values grouped per source identifier with optional tags.
    using EventBatch = EventBatchStruct;
    /// Metadata describing one source/PV from MLDP query APIs.
    using SourceInfo = SourceInfoStruct;

    virtual ~IDataBus() = default;

    /**
     * @brief Pushes a batch of populated ingestion events into the bus.
     *
     * Each entry in @p batch_values.frames represents one ingestion payload.
     * Implementations may forward all entries in a single call to the back-end
     * to minimize network round-trips.
     * When @p batch_values.tags is empty, implementations may add their own
     * default tags before forwarding the batch.
     *
     * @param batch_values Aggregated batch describing tags and payloads. Each
     *                     payload is shared with the bus implementation.
     * @return true if the batch was accepted for delivery.
     */
    virtual bool push(EventBatch batch_values) = 0;
};

} // namespace mldp_pvxs_driver::util::bus

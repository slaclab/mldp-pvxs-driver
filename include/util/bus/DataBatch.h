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
 * @file   DataBatch.h
 * @brief  Protocol-agnostic columnar data batch used as the event-bus frame type.
 * @author SLAC mldp-pvxs-driver contributors
 * @date   2024-01-01
 * @details
 *   `DataBatch` carries time-stamped, heterogeneous column data without any
 *   dependency on MLDP protobuf types.  Writers that target MLDP convert
 *   `DataBatch` → `dp::service::common::DataFrame` internally; other writers
 *   (e.g. HDF5, Arrow) consume `DataBatch` directly.
 *
 *   All types in this file live in `mldp_pvxs_driver::util::bus`.
 * @copyright
 *   See the LICENSE.txt file found in the top-level directory of this
 *   distribution and at:
 *   https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace mldp_pvxs_driver::util::bus {

// ---------------------------------------------------------------------------
// Timestamp
// ---------------------------------------------------------------------------

/**
 * @struct TimestampEntry
 * @brief  Per-sample timestamp: Unix epoch seconds plus nanosecond sub-second offset.
 * @note   Two `uint64_t` fields → `sizeof(TimestampEntry)` == 16 bytes.
 */
struct TimestampEntry
{
    uint64_t epoch_seconds{0}; ///< Seconds past Unix epoch.
    uint64_t nanoseconds{0};   ///< Nanosecond sub-second offset.
};

// ---------------------------------------------------------------------------
// Column types
// ---------------------------------------------------------------------------

/**
 * @struct ArrayDims
 * @brief  Shape descriptor for array-valued columns.
 * @details
 *   For a 1-D waveform of 1024 samples: `dims = {1024}`.
 *   For a 64×64 image frame: `dims = {64, 64}`.
 */
struct ArrayDims
{
    std::vector<uint32_t> dims; ///< Size of each dimension.
};

/**
 * @typedef ColumnValues
 * @brief   Union of all supported per-column data representations.
 *
 * Scalar columns store one value per timestamp entry.
 * Array columns store one flattened array per timestamp entry; use
 * @ref DataBatch::array_dims to look up the shape for a given column name.
 *
 * | Index | C++ type                           | Semantic          |
 * |-------|------------------------------------|-------------------|
 * |   0   | `std::vector<double>`              | scalar double     |
 * |   1   | `std::vector<float>`               | scalar float      |
 * |   2   | `std::vector<int64_t>`             | scalar int64      |
 * |   3   | `std::vector<int32_t>`             | scalar int32      |
 * |   4   | `std::vector<bool>`                | scalar bool       |
 * |   5   | `std::vector<std::string>`         | scalar string     |
 * |   6   | `std::vector<std::vector<uint8_t>>`| bytes/blob/struct |
 * |   7   | `std::vector<std::vector<double>>` | double array      |
 * |   8   | `std::vector<std::vector<float>>`  | float array       |
 * |   9   | `std::vector<std::vector<int64_t>>`| int64 array       |
 * |  10   | `std::vector<std::vector<int32_t>>`| int32 array       |
 * |  11   | `std::vector<std::vector<bool>>`   | bool array        |
 */
using ColumnValues = std::variant<
    std::vector<double>,               // 0 — scalar double
    std::vector<float>,                // 1 — scalar float
    std::vector<int64_t>,              // 2 — scalar int64
    std::vector<int32_t>,              // 3 — scalar int32
    std::vector<bool>,                 // 4 — scalar bool
    std::vector<std::string>,          // 5 — scalar string
    std::vector<std::vector<uint8_t>>, // 6 — bytes / struct / image blob
    std::vector<std::vector<double>>,  // 7 — double array column
    std::vector<std::vector<float>>,   // 8 — float array column
    std::vector<std::vector<int64_t>>, // 9 — int64 array column
    std::vector<std::vector<int32_t>>, // 10 — int32 array column
    std::vector<std::vector<bool>>     // 11 — bool array column
    >;

/**
 * @struct DataColumn
 * @brief  Named column of homogeneous typed samples.
 */
struct DataColumn
{
    std::string  name;   ///< Column / signal name.
    ColumnValues values; ///< Typed sample vector (one entry per timestamp).
    std::unordered_map<std::string, std::string> metadata; ///< Per-column key/value attributes.
};

/**
 * @struct EnumDataColumn
 * @brief  Named enum column: integer values with a user-defined semantic id.
 * @details
 *   The @p enum_id is a contract between data producer and consumer
 *   (e.g. `"epics:alarm_status:v2"`).
 */
struct EnumDataColumn
{
    std::string          name;    ///< Column / signal name.
    std::vector<int32_t> values;  ///< Enum integer values (one per timestamp).
    std::string          enum_id; ///< Semantic identifier for enum encoding.
};

// ---------------------------------------------------------------------------
// DataBatch
// ---------------------------------------------------------------------------

/**
 * @struct DataBatch
 * @brief  Protocol-agnostic, columnar data frame passed through the event bus.
 *
 * One `DataBatch` carries all samples for one ingestion event:
 * - `timestamps` — ordered list of per-sample time points.
 * - `columns`    — heterogeneous typed columns (one value per timestamp).
 * - `enum_columns` — integer enum columns with semantic ids.
 * - `array_dims`  — optional shape information keyed by column name; only
 *                   needed when a column holds array-valued samples.
 *
 * ## Usage
 *
 * ```cpp
 * util::bus::DataBatch batch;
 * batch.timestamps.push_back({epoch_sec, nanos});
 *
 * util::bus::DataColumn col;
 * col.name   = "temperature";
 * col.values = std::vector<double>{42.0};
 * batch.columns.push_back(std::move(col));
 *
 * IDataBus::EventBatch ev;
 * ev.payload = TimeSeriesPayload{.root_source_name = "sensor:temperature", .frames = {std::move(batch)}};
 * bus->push(std::move(ev));
 * ```
 */
struct DataBatch
{
    std::vector<TimestampEntry>                timestamps;   ///< Per-sample timestamps (required).
    std::vector<DataColumn>                    columns;      ///< Typed scalar or array columns.
    std::vector<EnumDataColumn>                enum_columns; ///< Enum columns.
    std::unordered_map<std::string, ArrayDims> array_dims;   ///< Shape info keyed by column name.
};

/**
 * @brief   Estimate the raw in-memory size of a DataBatch.
 * @details Sums timestamp storage, column name lengths, column payload bytes
 *          (exact for fixed-width scalars; conservative for `vector<bool>`),
 *          and enum-column name and value storage.
 * @param[in] batch  The DataBatch whose memory footprint to estimate.
 * @return  Estimated byte count.  Does not include `std::vector` internal
 *          bookkeeping (capacity, pointer, and size fields).
 * @note    `vector<bool>` is counted as 1 byte per element (packed storage is
 *          implementation-defined; this is a conservative upper bound).
 *          `sizeof(TimestampEntry)` is assumed to be 16 bytes (two `uint64_t`).
 */
inline std::size_t estimateDataBatchBytes(const DataBatch& batch)
{
    std::size_t bytes = batch.timestamps.size() * sizeof(TimestampEntry);

    for (const auto& col : batch.columns)
    {
        bytes += col.name.size();
        bytes += std::visit([](const auto& v) -> std::size_t
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::vector<double>>)   return v.size() * sizeof(double);
            if constexpr (std::is_same_v<T, std::vector<float>>)    return v.size() * sizeof(float);
            if constexpr (std::is_same_v<T, std::vector<int64_t>>)  return v.size() * sizeof(int64_t);
            if constexpr (std::is_same_v<T, std::vector<int32_t>>)  return v.size() * sizeof(int32_t);
            if constexpr (std::is_same_v<T, std::vector<bool>>)     return v.size();
            if constexpr (std::is_same_v<T, std::vector<std::string>>)
            { std::size_t s = 0; for (const auto& x : v) s += x.size(); return s; }
            if constexpr (std::is_same_v<T, std::vector<std::vector<uint8_t>>>)
            { std::size_t s = 0; for (const auto& x : v) s += x.size(); return s; }
            if constexpr (std::is_same_v<T, std::vector<std::vector<double>>>)
            { std::size_t s = 0; for (const auto& x : v) s += x.size() * sizeof(double); return s; }
            if constexpr (std::is_same_v<T, std::vector<std::vector<float>>>)
            { std::size_t s = 0; for (const auto& x : v) s += x.size() * sizeof(float); return s; }
            if constexpr (std::is_same_v<T, std::vector<std::vector<int64_t>>>)
            { std::size_t s = 0; for (const auto& x : v) s += x.size() * sizeof(int64_t); return s; }
            if constexpr (std::is_same_v<T, std::vector<std::vector<int32_t>>>)
            { std::size_t s = 0; for (const auto& x : v) s += x.size() * sizeof(int32_t); return s; }
            if constexpr (std::is_same_v<T, std::vector<std::vector<bool>>>)
            { std::size_t s = 0; for (const auto& x : v) s += x.size(); return s; }
            return 0;
        }, col.values);
    }

    for (const auto& ec : batch.enum_columns)
        bytes += ec.name.size() + ec.values.size() * sizeof(int32_t) + ec.enum_id.size();

    return bytes;
}

} // namespace mldp_pvxs_driver::util::bus

//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file IQueryable.h
 * @brief Declares the backend-independent queryable and streaming contracts. */
#pragma once

#include <query/LiteralValue.h>

#include <arrow/record_batch.h>

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mldp_pvxs_driver::query {

class ExecutionContext;

/** @brief Logical scalar types supported by query schemas and predicates. */
enum class ColumnType
{
    STRING,
    TIMESTAMP,
    DURATION_SECONDS,
    INT,
    NATIVE_VALUE,
    BOOL
};
/** @brief Comparison and membership operators available to a queryable. */
enum class PredicateOp
{
    EQ,
    NEQ,
    LT,
    LTE,
    GT,
    GTE,
    IN,
    PREFIX,
    CONTAINS,
    LIKE,
    BETWEEN,
    IS_NULL,
    IS_NOT_NULL
};

using ExecutableLiteralValue = std::variant<std::string, int64_t, double, bool, TimestampNsLiteral, DurationNsLiteral>;

/** @brief Executable predicate with backend-ready literal values. */
struct Predicate
{
    std::string                                           column;
    PredicateOp                                           op;
    std::vector<ExecutableLiteralValue> values;
};

/** @brief Describes one queryable table column and its predicate capabilities. */
struct ColumnSchema
{
    std::string           name;
    ColumnType            type;
    bool                  required;
    bool                  is_output;
    std::set<PredicateOp> pushable_ops;
    std::set<PredicateOp> filterable_ops;
    std::string           notes;
};

/** Pull source of Arrow batches.  A null batch denotes clean EOF. */
class IRecordBatchStream
{
public:
    virtual ~IRecordBatchStream() = default;
    virtual std::shared_ptr<arrow::RecordBatch> next() = 0;
};

using IRecordBatchStreamUPtr = std::unique_ptr<IRecordBatchStream>;

/** @brief Backend interface for table discovery and streaming execution.
 *
 * All execution is streaming.  Backends that internally paginate must handle
 * pagination themselves and return a pull stream over all pages.  Callers
 * always drive a single IRecordBatchStream regardless of table or transport.
 */
class IQueryable
{
public:
    virtual ~IQueryable() = default;

    virtual std::set<std::string_view> virtualTables() const = 0;
    virtual std::vector<ColumnSchema>  tableSchema(std::string_view table_name) const = 0;

    /**
     * Maximum number of independent streams that this queryable can service
     * concurrently for one query.  The default preserves serial execution.
     */
    virtual std::size_t maxConcurrentStreams() const noexcept { return 1; }

    /**
     * Execute a valid table selection and return a pull stream of Arrow batches.
     *
     * The implementation is responsible for fetching all pages from the backend,
     * spilling to disk as needed, and exposing the result as a pull stream.
     * A null batch from next() signals clean EOF.
     */
    virtual IRecordBatchStreamUPtr executeStream(std::string_view              table_name,
                                                 const std::vector<Predicate>& pushable_predicates,
                                                 const std::set<std::string>&  projection_hint,
                                                 const ExecutionContext&       context) = 0;
};

using IQueryableUPtr = std::unique_ptr<IQueryable>;

} // namespace mldp_pvxs_driver::query

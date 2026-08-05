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
    STRING,           ///< UTF-8 string column.
    TIMESTAMP,        ///< Nanosecond-precision timestamp.
    DURATION_SECONDS, ///< Duration expressed as fractional seconds.
    INT,              ///< 64-bit signed integer.
    NATIVE_VALUE,     ///< Backend-native numeric value (double).
    BOOL              ///< Boolean flag.
};
/** @brief Comparison and membership operators available to a queryable. */
enum class PredicateOp
{
    EQ,          ///< Equal (=).
    NEQ,         ///< Not equal (!=).
    LT,          ///< Less than (<).
    LTE,         ///< Less than or equal (<=).
    GT,          ///< Greater than (>).
    GTE,         ///< Greater than or equal (>=).
    IN,          ///< Value is a member of a set.
    PREFIX,      ///< String starts with the given prefix.
    CONTAINS,    ///< String contains the given substring.
    LIKE,        ///< SQL LIKE pattern match.
    BETWEEN,     ///< Inclusive range check (two-value list).
    IS_NULL,     ///< Value is NULL.
    IS_NOT_NULL  ///< Value is not NULL.
};

/** @brief Literal value type used in executable predicates; covers all supported column types. */
using ExecutableLiteralValue = std::variant<std::string, int64_t, double, bool, TimestampNsLiteral, DurationNsLiteral>;

/** @brief Executable predicate with backend-ready literal values. */
struct Predicate
{
    std::string                         column; ///< Name of the column the predicate applies to.
    PredicateOp                         op;     ///< Comparison or membership operator.
    std::vector<ExecutableLiteralValue> values; ///< Bound literal operands (one for unary ops, two for BETWEEN, many for IN).
};

/** @brief Describes one queryable table column and its predicate capabilities. */
struct ColumnSchema
{
    std::string           name;           ///< Column identifier as used in SQL expressions.
    ColumnType            type;           ///< Logical scalar type of the column.
    bool                  required;       ///< True if the column must appear in every query against this table.
    bool                  is_output;      ///< True if the column carries result data (false for predicate-only columns).
    std::set<PredicateOp> pushable_ops;   ///< Operators the backend can evaluate natively (pushed down).
    std::set<PredicateOp> filterable_ops; ///< Operators the engine can evaluate post-fetch (in-memory filter).
    std::string           notes;          ///< Free-text annotations shown in schema introspection output.
};

/** @brief Pull source of Arrow record batches; nullptr from next() signals clean EOF. */
class IRecordBatchStream
{
public:
    virtual ~IRecordBatchStream() = default;
    /** @brief Returns the next batch, or nullptr at clean EOF. */
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

    /** @brief Returns the set of virtual table names this backend serves. */
    virtual std::set<std::string_view> virtualTables() const = 0;

    /**
     * @brief Returns the column schema for the named virtual table.
     * @param[in] table_name Virtual table name.
     * @return Column schema, or empty vector if unknown.
     */
    virtual std::vector<ColumnSchema>  tableSchema(std::string_view table_name) const = 0;

    /**
     * @brief Maximum number of independent streams this queryable can service concurrently for one query.
     * @return Maximum concurrent stream count; the default preserves serial execution.
     */
    virtual std::size_t maxConcurrentStreams() const noexcept { return 1; }

    /**
     * @brief Executes a valid table selection and returns a pull stream of Arrow batches.
     * @details The implementation is responsible for fetching all pages from the backend,
     *          spilling to disk as needed, and exposing the result as a pull stream.
     *          A null batch from next() signals clean EOF.
     * @param[in] table_name           Name of the virtual table to query.
     * @param[in] pushable_predicates  Predicates the backend can evaluate natively.
     * @param[in] projection_hint      Set of column names the caller will read; may be empty.
     * @param[in] context              Execution resources and controls.
     * @return Pull stream; nullptr from next() signals clean EOF.
     */
    virtual IRecordBatchStreamUPtr executeStream(std::string_view              table_name,
                                                 const std::vector<Predicate>& pushable_predicates,
                                                 const std::set<std::string>&  projection_hint,
                                                 const ExecutionContext&       context) = 0;
};

using IQueryableUPtr = std::unique_ptr<IQueryable>;

} // namespace mldp_pvxs_driver::query

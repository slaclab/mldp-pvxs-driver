//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <query/LiteralValue.h>

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mldp_pvxs_driver::query {

class ExecutionContext;
struct QueryResult;

enum class ColumnType
{
    STRING,
    TIMESTAMP,
    DURATION_SECONDS,
    INT,
    NATIVE_VALUE,
    BOOL
};
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
    IS_NOT_NULL
};

using ExecutableLiteralValue = std::variant<std::string, int64_t, double, bool, TimestampNsLiteral, DurationNsLiteral>;

struct Predicate
{
    std::string                                           column;
    PredicateOp                                           op;
    std::vector<ExecutableLiteralValue> values;
};

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

class IQueryable
{
public:
    virtual ~IQueryable() = default;

    virtual std::set<std::string_view> virtualTables() const = 0;
    virtual std::vector<ColumnSchema>  tableSchema(std::string_view table_name) const = 0;
    /**
     * Execute a valid table selection.
     *
     * A successful backend request that matches no records is represented by
     * an empty result; it is not an execution error. Transport failures,
     * backend exceptional responses, and malformed response payloads remain
     * errors.
     */
    virtual QueryResult                execute(std::string_view              table_name,
                                               const std::vector<Predicate>& pushable_predicates,
                                               const std::set<std::string>&  projection_hint,
                                               const ExecutionContext&       context,
                                               std::string_view              page_token = {}) = 0;
};

using IQueryableUPtr = std::unique_ptr<IQueryable>;

} // namespace mldp_pvxs_driver::query

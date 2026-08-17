//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


#include <query/executor/StateInternal.h>

#include <query/QueryableFactory.h>
#include <query/QueryTableCatalog.h>

#include <arrow/array/builder_binary.h>

#include <stdexcept>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;
namespace {

class State final : public ExecutionStateBase
{
public:
    State(const ExecutionContext& context, QueryStats& stats)
        : ExecutionStateBase(context, stats)
    {
    }

    std::string_view typeName() const noexcept override
    {
        return "ShowTablesExecutionState";
    }

    RecordBatches execute() override
    {
        arrow::StringBuilder names;
        arrow::StringBuilder types;
        arrow::StringBuilder locations;
        const auto append = [&names, &types, &locations](const std::string& name, const std::string& type, const std::string& location)
        {
            if (!names.Append(name).ok() || !types.Append(type).ok() || !locations.Append(location).ok())
            {
                throw std::runtime_error("Failed to append SHOW TABLES row");
            }
        };

        for (const auto& table : QueryableFactory::instance().registeredTables())
        {
            append(table, "virtual", "");
        }
        if (context().table_catalog)
        {
            for (const auto& table : context().table_catalog->tables())
            {
                append(table.name,
                       table.lifetime == TableLifetime::Session ? "session Arrow IPC" : "persistent Arrow IPC",
                       table.path);
            }
        }

        std::shared_ptr<arrow::Array> name_output;
        std::shared_ptr<arrow::Array> type_output;
        std::shared_ptr<arrow::Array> location_output;
        if (!names.Finish(&name_output).ok() || !types.Finish(&type_output).ok() || !locations.Finish(&location_output).ok())
        {
            throw std::runtime_error("Failed to build SHOW TABLES output");
        }
        const auto batch = arrow::RecordBatch::Make(
            arrow::schema({arrow::field("table_name", arrow::utf8()), arrow::field("type", arrow::utf8()), arrow::field("location", arrow::utf8())}),
            name_output->length(),
            {name_output, type_output, location_output});
        stats().rows_from_backend += static_cast<uint64_t>(batch->num_rows());
        return {batch};
    }
};

} // namespace

std::unique_ptr<IExecutionState> mldp_pvxs_driver::query::executor::makeShowTablesExecutionState(
    const plan::PhysicalShowTables&,
    const plan::PhysicalNodePtr&,
    const ExecutionContext& context,
    QueryStats& stats)
{
    return std::make_unique<State>(context, stats);
}

//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/ExecutorUtils.h>
#include <query/executor/ScanExecutionHelpers.h>
#include <query/executor/StateInternal.h>

#include <query/QueryPlanner.h>
#include <limits>
#include <stdexcept>

namespace mldp_pvxs_driver::query::executor {
namespace {

class TableScanExecutionState final : public ExecutionStateBase
{
public:
    TableScanExecutionState(plan::PhysicalTableScan scan, const plan::PhysicalNodePtr& physical, const ExecutionContext& context, QueryStats& stats)
        : ExecutionStateBase(context, stats), scan_(std::move(scan))
    {
        QueryPlanner planner(context.table_catalog);
        if (scan_.derived_query)
        {
            derived_child_index_ = children().size();
            addChild(planner.plan(QueryStatement{*scan_.derived_query}));
        }
        for (const auto& subquery : scan_.in_subqueries)
        {
            in_subquery_indices_.push_back(children().size());
            addChild(planner.plan(QueryStatement{*subquery.child}));
        }
        if (scan_.window_subquery)
        {
            window_child_index_ = children().size();
            addChild(planner.plan(QueryStatement{*scan_.window_subquery}));
        }
    }

    std::string_view typeName() const noexcept override { return "TableScanExecutionState"; }

    RecordBatches execute() override
    {
        if (scan_.derived_query)
        {
            auto output = childAt(*derived_child_index_).execute();
            if (!applyMembershipPredicates(output, false)) return {};
            qualify(output);
            return output;
        }

        if (scan_.arrow_ipc)
        {
            auto output = readCatalogTable(scan_, context());
            if (!applyMembershipPredicates(output, false)) return {};
            qualify(output);
            return output;
        }

        std::vector<Predicate> pushable = scan_.pushable_predicates;
        std::vector<Predicate> local;
        if (!materializeMembershipPredicates(pushable, local)) return {};
        const auto wide_table = scan_.table_name == "mldp.time_series_table" &&
                                (!scan_.in_subqueries.empty() || scan_.window_subquery || scan_.window_literal);
        if (!wide_table)
        {
            return fetchBackendPages(scan_, pushable, local, context(), stats());
        }

        std::vector<std::pair<int64_t, int64_t>> windows;
        if (window_child_index_)
            windows = extractNormalizedWindows(childAt(*window_child_index_).execute());
        else if (scan_.window_literal)
            windows.emplace_back((*scan_.window_literal)[0] * 1'000'000'000LL, (*scan_.window_literal)[1] * 1'000'000'000LL);
        else
            windows.emplace_back(0, std::numeric_limits<int64_t>::max());
        if (windows.empty()) return {};

        return fetchWideTableWindows(scan_, pushable, local, windows, context(), stats());
    }

private:
    bool applyMembershipPredicates(RecordBatches& batches, const bool partition_pushable)
    {
        std::vector<Predicate> ignored;
        std::vector<Predicate> predicates;
        if (!materializeMembershipPredicates(predicates, ignored, partition_pushable)) return false;
        if (predicates.empty()) return true;
        for (auto& batch : batches)
        {
            auto filtered = applyFilter(batch, predicates);
            if (!filtered.ok()) throw std::runtime_error(filtered.status().ToString());
            batch = *filtered;
        }
        return true;
    }

    bool materializeMembershipPredicates(std::vector<Predicate>& pushable, std::vector<Predicate>& local, const bool partition = true)
    {
        for (std::size_t index = 0; index < scan_.in_subqueries.size(); ++index)
        {
            const auto& subquery = scan_.in_subqueries[index];
            auto predicate = subquery.predicate;
            predicate.values = extractInSubqueryValues(childAt(in_subquery_indices_[index]).execute(), subquery.column_type, predicate.column);
            if (predicate.values.empty()) return false;
            if (partition && !subquery.pushable) local.push_back(std::move(predicate));
            else pushable.push_back(std::move(predicate));
        }
        return true;
    }

    std::shared_ptr<arrow::RecordBatch> applyLocalPredicates(const std::shared_ptr<arrow::RecordBatch>& batch, const std::vector<Predicate>& predicates)
    {
        if (predicates.empty()) return batch;
        auto filtered = applyFilter(batch, predicates);
        if (!filtered.ok()) throw std::runtime_error(filtered.status().ToString());
        return *filtered;
    }

    void qualify(RecordBatches& batches) const
    {
        if (scan_.qualify_output)
            for (auto& batch : batches) batch = qualifyBatchColumns(batch, scan_.table_alias);
    }

    plan::PhysicalTableScan scan_;
    std::optional<std::size_t> derived_child_index_;
    std::vector<std::size_t> in_subquery_indices_;
    std::optional<std::size_t> window_child_index_;
};

} // namespace

std::unique_ptr<IExecutionState> makeScanExecutionState(const plan::PhysicalTableScan& scan, const plan::PhysicalNodePtr& physical, const ExecutionContext& context, QueryStats& stats)
{
    return std::make_unique<TableScanExecutionState>(scan, physical, context, stats);
}

} // namespace mldp_pvxs_driver::query::executor

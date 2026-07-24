//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <query/executor/ExecutionState.h>

#include <stdexcept>
#include <utility>

namespace mldp_pvxs_driver::query::executor {


class ExecutionStateBase : public IExecutionState
{
public:
    ExecutionStateBase(const ExecutionContext& context, QueryStats& stats)
        : context_(context), stats_(stats)
    {
    }

    const std::vector<std::unique_ptr<IExecutionState>>& children() const noexcept override
    {
        return children_;
    }

protected:
    const ExecutionContext& context() const noexcept
    {
        return context_;
    }

    QueryStats& stats() const noexcept
    {
        return stats_;
    }

    IExecutionState& childAt(const std::size_t index) const
    {
        if (index >= children_.size())
        {
            throw std::runtime_error("Execution state child index is out of range");
        }
        return *children_[index];
    }

    void addChild(const plan::PhysicalNodePtr& child)
    {
        if (!child)
        {
            throw std::runtime_error("Physical execution state has a null child node");
        }
        children_.push_back(makeExecutionState(child, context_, stats_));
    }

private:
    const ExecutionContext&                        context_;
    QueryStats&                                    stats_;
    std::vector<std::unique_ptr<IExecutionState>> children_;
};

std::unique_ptr<IExecutionState> makeScanExecutionState(const plan::PhysicalTableScan&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
std::unique_ptr<IExecutionState> makeRelationalExecutionState(const plan::PhysicalNodeVariant&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
std::unique_ptr<IExecutionState> makeStatementExecutionState(const plan::PhysicalNodeVariant&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);

std::unique_ptr<IExecutionState> makeFilterExecutionState(const plan::PhysicalFilter&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
std::unique_ptr<IExecutionState> makeProjectExecutionState(const plan::PhysicalProject&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
std::unique_ptr<IExecutionState> makeSortExecutionState(const plan::PhysicalSort&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
std::unique_ptr<IExecutionState> makeLimitExecutionState(const plan::PhysicalLimit&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
std::unique_ptr<IExecutionState> makeHashJoinExecutionState(const plan::PhysicalHashJoin&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
std::unique_ptr<IExecutionState> makeNestedLoopJoinExecutionState(const plan::PhysicalNestedLoopJoin&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
std::unique_ptr<IExecutionState> makeBlockNestedLoopJoinExecutionState(const plan::PhysicalBlockNestedLoopJoin&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
std::unique_ptr<IExecutionState> makeShowTablesExecutionState(const plan::PhysicalShowTables&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
std::unique_ptr<IExecutionState> makeDescribeExecutionState(const plan::PhysicalDescribe&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
std::unique_ptr<IExecutionState> makeExplainExecutionState(const plan::PhysicalExplain&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
std::unique_ptr<IExecutionState> makeCreateTableExecutionState(const plan::PhysicalCreateTable&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
std::unique_ptr<IExecutionState> makeDropTableExecutionState(const plan::PhysicalDropTable&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);

} // namespace mldp_pvxs_driver::query::executor

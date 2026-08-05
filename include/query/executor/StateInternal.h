//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


/** @file StateInternal.h
 * @brief Declares internal materialized-execution state helpers. */
#pragma once

#include <query/executor/ExecutionState.h>
#include <query/QueryCancellation.h>

#include <stdexcept>

namespace mldp_pvxs_driver::query::executor {


/** @brief Base implementation that owns child states and shared execution context. */
class ExecutionStateBase : public IExecutionState
{
public:
    /** @brief Constructs the base state with the shared execution context and statistics.
     * @param[in] context Execution context.
     * @param[in,out] stats Statistics accumulator. */
    ExecutionStateBase(const ExecutionContext& context, QueryStats& stats)
        : context_(context), stats_(stats)
    {
    }

    const std::vector<std::unique_ptr<IExecutionState>>& children() const noexcept override
    {
        return children_;
    }

protected:
    /** @brief Returns the shared execution context.
     * @return Const reference to the context. */
    const ExecutionContext& context() const noexcept
    {
        return context_;
    }

    /** @brief Returns the shared statistics accumulator.
     * @return Reference to stats. */
    QueryStats& stats() const noexcept
    {
        return stats_;
    }

    /** @brief Throws QueryCancelled if cancellation has been requested.
     * @throws QueryCancelled If the context's cancellation token is set. */
    void throwIfCancelled() const
    {
        if (context_.cancellation) context_.cancellation->throwIfCancelled();
    }

    /** @brief Returns the child state at the given index.
     * @param[in] index Zero-based child index.
     * @return Reference to the child.
     * @throws std::runtime_error If index is out of range. */
    IExecutionState& childAt(const std::size_t index) const
    {
        if (index >= children_.size())
        {
            throw std::runtime_error("Execution state child index is out of range");
        }
        return *children_[index];
    }

    /** @brief Builds a child execution state from a physical plan node and registers it.
     * @param[in] child Physical plan child node; must not be null.
     * @throws std::runtime_error If child is null. */
    void addChild(const plan::PhysicalNodePtr& child)
    {
        if (!child)
        {
            throw std::runtime_error("Physical execution state has a null child node");
        }
        children_.push_back(makeExecutionState(child, context_, stats_));
    }

private:
    const ExecutionContext&                        context_;   ///< Shared execution context.
    QueryStats&                                    stats_;     ///< Shared statistics accumulator.
    std::vector<std::unique_ptr<IExecutionState>> children_;  ///< Child execution state nodes.
};

/** @brief Creates an execution state for the given physical node. @return Execution state. */
std::unique_ptr<IExecutionState> makeScanExecutionState(const plan::PhysicalTableScan&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
/** @brief Creates an execution state for the given physical node. @return Execution state. */
std::unique_ptr<IExecutionState> makePivotExecutionState(const plan::PhysicalPivot&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
/** @brief Creates an execution state for the given physical node. @return Execution state. */
std::unique_ptr<IExecutionState> makeRelationalExecutionState(const plan::PhysicalNodeVariant&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
/** @brief Creates an execution state for the given physical node. @return Execution state. */
std::unique_ptr<IExecutionState> makeStatementExecutionState(const plan::PhysicalNodeVariant&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);

/** @brief Creates an execution state for the given physical node. @return Execution state. */
std::unique_ptr<IExecutionState> makeFilterExecutionState(const plan::PhysicalFilter&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
/** @brief Creates an execution state for the given physical node. @return Execution state. */
std::unique_ptr<IExecutionState> makeProjectExecutionState(const plan::PhysicalProject&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
/** @brief Creates an execution state for the given physical node. @return Execution state. */
std::unique_ptr<IExecutionState> makeSortExecutionState(const plan::PhysicalSort&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
/** @brief Creates an execution state for the given physical node. @return Execution state. */
std::unique_ptr<IExecutionState> makeLimitExecutionState(const plan::PhysicalLimit&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
/** @brief Creates an execution state for the given physical node. @return Execution state. */
std::unique_ptr<IExecutionState> makeHashJoinExecutionState(const plan::PhysicalHashJoin&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
/** @brief Creates an execution state for the given physical node. @return Execution state. */
std::unique_ptr<IExecutionState> makeNestedLoopJoinExecutionState(const plan::PhysicalNestedLoopJoin&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
/** @brief Creates an execution state for the given physical node. @return Execution state. */
std::unique_ptr<IExecutionState> makeBlockNestedLoopJoinExecutionState(const plan::PhysicalBlockNestedLoopJoin&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
/** @brief Creates an execution state for the given physical node. @return Execution state. */
std::unique_ptr<IExecutionState> makeShowTablesExecutionState(const plan::PhysicalShowTables&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
/** @brief Creates an execution state for the given physical node. @return Execution state. */
std::unique_ptr<IExecutionState> makeShowFunctionsExecutionState(const plan::PhysicalShowFunctions&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
/** @brief Creates an execution state for the given physical node. @return Execution state. */
std::unique_ptr<IExecutionState> makeShowOperatorsExecutionState(const plan::PhysicalShowOperators&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
/** @brief Creates an execution state for the given physical node. @return Execution state. */
std::unique_ptr<IExecutionState> makeDescribeExecutionState(const plan::PhysicalDescribe&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
/** @brief Creates an execution state for the given physical node. @return Execution state. */
std::unique_ptr<IExecutionState> makeExplainExecutionState(const plan::PhysicalExplain&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
/** @brief Creates an execution state for the given physical node. @return Execution state. */
std::unique_ptr<IExecutionState> makeCreateTableExecutionState(const plan::PhysicalCreateTable&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);
/** @brief Creates an execution state for the given physical node. @return Execution state. */
std::unique_ptr<IExecutionState> makeDropTableExecutionState(const plan::PhysicalDropTable&, const plan::PhysicalNodePtr&, const ExecutionContext&, QueryStats&);

} // namespace mldp_pvxs_driver::query::executor

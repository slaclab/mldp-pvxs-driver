//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <query/IQueryable.h>
#include <query/QueryStats.h>
#include <query/ExecutionContext.h>
#include <query/plan/PhysicalPlan.h>

#include <future>
#include <memory>
#include <utility>
#include <vector>

namespace mldp_pvxs_driver::query {

class WindowBackendScanRecordBatchStream final : public IRecordBatchStream
{
public:
    WindowBackendScanRecordBatchStream(const plan::PhysicalTableScan& scan,
                                       ExecutionContext context,
                                       std::shared_ptr<QueryStats> stats,
                                       std::vector<std::pair<int64_t, int64_t>> windows);
    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    struct PullResult { IRecordBatchStreamUPtr stream; std::shared_ptr<arrow::RecordBatch> batch; };
    struct Group {
        std::size_t index{0};
        uint64_t slice_index{0};
        uint64_t series_in_shard{0};
        std::vector<Predicate> predicates;
        IRecordBatchStreamUPtr stream;
        std::future<PullResult> next;
    };
    void scheduleFirst(Group& group);
    void scheduleNext(Group& group);
    void prepareNextSlice();
    void selectWindow();

    plan::PhysicalTableScan scan_;
    ExecutionContext context_;
    std::shared_ptr<QueryStats> stats_;
    IQueryableUPtr queryable_;
    std::vector<std::string> requested_pvs_;
    std::vector<Group> groups_;
    std::size_t group_index_{0};
    std::size_t next_group_to_start_{0};
    uint64_t parallel_shard_limit_{0};
    std::vector<std::pair<int64_t, int64_t>> windows_;
    std::size_t window_index_{0};
    int64_t window_begin_ns_{0};
    int64_t window_end_ns_{0};
    int64_t slice_begin_ns_{0};
    int64_t slice_end_ns_{0};
    bool final_slice_{false};
};

} // namespace mldp_pvxs_driver::query

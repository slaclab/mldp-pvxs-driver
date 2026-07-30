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

#include <query/ExecutionContext.h>
#include <query/IQueryable.h>
#include <query/QueryCancellation.h>

#include <future>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::query::impl::mldp {

class MLDPQueryClient;

/** Pull stream that executes independent PV shards concurrently in order. */
class ParallelSeriesRecordBatchStream final : public mldp_pvxs_driver::query::IRecordBatchStream
{
public:
    ParallelSeriesRecordBatchStream(MLDPQueryClient& client,
                                    std::string table_name,
                                    std::vector<mldp_pvxs_driver::query::Predicate> predicates,
                                    std::set<std::string> projection_hint,
                                    mldp_pvxs_driver::query::ExecutionContext context,
                                    const std::vector<std::string>& pvs);
    ~ParallelSeriesRecordBatchStream() override;

    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    struct PullResult
    {
        mldp_pvxs_driver::query::IRecordBatchStreamUPtr stream;
        std::shared_ptr<arrow::RecordBatch> batch;
    };

    struct Group
    {
        std::size_t index{0};
        uint64_t series{0};
        std::vector<mldp_pvxs_driver::query::Predicate> predicates;
        mldp_pvxs_driver::query::IRecordBatchStreamUPtr stream;
        std::future<PullResult> next;
    };

    void schedule(Group& group);
    void scheduleNext(Group& group);
    void updateProgress();
    void cancelAndDrain() noexcept;

    MLDPQueryClient& client_;
    std::string table_name_;
    std::vector<mldp_pvxs_driver::query::Predicate> predicates_;
    std::set<std::string> projection_hint_;
    mldp_pvxs_driver::query::ExecutionContext context_;
    std::shared_ptr<mldp_pvxs_driver::query::QueryCancellation> shard_cancellation_;
    mldp_pvxs_driver::query::QueryCancellation::Registration cancellation_registration_;
    std::vector<Group> groups_;
    std::size_t current_{0};
    std::size_t next_to_start_{0};
    std::size_t limit_{0};
    bool drained_{false};
};

} // namespace mldp_pvxs_driver::query::impl::mldp

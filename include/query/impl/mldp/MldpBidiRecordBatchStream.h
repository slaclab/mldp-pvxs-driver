//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file MldpBidiRecordBatchStream.h
 * @brief gRPC bidirectional streaming cursor for mldp.time_series. */
#pragma once

#include <pool/MLDPGrpcQueryPool.h>
#include <query/ExecutionContext.h>
#include <query/IQueryable.h>
#include <query/QueryCancellation.h>

#include <query.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::query::impl::mldp {

/** @brief Drives a queryDataBidiStream RPC cursor and converts each response to an Arrow RecordBatch. */
class MldpBidiRecordBatchStream final : public mldp_pvxs_driver::query::IRecordBatchStream
{
public:
    MldpBidiRecordBatchStream(mldp_pvxs_driver::util::pool::PooledHandle<mldp_pvxs_driver::util::pool::MLDPGrpcObject> handle,
                              dp::service::query::QueryDataRequest                                                     request,
                              std::vector<mldp_pvxs_driver::query::Predicate>                                          column_predicates,
                              const std::set<std::string>&                                                             projection_hint,
                              mldp_pvxs_driver::query::ExecutionContext                                                context);

    ~MldpBidiRecordBatchStream() override;

    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    std::shared_ptr<arrow::RecordBatch> makeBatch(const dp::service::query::QueryDataResponse& response) const;

    mldp_pvxs_driver::util::pool::PooledHandle<mldp_pvxs_driver::util::pool::MLDPGrpcObject>                               handle_;
    mldp_pvxs_driver::query::ExecutionContext                                                                               context_;
    std::vector<mldp_pvxs_driver::query::Predicate>                                                                        column_predicates_;
    std::set<std::string>                                                                                                  projection_hint_;
    std::shared_ptr<grpc::ClientContext>                                                                                   rpc_context_;
    std::unique_ptr<grpc::ClientReaderWriter<dp::service::query::QueryDataRequest, dp::service::query::QueryDataResponse>> stream_;
    mldp_pvxs_driver::query::QueryCancellation::Registration                                                               cancellation_registration_;
    bool                                                                                                                   request_next_{false};
    bool                                                                                                                   finished_{false};
};

} // namespace mldp_pvxs_driver::query::impl::mldp

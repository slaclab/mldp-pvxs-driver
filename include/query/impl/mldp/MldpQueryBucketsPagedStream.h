//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file MldpQueryBucketsPagedStream.h
 * @brief Backend-paged (dp-grpc V2 queryBuckets) stream for mldp.time_series. */
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

/** @brief Default number of buckets requested per backend page. */
constexpr uint32_t kDefaultBucketPageSize = 5000;

/** @brief Drives repeated unary queryBuckets RPCs using ExecutionOptions{limit, pageToken}
 *         and converts each backend page to one Arrow RecordBatch. */
class MldpQueryBucketsPagedStream final : public mldp_pvxs_driver::query::IRecordBatchStream
{
public:
    MldpQueryBucketsPagedStream(mldp_pvxs_driver::util::pool::PooledHandle<mldp_pvxs_driver::util::pool::MLDPGrpcObject> handle,
                               dp::service::query::QuerySpec                                                            query_spec,
                               std::vector<mldp_pvxs_driver::query::Predicate>                                          column_predicates,
                               const std::set<std::string>&                                                             projection_hint,
                               mldp_pvxs_driver::query::ExecutionContext                                                context,
                               uint32_t                                                                                 page_size = kDefaultBucketPageSize);

    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    mldp_pvxs_driver::util::pool::PooledHandle<mldp_pvxs_driver::util::pool::MLDPGrpcObject> handle_;
    dp::service::query::QuerySpec                                                            query_spec_;
    mldp_pvxs_driver::query::ExecutionContext                                                context_;
    std::vector<mldp_pvxs_driver::query::Predicate>                                          column_predicates_;
    std::set<std::string>                                                                    projection_hint_;
    uint32_t                                                                                 page_size_;
    std::string                                                                              next_page_token_;
    bool                                                                                      first_page_{true};
    bool                                                                                      finished_{false};
};

} // namespace mldp_pvxs_driver::query::impl::mldp

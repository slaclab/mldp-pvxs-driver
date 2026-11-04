//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file MldpQuerySamplesPagedStream.h
 * @brief Backend-paged (dp-grpc V2 querySamples) page source for mldp.time_series_table.
 * @details The wide/pivot table format requires all pages to be merged into a single
 *          columnar result before it can be materialized, so this class exposes raw
 *          ColumnTable pages rather than the IRecordBatchStream interface. */
#pragma once

#include <pool/MLDPGrpcQueryPool.h>
#include <query/ExecutionContext.h>
#include <query/IQueryable.h>
#include <query/QueryCancellation.h>

#include <query.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <optional>

namespace mldp_pvxs_driver::query::impl::mldp {

/** @brief Default number of samples requested per backend page. */
constexpr uint32_t kDefaultSamplePageSize = 5000;

/** @brief Drives repeated unary querySamples RPCs using ExecutionOptions{limit, pageToken}. */
class MldpQuerySamplesPagedStream final
{
public:
    MldpQuerySamplesPagedStream(mldp_pvxs_driver::util::pool::PooledHandle<mldp_pvxs_driver::util::pool::MLDPGrpcObject> handle,
                               dp::service::query::QuerySpec                                                            query_spec,
                               mldp_pvxs_driver::query::ExecutionContext                                                context,
                               uint32_t                                                                                 page_size = kDefaultSamplePageSize);

    /** @brief Returns the next backend page, or nullopt at clean EOF. */
    std::optional<dp::service::query::ColumnTable> next();

private:
    mldp_pvxs_driver::util::pool::PooledHandle<mldp_pvxs_driver::util::pool::MLDPGrpcObject> handle_;
    dp::service::query::QuerySpec                                                            query_spec_;
    mldp_pvxs_driver::query::ExecutionContext                                                context_;
    uint32_t                                                                                 page_size_;
    std::string                                                                              next_page_token_;
    bool                                                                                      first_page_{true};
    bool                                                                                      finished_{false};
};

} // namespace mldp_pvxs_driver::query::impl::mldp

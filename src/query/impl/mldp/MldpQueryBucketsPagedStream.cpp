//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/impl/mldp/MldpQueryBucketsPagedStream.h>
#include <query/impl/mldp/MldpBidiRecordBatchStream.h>

#include <query/QueryCancellation.h>
#include <query/QueryProgress.h>

#include <arrow/record_batch.h>

#include <stdexcept>

using namespace mldp_pvxs_driver::query::impl::mldp;
using namespace mldp_pvxs_driver::query;

MldpQueryBucketsPagedStream::MldpQueryBucketsPagedStream(
    mldp_pvxs_driver::util::pool::PooledHandle<mldp_pvxs_driver::util::pool::MLDPGrpcObject> handle,
    dp::service::query::QuerySpec                                                            query_spec,
    std::vector<Predicate>                                                                   column_predicates,
    const std::set<std::string>&                                                             projection_hint,
    ExecutionContext                                                                         context,
    const uint32_t                                                                           page_size)
    : handle_(std::move(handle)), query_spec_(std::move(query_spec)), context_(std::move(context)),
      column_predicates_(std::move(column_predicates)), projection_hint_(projection_hint), page_size_(page_size)
{
}

std::shared_ptr<arrow::RecordBatch> MldpQueryBucketsPagedStream::next()
{
    if (finished_)
        return nullptr;
    if (context_.cancellation)
        context_.cancellation->throwIfCancelled();

    dp::service::query::QueryBucketsRequest request;
    *request.mutable_queryspec() = query_spec_;
    request.mutable_executionoptions()->set_limit(page_size_);
    if (!first_page_)
        request.mutable_executionoptions()->set_pagetoken(next_page_token_);
    first_page_ = false;

    if (context_.progress)
    {
        context_.progress->setActivity("mldp.time_series", "MLDP queryBuckets (paged)", "requesting backend page");
        context_.progress->cursorNext();
    }

    auto        rpc_context = std::make_shared<grpc::ClientContext>();
    auto        cancellation_registration = context_.cancellation
                                                ? context_.cancellation->onCancel([rpc_context]
                                                                                  { rpc_context->TryCancel(); })
                                                : QueryCancellation::Registration{};
    dp::service::query::QueryBucketsResponse response;
    const auto status = handle_->query_stub->queryBuckets(rpc_context.get(), request, &response);
    if (context_.cancellation && context_.cancellation->cancelled())
        throw QueryCancelled{};
    if (!status.ok())
        throw std::runtime_error("MLDP queryBuckets failed: " + status.error_message());
    if (!response.has_bucketqueryresult())
        throw std::runtime_error("MLDP queryBuckets failed: " + response.exceptionalresult().message());

    const auto& result = response.bucketqueryresult();
    next_page_token_ = result.nextpagetoken();
    if (result.databuckets().empty() && next_page_token_.empty())
    {
        finished_ = true;
        return nullptr;
    }
    if (next_page_token_.empty())
        finished_ = true;

    auto* pool = context_.pool != nullptr ? context_.pool : arrow::default_memory_pool();
    auto  batch = decodeDataBucketsToBatch(result.databuckets(), column_predicates_, projection_hint_, pool);
    if (context_.progress)
        context_.progress->cursorResponse(static_cast<uint64_t>(batch->num_rows()));
    return batch;
}

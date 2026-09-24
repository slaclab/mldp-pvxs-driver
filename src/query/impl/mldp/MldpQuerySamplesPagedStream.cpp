//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/impl/mldp/MldpQuerySamplesPagedStream.h>

#include <query/QueryCancellation.h>
#include <query/QueryProgress.h>

#include <stdexcept>

using namespace mldp_pvxs_driver::query::impl::mldp;
using namespace mldp_pvxs_driver::query;

MldpQuerySamplesPagedStream::MldpQuerySamplesPagedStream(
    mldp_pvxs_driver::util::pool::PooledHandle<mldp_pvxs_driver::util::pool::MLDPGrpcObject> handle,
    dp::service::query::QuerySpec                                                            query_spec,
    ExecutionContext                                                                         context,
    const uint32_t                                                                           page_size)
    : handle_(std::move(handle)), query_spec_(std::move(query_spec)), context_(std::move(context)), page_size_(page_size)
{
}

std::optional<dp::service::query::ColumnTable> MldpQuerySamplesPagedStream::next()
{
    if (finished_)
        return std::nullopt;
    if (context_.cancellation)
        context_.cancellation->throwIfCancelled();

    dp::service::query::QuerySamplesRequest request;
    *request.mutable_queryspec() = query_spec_;
    request.mutable_executionoptions()->set_limit(page_size_);
    if (!first_page_)
        request.mutable_executionoptions()->set_pagetoken(next_page_token_);
    first_page_ = false;

    if (context_.progress)
    {
        context_.progress->setActivity("mldp.time_series_table", "MLDP querySamples (paged)", "requesting backend page");
        context_.progress->cursorNext();
    }

    auto        rpc_context = std::make_shared<grpc::ClientContext>();
    auto        cancellation_registration = context_.cancellation
                                                ? context_.cancellation->onCancel([rpc_context]
                                                                                  { rpc_context->TryCancel(); })
                                                : QueryCancellation::Registration{};
    dp::service::query::QuerySamplesResponse response;
    const auto status = handle_->query_stub->querySamples(rpc_context.get(), request, &response);
    if (context_.cancellation && context_.cancellation->cancelled())
        throw QueryCancelled{};
    if (!status.ok())
        throw std::runtime_error("MLDP querySamples failed: " + status.error_message());
    if (!response.has_samplequeryresult())
        throw std::runtime_error("MLDP querySamples failed: " + response.exceptionalresult().message());

    const auto& result = response.samplequeryresult();
    next_page_token_ = result.nextpagetoken();
    if (next_page_token_.empty())
        finished_ = true;

    if (context_.progress)
        context_.progress->cursorResponse(static_cast<uint64_t>(result.columntable().timestamplist().timestamps_size()));
    return result.columntable();
}

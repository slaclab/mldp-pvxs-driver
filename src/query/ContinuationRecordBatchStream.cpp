//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#include <query/ContinuationRecordBatchStream.h>

#include <query/QueryCancellation.h>

#include <utility>

using namespace mldp_pvxs_driver::query;

ContinuationRecordBatchStream::ContinuationRecordBatchStream(IQueryable& queryable,
                                                             std::string table_name,
                                                             std::vector<Predicate> predicates,
                                                             std::set<std::string> projection_hint,
                                                             const ExecutionContext& context,
                                                             std::string page_token)
    : queryable_(queryable), table_name_(std::move(table_name)), predicates_(std::move(predicates)),
      projection_hint_(std::move(projection_hint)), context_(context), page_token_(std::move(page_token))
{
}

std::shared_ptr<arrow::RecordBatch> ContinuationRecordBatchStream::next()
{
    while (!eof_)
    {
        if (context_.cancellation) context_.cancellation->throwIfCancelled();
        const auto result = queryable_.execute(table_name_, predicates_, projection_hint_, context_, page_token_);
        page_token_ = result.next_page_token;
        eof_ = page_token_.empty();
        if (result.batch) return result.batch;
    }
    return nullptr;
}

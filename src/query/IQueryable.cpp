//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////

#include <query/IQueryable.h>

#include <query/ExecutionContext.h>
#include <query/QueryCancellation.h>
#include <query/QueryResult.h>

using namespace mldp_pvxs_driver::query;

namespace {

class ContinuationRecordBatchStream final : public IRecordBatchStream
{
public:
    ContinuationRecordBatchStream(IQueryable& queryable,
                                  std::string table_name,
                                  std::vector<Predicate> predicates,
                                  std::set<std::string> projection_hint,
                                  const ExecutionContext& context,
                                  std::string page_token)
        : queryable_(queryable), table_name_(std::move(table_name)), predicates_(std::move(predicates)),
          projection_hint_(std::move(projection_hint)), context_(context), page_token_(std::move(page_token)) {}

    std::shared_ptr<arrow::RecordBatch> next() override
    {
        while (!eof_)
        {
            if (context_.cancellation) context_.cancellation->throwIfCancelled();
            const auto result = queryable_.execute(table_name_, predicates_, projection_hint_, context_, page_token_);
            page_token_ = result.next_page_token;
            eof_ = page_token_.empty();
            // A continuation protocol may legitimately return an empty page
            // before a later page contains rows.  Only a null batch *and* no
            // continuation token denotes clean EOF to the pull consumer.
            if (result.batch) return result.batch;
        }
        return nullptr;
    }

private:
    IQueryable& queryable_;
    std::string table_name_;
    std::vector<Predicate> predicates_;
    std::set<std::string> projection_hint_;
    const ExecutionContext& context_;
    std::string page_token_;
    bool eof_{false};
};

} // namespace

IRecordBatchStreamUPtr IQueryable::executeStream(const std::string_view table_name,
                                                  const std::vector<Predicate>& pushable_predicates,
                                                  const std::set<std::string>& projection_hint,
                                                  const ExecutionContext& context,
                                                  const std::string_view page_token)
{
    return std::make_unique<ContinuationRecordBatchStream>(*this, std::string(table_name), pushable_predicates,
                                                            projection_hint, context, std::string(page_token));
}

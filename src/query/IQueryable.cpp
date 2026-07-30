//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////

#include <query/IQueryable.h>
#include <query/ContinuationRecordBatchStream.h>

#include <query/ExecutionContext.h>
#include <query/QueryResult.h>

using namespace mldp_pvxs_driver::query;

IRecordBatchStreamUPtr IQueryable::executeStream(const std::string_view table_name,
                                                  const std::vector<Predicate>& pushable_predicates,
                                                  const std::set<std::string>& projection_hint,
                                                  const ExecutionContext& context,
                                                  const std::string_view page_token)
{
    return std::make_unique<ContinuationRecordBatchStream>(*this, std::string(table_name), pushable_predicates,
                                                            projection_hint, context, std::string(page_token));
}

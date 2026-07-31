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

#include <query/IQueryable.h>

#include <string>

namespace mldp_pvxs_driver::query {

class ContinuationRecordBatchStream final : public IRecordBatchStream
{
public:
    ContinuationRecordBatchStream(IQueryable& queryable,
                                  std::string table_name,
                                  std::vector<Predicate> predicates,
                                  std::set<std::string> projection_hint,
                                  const ExecutionContext& context,
                                  std::string page_token);
    std::shared_ptr<arrow::RecordBatch> next() override;

private:
    IQueryable& queryable_;
    std::string table_name_;
    std::vector<Predicate> predicates_;
    std::set<std::string> projection_hint_;
    const ExecutionContext& context_;
    std::string page_token_;
    bool eof_{false};
};

} // namespace mldp_pvxs_driver::query

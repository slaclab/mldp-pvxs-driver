//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/CreateTableRecordBatchStream.h>

#include <query/QueryTableCatalog.h>

#include <stdexcept>
#include <utility>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

CreateTableRecordBatchStream::CreateTableRecordBatchStream(IRecordBatchStreamUPtr input,
                                                           const plan::PhysicalCreateTable& create,
                                                           ExecutionContext context,
                                                           std::shared_ptr<QueryStats> stats)
    : input_(std::move(input)), create_(create), context_(std::move(context)), stats_(std::move(stats))
{
}

std::shared_ptr<arrow::RecordBatch> CreateTableRecordBatchStream::next()
{
    if (done_) return nullptr;
    done_ = true;
    if (!context_.table_catalog) throw std::runtime_error("CREATE TABLE has no catalog");
    const auto status = context_.table_catalog->create(create_.table_name,
                                                        create_.temporary ? TableLifetime::Session : TableLifetime::Persistent,
                                                        *input_);
    if (!status.ok()) throw std::runtime_error(status.ToString());
    if (const auto table = context_.table_catalog->find(create_.table_name))
    {
        ++stats_->materialized_files;
        stats_->materialized_bytes += static_cast<uint64_t>(table->byte_count);
    }
    return nullptr;
}

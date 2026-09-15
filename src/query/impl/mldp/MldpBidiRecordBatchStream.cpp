//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/impl/mldp/MldpBidiRecordBatchStream.h>
#include <query/impl/mldp/ColumnPredicateFilter.h>
#include <query/impl/mldp/DataValueBuilder.h>
#include <query/impl/mldp/MldpTimestampUtils.h>
#include <query/impl/mldp/TimeSeriesMetadataBuilders.h>

#include <query/QueryCancellation.h>
#include <query/QueryProgress.h>

#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/record_batch.h>
#include <arrow/type.h>

#include <stdexcept>

using namespace mldp_pvxs_driver::query::impl::mldp;
using namespace mldp_pvxs_driver::query;

namespace {

std::set<std::string> attributeKeys(const std::vector<dp::service::common::ColumnMetadata>& metadata)
{
    std::set<std::string> keys;
    for (const auto& column_metadata : metadata)
        for (const auto& attribute : column_metadata.attributes())
            keys.insert(attribute.name());
    return keys;
}

std::set<std::string> provenanceKeys(const std::vector<dp::service::common::ColumnMetadata>& metadata)
{
    std::set<std::string> keys;
    for (const auto& column_metadata : metadata)
    {
        if (!column_metadata.provenance().source().empty())
            keys.insert("source");
        if (!column_metadata.provenance().process().empty())
            keys.insert("process");
    }
    return keys;
}

void addRequestedDynamicMetadataKeys(std::set<std::string>&       keys,
                                     const std::set<std::string>& projection_hint,
                                     const std::string_view       prefix)
{
    for (const auto& column : projection_hint)
        if (column.rfind(prefix, 0) == 0 && column.size() > prefix.size())
            keys.insert(column.substr(prefix.size()));
}

std::vector<int64_t> bucketTimestamps(const dp::service::common::DataBucket& bucket)
{
    if (!bucket.has_datatimestamps())
        throw std::runtime_error("MLDP queryDataBidiStream bucket has no timestamps");
    std::vector<int64_t> timestamps;
    const auto&          source = bucket.datatimestamps();
    if (source.has_timestamplist())
    {
        timestamps.reserve(source.timestamplist().timestamps_size());
        for (const auto& timestamp : source.timestamplist().timestamps())
            timestamps.push_back(timestampToNanoseconds(timestamp));
        return timestamps;
    }
    if (source.has_samplingclock())
    {
        const auto& clock = source.samplingclock();
        if (!clock.has_starttime())
            throw std::runtime_error("MLDP queryDataBidiStream sampling clock has no start time");
        timestamps.reserve(clock.count());
        const auto start = timestampToNanoseconds(clock.starttime());
        for (uint32_t index = 0; index < clock.count(); ++index)
            timestamps.push_back(start + static_cast<int64_t>(index) * static_cast<int64_t>(clock.periodnanos()));
        return timestamps;
    }
    throw std::runtime_error("MLDP queryDataBidiStream bucket has unsupported timestamps");
}

} // namespace

MldpBidiRecordBatchStream::MldpBidiRecordBatchStream(
    mldp_pvxs_driver::util::pool::PooledHandle<mldp_pvxs_driver::util::pool::MLDPGrpcObject> handle,
    dp::service::query::QueryDataRequest                                                     request,
    std::vector<Predicate>                                                                   column_predicates,
    const std::set<std::string>&                                                             projection_hint,
    ExecutionContext                                                                         context)
    : handle_(std::move(handle)), context_(std::move(context)), column_predicates_(std::move(column_predicates)),
      projection_hint_(projection_hint), rpc_context_(std::make_shared<grpc::ClientContext>())
{
    if (context_.cancellation)
        cancellation_registration_ = context_.cancellation->onCancel([rpc_context = rpc_context_]
                                                                     {
                                                                         rpc_context->TryCancel();
                                                                     });
    stream_ = handle_->query_stub->queryDataBidiStream(rpc_context_.get());
    if (!stream_->Write(request))
        throw std::runtime_error("MLDP queryDataBidiStream failed to send initial QuerySpec");
}

MldpBidiRecordBatchStream::~MldpBidiRecordBatchStream()
{
    if (!finished_)
    {
        rpc_context_->TryCancel();
        stream_->WritesDone();
        (void)stream_->Finish();
    }
}

std::shared_ptr<arrow::RecordBatch> MldpBidiRecordBatchStream::next()
{
    if (finished_)
        return nullptr;
    if (context_.cancellation)
        context_.cancellation->throwIfCancelled();
    if (request_next_)
    {
        if (context_.progress)
        {
            context_.progress->setActivity("mldp.time_series", "MLDP bidi cursor", "cursor next");
            context_.progress->cursorNext();
        }
        dp::service::query::QueryDataRequest request;
        request.mutable_cursorop()->set_cursoroperationtype(dp::service::query::QueryDataRequest::CursorOperation::CURSOR_OP_NEXT);
        if (!stream_->Write(request))
        {
            // A backend is allowed to close a shard immediately after its
            // final response.  In that case the next pull observes a
            // rejected CURSOR_OP_NEXT rather than a failed Read; finish
            // the RPC and treat an OK terminal status as clean EOF.
            stream_->WritesDone();
            const auto status = stream_->Finish();
            finished_ = true;
            if (context_.cancellation && context_.cancellation->cancelled())
                throw QueryCancelled{};
            if (!status.ok())
                throw std::runtime_error("MLDP queryDataBidiStream cursor-next failed (gRPC status " +
                                         std::to_string(static_cast<int>(status.error_code())) + "): " + status.error_message());
            return nullptr;
        }
    }
    dp::service::query::QueryDataResponse response;
    if (!stream_->Read(&response))
    {
        stream_->WritesDone();
        const auto status = stream_->Finish();
        finished_ = true;
        if (context_.cancellation && context_.cancellation->cancelled())
            throw QueryCancelled{};
        if (!status.ok())
            throw std::runtime_error("MLDP queryDataBidiStream read failed (gRPC status " +
                                     std::to_string(static_cast<int>(status.error_code())) + "): " + status.error_message());
        return nullptr;
    }
    request_next_ = true;
    if (context_.progress)
        context_.progress->setActivity("mldp.time_series", "MLDP bidi cursor", "cursor response");
    if (response.has_exceptionalresult())
        throw std::runtime_error("MLDP queryDataBidiStream failed: " + response.exceptionalresult().message());
    if (!response.has_querydata())
        throw std::runtime_error("MLDP queryDataBidiStream returned neither data nor an error");
    auto batch = makeBatch(response);
    if (context_.progress)
        context_.progress->cursorResponse(static_cast<uint64_t>(batch->num_rows()));
    return batch;
}

std::shared_ptr<arrow::RecordBatch> MldpBidiRecordBatchStream::makeBatch(const dp::service::query::QueryDataResponse& response) const
{
    struct Row
    {
        std::string                         pv;
        int64_t                             timestamp;
        dp::service::common::DataValue      value;
        dp::service::common::ColumnMetadata metadata;
    };

    std::vector<Row> rows;
    for (const auto& bucket : response.querydata().databuckets())
    {
        if (bucket.pvname().empty())
            throw std::runtime_error("MLDP queryDataBidiStream bucket has no PV name");
        const auto timestamps = bucketTimestamps(bucket);
        if (!bucket.has_datavalues())
            throw std::runtime_error("MLDP queryDataBidiStream bucket has no values");
        const auto&                                 values = bucket.datavalues();
        std::vector<dp::service::common::DataValue> decoded;
        dp::service::common::ColumnMetadata         metadata;
        if (values.has_datacolumn())
        {
            const auto& column = values.datacolumn();
            decoded.assign(column.datavalues().begin(), column.datavalues().end());
            metadata = column.metadata();
        }
        else
        {
            if (values.has_int64column())
            {
                decoded.reserve(static_cast<std::size_t>(values.int64column().values_size()));
                for (const auto value : values.int64column().values())
                    decoded.emplace_back().set_longvalue(value);
            }
            else if (values.has_int32column())
            {
                decoded.reserve(static_cast<std::size_t>(values.int32column().values_size()));
                for (const auto value : values.int32column().values())
                    decoded.emplace_back().set_intvalue(value);
            }
            else if (values.has_doublecolumn())
            {
                decoded.reserve(static_cast<std::size_t>(values.doublecolumn().values_size()));
                for (const auto value : values.doublecolumn().values())
                    decoded.emplace_back().set_doublevalue(value);
            }
            else if (values.has_floatcolumn())
            {
                decoded.reserve(static_cast<std::size_t>(values.floatcolumn().values_size()));
                for (const auto value : values.floatcolumn().values())
                    decoded.emplace_back().set_floatvalue(value);
            }
            else if (values.has_boolcolumn())
            {
                decoded.reserve(static_cast<std::size_t>(values.boolcolumn().values_size()));
                for (const auto value : values.boolcolumn().values())
                    decoded.emplace_back().set_booleanvalue(value);
            }
            else if (values.has_stringcolumn())
            {
                decoded.reserve(static_cast<std::size_t>(values.stringcolumn().values_size()));
                for (const auto& value : values.stringcolumn().values())
                    decoded.emplace_back().set_stringvalue(value);
            }
            else if (values.has_enumcolumn())
            {
                decoded.reserve(static_cast<std::size_t>(values.enumcolumn().values_size()));
                for (const auto value : values.enumcolumn().values())
                    decoded.emplace_back().set_intvalue(value);
            }
            else
            {
                throw std::runtime_error("MLDP queryDataBidiStream bucket has unsupported serialized values");
            }
        }
        if (timestamps.size() != decoded.size())
            throw std::runtime_error("MLDP queryDataBidiStream bucket timestamp/value cardinality mismatch for '" + bucket.pvname() + "'");
        if (!matchesColumnMetadataPredicates(metadata, dataValuesKind(decoded), column_predicates_))
            continue;
        for (std::size_t index = 0; index < decoded.size(); ++index)
            rows.push_back({bucket.pvname(), timestamps[index], std::move(decoded[index]), metadata});
    }
    auto*                                            pool = context_.pool != nullptr ? context_.pool : arrow::default_memory_pool();
    arrow::StringBuilder                             pv_builder(pool);
    arrow::TimestampBuilder                          time_builder(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), pool);
    DataValueBuilder                                 value_builder(pool);
    arrow::StringBuilder                             type_builder(pool);
    std::vector<dp::service::common::ColumnMetadata> metadata_values;
    metadata_values.reserve(rows.size());
    for (const auto& row : rows)
        metadata_values.push_back(row.metadata);
    auto attribute_keys_set = attributeKeys(metadata_values);
    auto provenance_keys_set = provenanceKeys(metadata_values);
    addRequestedDynamicMetadataKeys(attribute_keys_set, projection_hint_, "attributes.");
    addRequestedDynamicMetadataKeys(provenance_keys_set, projection_hint_, "provenance.");
    TimeSeriesMetadataBuilders metadata(attribute_keys_set, provenance_keys_set);
    for (const auto& row : rows)
    {
        if (!pv_builder.Append(row.pv).ok() || !time_builder.Append(row.timestamp).ok() || !type_builder.Append(dataValueKind(row.value)).ok())
            throw std::runtime_error("Failed to build Arrow queryDataBidiStream batch");
        value_builder.append(row.value);
        metadata.append(row.metadata);
    }
    std::shared_ptr<arrow::Array> pv, time, type;
    if (!pv_builder.Finish(&pv).ok() || !time_builder.Finish(&time).ok() || !type_builder.Finish(&type).ok())
        throw std::runtime_error("Failed to finish Arrow queryDataBidiStream batch");
    const auto                                 value = value_builder.finish();
    std::vector<std::shared_ptr<arrow::Field>> fields = {arrow::field("pv", pv->type()), arrow::field("time", time->type()), arrow::field("value", value->type()), arrow::field("column_type", type->type())};
    std::vector<std::shared_ptr<arrow::Array>> arrays = {pv, time, value, type};
    metadata.finish(fields, arrays);
    return arrow::RecordBatch::Make(arrow::schema(std::move(fields)), pv->length(), std::move(arrays));
}

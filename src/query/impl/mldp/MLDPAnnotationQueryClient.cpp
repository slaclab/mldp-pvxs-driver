//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/impl/mldp/MLDPAnnotationQueryClient.h>
#include <query/impl/mldp/AnnotationMetadataBuilders.h>
#include <query/impl/mldp/MldpTimestampUtils.h>

#include <query/ExecutionContext.h>
#include <query/QueryCancellation.h>
#include <query/SpillBackedStream.h>

#include <pool/MLDPGrpcAnnotationPoolConfig.h>
#include <util/log/Logger.h>

#include <annotation.grpc.pb.h>
#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_nested.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/record_batch.h>
#include <arrow/type.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <variant>

using namespace mldp_pvxs_driver::query::impl::mldp;
using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::util::log;
using mldp_pvxs_driver::util::pool::MLDPGrpcAnnotationPool;

const std::set<std::string_view> MLDPAnnotationQueryClient::kVirtualTables = {
    "mldp.pv_metadata",
    "mldp.configuration",
    "mldp.configuration_activation",
    "mldp.active_configurations",
};

std::set<std::string_view> MLDPAnnotationQueryClient::virtualTables() const
{
    return kVirtualTables;
}

std::vector<ColumnSchema> MLDPAnnotationQueryClient::tableSchema(std::string_view table_name) const
{
    const auto stringSearch = std::set<PredicateOp>{PredicateOp::EQ, PredicateOp::IN, PredicateOp::PREFIX, PredicateOp::CONTAINS};
    const auto stringFilter = std::set<PredicateOp>{PredicateOp::LIKE};
    if (table_name == "mldp.pv_metadata")
    {
        return {{"pv", ColumnType::STRING, false, true, stringSearch, stringFilter, "PV name; LIMIT is pushed to the annotation service when the query has no residual filter, join, sort or aggregate"},
                {"alias", ColumnType::STRING, false, true, stringSearch, stringFilter, "PV alias"},
                {"tags", ColumnType::STRING, false, true, {}, {}, "Complete tag collection; filter with tag = or tag IN (backend-pushed and locally verified)"},
                {"attributes", ColumnType::STRING, false, false, {}, {}, "Dynamic attribute map; select explicitly when needed; use attributes.<key> virtual columns for scalar results and filters; selecting it (or SELECT *) forces a full fetch because the key set is the union over every row"},
                {"tag", ColumnType::STRING, false, false, {PredicateOp::EQ, PredicateOp::IN}, {}, "Tag membership predicate shorthand for tags"},
                {"description", ColumnType::STRING, false, true, {}, stringFilter, "Description"},
                {"created_time", ColumnType::TIMESTAMP, false, true, {}, {}, "Created time"},
                {"updated_time", ColumnType::TIMESTAMP, false, true, {}, {}, "Updated time"},
                {"modified_by", ColumnType::STRING, false, true, {}, stringFilter, "Last modifier"}};
    }
    if (table_name == "mldp.configuration")
    {
        return {{"name", ColumnType::STRING, false, true, stringSearch, stringFilter, "Configuration name; LIMIT is pushed to the annotation service when the query has no residual filter, join, sort or aggregate"},
                {"category", ColumnType::STRING, false, true, {PredicateOp::EQ, PredicateOp::IN}, stringFilter, "Configuration category"},
                {"parent", ColumnType::STRING, false, true, {PredicateOp::EQ, PredicateOp::IN}, stringFilter, "Parent configuration"},
                {"tags", ColumnType::STRING, false, true, {}, {}, "Complete tag collection; filter with tag = or tag IN (backend-pushed and locally verified)"},
                {"attributes", ColumnType::STRING, false, false, {}, {}, "Dynamic attribute map; select explicitly when needed; use attributes.<key> virtual columns for scalar results and filters; selecting it (or SELECT *) forces a full fetch because the key set is the union over every row"},
                {"tag", ColumnType::STRING, false, false, {PredicateOp::EQ, PredicateOp::IN}, {}, "Tag membership predicate shorthand for tags"},
                {"description", ColumnType::STRING, false, true, {}, stringFilter, "Description"},
                {"created_time", ColumnType::TIMESTAMP, false, true, {}, {}, "Created time"},
                {"updated_time", ColumnType::TIMESTAMP, false, true, {}, {}, "Updated time"},
                {"modified_by", ColumnType::STRING, false, true, {}, stringFilter, "Last modifier"}};
    }
    if (table_name == "mldp.configuration_activation")
    {
        const auto timestampOps = std::set<PredicateOp>{PredicateOp::EQ, PredicateOp::NEQ, PredicateOp::LT, PredicateOp::LTE, PredicateOp::GT, PredicateOp::GTE};
        const auto nullableTimestampOps = std::set<PredicateOp>{PredicateOp::EQ, PredicateOp::NEQ, PredicateOp::LT, PredicateOp::LTE, PredicateOp::GT, PredicateOp::GTE, PredicateOp::IS_NULL, PredicateOp::IS_NOT_NULL};
        return {{"time", ColumnType::TIMESTAMP, false, true, timestampOps, timestampOps, "Activation start time; backend candidate set is locally verified, and that local verification suppresses LIMIT pushdown, so a filtered activation query still fetches every matching page"},
                {"end_time", ColumnType::TIMESTAMP, false, true, nullableTimestampOps, nullableTimestampOps, "Activation end time; null while open; backend candidate set is locally verified"},
                {"config_name", ColumnType::STRING, false, true, {PredicateOp::EQ, PredicateOp::IN}, stringFilter, "Configuration name"},
                {"activation_id", ColumnType::STRING, false, true, {PredicateOp::EQ, PredicateOp::IN}, stringFilter, "Activation identifier"},
                {"description", ColumnType::STRING, false, true, {}, stringFilter, "Description"},
                {"tags", ColumnType::STRING, false, true, {}, {}, "Complete tag collection; filter with tag = or tag IN (backend-pushed and locally verified)"},
                {"attributes", ColumnType::STRING, false, false, {}, {}, "Dynamic attribute map; select explicitly when needed; use attributes.<key> virtual columns for scalar results and filters; selecting it (or SELECT *) forces a full fetch because the key set is the union over every row"},
                {"tag", ColumnType::STRING, false, false, {PredicateOp::EQ, PredicateOp::IN}, {}, "Tag membership predicate shorthand for tags"},
                {"created_time", ColumnType::TIMESTAMP, false, true, {}, {}, "Created time"},
                {"updated_time", ColumnType::TIMESTAMP, false, true, {}, {}, "Updated time"}};
    }
    if (table_name == "mldp.active_configurations")
    {
        return {{"at", ColumnType::TIMESTAMP, true, false, {PredicateOp::EQ}, {}, "Requested point in time; the backend RPC is unpaginated, so LIMIT is always applied locally"},
                {"name", ColumnType::STRING, false, true, {}, stringFilter, "Active configuration name"},
                {"activation_id", ColumnType::STRING, false, true, {}, stringFilter, "Activation identifier"},
                {"time", ColumnType::TIMESTAMP, false, true, {}, {}, "Activation start time"}};
    }
    throw std::invalid_argument("MLDPAnnotationQueryClient: unknown virtual table: " + std::string(table_name));
}

namespace {

std::vector<std::string> stringValues(const Predicate& predicate)
{
    std::vector<std::string> values;
    for (const auto& value : predicate.values)
    {
        if (!std::holds_alternative<std::string>(value))
            throw std::invalid_argument("Annotation string predicate requires string values");
        values.push_back(std::get<std::string>(value));
    }
    if (values.empty())
        throw std::invalid_argument("Annotation predicate must include at least one value");
    return values;
}

int64_t timestampValue(const Predicate& predicate)
{
    if (predicate.values.size() != 1 || !std::holds_alternative<int64_t>(predicate.values.front()))
        throw std::invalid_argument("Annotation timestamp predicate requires one integer timestamp");
    return std::get<int64_t>(predicate.values.front());
}

bool addActivationTimeRangeCriterion(
    dp::service::annotation::QueryConfigurationActivationsRequest& request,
    const std::vector<Predicate>& predicates,
    const bool                    always)
{
    constexpr int64_t kMaximumTimestampSeconds = 253'402'300'799LL;
    // The backend treats a zero start time as unset and matches nothing, so
    // the open lower bound starts one second after the epoch.
    int64_t lower = 1;
    int64_t upper = kMaximumTimestampSeconds;
    bool    has_time_range = false;
    for (const auto& predicate : predicates)
    {
        if (predicate.column != "time" ||
            (predicate.op != PredicateOp::GTE && predicate.op != PredicateOp::GT &&
             predicate.op != PredicateOp::LTE && predicate.op != PredicateOp::LT))
            continue;
        has_time_range = true;
        const auto value = timestampValue(predicate);
        if (predicate.op == PredicateOp::GTE || predicate.op == PredicateOp::GT)
            lower = std::max(lower, value);
        else if (value > 0)
            upper = std::min(upper, predicate.op == PredicateOp::LTE ? value + 1 : value);
    }
    // Without a time predicate the full range is an always-true overlap
    // criterion, used when nothing else was pushed (browse-all).
    if (!has_time_range && !always)
    {
        return false;
    }
    if (upper <= lower)
    {
        upper = lower == kMaximumTimestampSeconds ? kMaximumTimestampSeconds : lower + 1;
    }
    auto* range = request.add_criteria()->mutable_timerangecriterion();
    setTimestamp(range->mutable_starttime(), lower);
    setTimestamp(range->mutable_endtime(), upper);
    return true;
}

void appendTimestamp(arrow::TimestampBuilder& builder, const dp::service::common::Timestamp& timestamp)
{
    if (!builder.Append(timestampToNanoseconds(timestamp)).ok())
        throw std::runtime_error("Failed to build Arrow annotation timestamp column");
}

void appendString(arrow::StringBuilder& builder, std::string_view value)
{
    if (!builder.Append(value).ok())
        throw std::runtime_error("Failed to build Arrow annotation string column");
}

template <typename Metadata>
std::set<std::string> dynamicAttributeKeys(const std::vector<Metadata>& records)
{
    std::set<std::string> keys;
    for (const auto& record : records)
    {
        for (const auto& attribute : record.attributes())
        {
            keys.insert(attribute.name());
        }
    }
    return keys;
}

std::set<std::string> requestedDynamicAttributeKeys(const std::set<std::string>& projection_hint)
{
    std::set<std::string> keys;
    constexpr std::string_view prefix = "attributes.";
    for (const auto& column : projection_hint)
    {
        if (column.rfind(prefix, 0) == 0 && column.size() > prefix.size())
            keys.insert(column.substr(prefix.size()));
    }
    return keys;
}

template <typename Metadata>
std::set<std::string> metadataAttributeKeys(const std::vector<Metadata>& records,
                                             const std::set<std::string>& projection_hint)
{
    auto keys = dynamicAttributeKeys(records);
    const auto requested = requestedDynamicAttributeKeys(projection_hint);
    keys.insert(requested.begin(), requested.end());
    return keys;
}

/** @brief Builds one Arrow record batch from a set of PV metadata records. */
std::shared_ptr<arrow::RecordBatch> buildPvMetadataBatch(const std::vector<dp::service::common::PvMetadata>& records,
                                                         const std::set<std::string>&                       attribute_keys,
                                                         const bool                                         include_attributes)
{
    arrow::StringBuilder    pv, alias, description, modified_by;
    arrow::TimestampBuilder created(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
    arrow::TimestampBuilder updated(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
    AnnotationMetadataBuilders metadata(attribute_keys);
    for (const auto& record : records)
    {
        appendString(pv, record.pvname());
        if (record.aliases_size() > 0)
            appendString(alias, record.aliases(0));
        else
        {
            (void)alias.AppendNull();
        }
        appendString(description, record.description());
        appendString(modified_by, record.modifiedby());
        appendTimestamp(created, record.createdtime());
        appendTimestamp(updated, record.updatedtime());
        metadata.append(record);
    }
    std::shared_ptr<arrow::Array> a, b, c, d, e, f;
    if (!pv.Finish(&a).ok() || !alias.Finish(&b).ok() || !description.Finish(&c).ok() || !modified_by.Finish(&d).ok() ||
        !created.Finish(&e).ok() || !updated.Finish(&f).ok())
        throw std::runtime_error("Failed to finish Arrow pv_metadata batch");
    std::vector<std::shared_ptr<arrow::Field>> fields = {arrow::field("pv", a->type()), arrow::field("alias", b->type()), arrow::field("description", c->type()), arrow::field("modified_by", d->type()), arrow::field("created_time", e->type()), arrow::field("updated_time", f->type())};
    std::vector<std::shared_ptr<arrow::Array>> arrays = {a, b, c, d, e, f};
    metadata.finish(fields, arrays, include_attributes);
    return arrow::RecordBatch::Make(arrow::schema(std::move(fields)), a->length(), std::move(arrays));
}

/** @brief Builds one Arrow record batch from a set of configuration records. */
std::shared_ptr<arrow::RecordBatch> buildConfigurationBatch(const std::vector<dp::service::common::Configuration>& records,
                                                            const std::set<std::string>&                          attribute_keys,
                                                            const bool                                            include_attributes)
{
    arrow::StringBuilder    name, category, parent, description, modified_by;
    arrow::TimestampBuilder created(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool()), updated(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
    AnnotationMetadataBuilders metadata(attribute_keys);
    for (const auto& record : records)
    {
        appendString(name, record.configurationname());
        appendString(category, record.category());
        appendString(parent, record.parentconfigurationname());
        appendString(description, record.description());
        appendString(modified_by, record.modifiedby());
        appendTimestamp(created, record.createdtime());
        appendTimestamp(updated, record.updatedtime());
        metadata.append(record);
    }
    std::shared_ptr<arrow::Array> a, b, c, d, e, f, g;
    if (!name.Finish(&a).ok() || !category.Finish(&b).ok() || !parent.Finish(&c).ok() ||
        !description.Finish(&d).ok() || !modified_by.Finish(&e).ok() ||
        !created.Finish(&f).ok() || !updated.Finish(&g).ok())
        throw std::runtime_error("Failed to finish Arrow configuration batch");
    std::vector<std::shared_ptr<arrow::Field>> fields = {arrow::field("name", a->type()), arrow::field("category", b->type()), arrow::field("parent", c->type()), arrow::field("description", d->type()), arrow::field("modified_by", e->type()), arrow::field("created_time", f->type()), arrow::field("updated_time", g->type())};
    std::vector<std::shared_ptr<arrow::Array>> arrays = {a, b, c, d, e, f, g};
    metadata.finish(fields, arrays, include_attributes);
    return arrow::RecordBatch::Make(arrow::schema(std::move(fields)), a->length(), std::move(arrays));
}

/** @brief Builds one Arrow record batch from a set of configuration activation records. */
std::shared_ptr<arrow::RecordBatch> buildConfigurationActivationBatch(
    const std::vector<dp::service::common::ConfigurationActivation>& records,
    const std::set<std::string>&                                     attribute_keys,
    const bool                                                       include_attributes)
{
    arrow::TimestampBuilder time(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
    arrow::TimestampBuilder end_time(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
    arrow::StringBuilder    config, id, description;
    AnnotationMetadataBuilders metadata(attribute_keys);
    for (const auto& record : records)
    {
        appendTimestamp(time, record.starttime());
        if (record.has_endtime())
            appendTimestamp(end_time, record.endtime());
        else if (!end_time.AppendNull().ok())
            throw std::runtime_error("Failed to append null Arrow configuration activation end time");
        appendString(config, record.configurationname());
        appendString(id, record.clientactivationid());
        appendString(description, record.description());
        metadata.append(record);
    }
    std::shared_ptr<arrow::Array> a, b, c, d, e;
    if (!time.Finish(&a).ok() || !end_time.Finish(&b).ok() || !config.Finish(&c).ok() || !id.Finish(&d).ok() || !description.Finish(&e).ok())
        throw std::runtime_error("Failed to finish Arrow configuration_activation batch");
    std::vector<std::shared_ptr<arrow::Field>> fields = {arrow::field("time", a->type()), arrow::field("end_time", b->type(), true), arrow::field("config_name", c->type()), arrow::field("activation_id", d->type()), arrow::field("description", e->type())};
    std::vector<std::shared_ptr<arrow::Array>> arrays = {a, b, c, d, e};
    metadata.finish(fields, arrays, include_attributes);
    return arrow::RecordBatch::Make(arrow::schema(std::move(fields)), a->length(), std::move(arrays));
}

/** @brief Computes the per-request page limit honouring a remaining row budget.
 *  @param[in] page_size Configured page size; 0 defers to the server default.
 *  @param[in] remaining Rows still needed, or 0 when the budget is unlimited.
 *  @return Value to place in the request's limit field.
 */
uint32_t pageRequestLimit(const uint32_t page_size, const uint64_t remaining)
{
    if (remaining == 0)
    {
        return page_size;
    }
    const uint64_t candidate = page_size != 0 ? std::min<uint64_t>(page_size, remaining) : remaining;
    return static_cast<uint32_t>(std::min<uint64_t>(candidate, std::numeric_limits<uint32_t>::max()));
}

/** @brief Fetches every page of an annotation query, stopping early once @p max_rows are collected.
 *  @param[in] max_rows Row budget pushed down from SQL LIMIT; 0 means unlimited.
 */
template <typename Record, typename Request, typename Query>
std::vector<Record> queryAllPages(Request                                   request,
                                  Query&&                                   query,
                                  const std::shared_ptr<QueryCancellation>& cancellation,
                                  const uint32_t                            page_size,
                                  const uint64_t                            max_rows)
{
    std::vector<Record> records;
    while (true)
    {
        if (cancellation) cancellation->throwIfCancelled();
        request.set_limit(pageRequestLimit(page_size, max_rows != 0 ? max_rows - records.size() : 0));
        auto [page, next_page_token] = query(request);
        if (cancellation) cancellation->throwIfCancelled();
        records.insert(records.end(),
                       std::make_move_iterator(page.begin()),
                       std::make_move_iterator(page.end()));
        if (max_rows != 0 && records.size() >= max_rows)
        {
            records.resize(max_rows);
            return records;
        }
        if (next_page_token.empty())
        {
            return records;
        }
        request.set_pagetoken(std::move(next_page_token));
    }
}

/** @brief Pull stream that issues one annotation query RPC page per next() call.
 *
 *  Usable only when the dynamic @c attributes.<key> column set is known up front, because
 *  every emitted batch must share one Arrow schema.
 */
template <typename Record, typename Request, typename Query, typename Build>
class AnnotationPagedRecordBatchStream : public IRecordBatchStream
{
public:
    AnnotationPagedRecordBatchStream(Request                            request,
                                     Query                              query,
                                     Build                              build,
                                     std::shared_ptr<QueryCancellation> cancellation,
                                     const uint32_t                     page_size,
                                     const uint64_t                     max_rows)
        : request_(std::move(request)), query_(std::move(query)), build_(std::move(build)),
          cancellation_(std::move(cancellation)), page_size_(page_size), max_rows_(max_rows)
    {
    }

    std::shared_ptr<arrow::RecordBatch> next() override
    {
        while (!done_)
        {
            if (cancellation_) cancellation_->throwIfCancelled();
            if (max_rows_ != 0 && emitted_ >= max_rows_)
            {
                done_ = true;
                break;
            }
            request_.set_limit(pageRequestLimit(page_size_, max_rows_ != 0 ? max_rows_ - emitted_ : 0));
            auto [page, next_page_token] = query_(request_);
            if (cancellation_) cancellation_->throwIfCancelled();
            if (next_page_token.empty())
                done_ = true;
            else
                request_.set_pagetoken(std::move(next_page_token));
            if (max_rows_ != 0 && emitted_ + page.size() > max_rows_)
                page.resize(max_rows_ - emitted_);
            if (page.empty())
                continue;
            emitted_ += page.size();
            return build_(page);
        }
        return nullptr;
    }

private:
    Request                            request_;
    Query                              query_;
    Build                              build_;
    std::shared_ptr<QueryCancellation> cancellation_;
    uint32_t                           page_size_{0};
    uint64_t                           max_rows_{0};
    uint64_t                           emitted_{0};
    bool                               done_{false};
};

/** @brief Runs one annotation virtual-table scan, streaming per page when the schema allows it.
 *
 *  @p build receives a page of records plus the dynamic attribute key set and returns one batch.
 *  Streaming requires a stable key set, which only holds for an explicit select list that does
 *  not project the whole @c attributes map; otherwise the key set is the union over every record
 *  and the result must be materialized.
 */
template <typename Record, typename Request, typename Query, typename Build>
IRecordBatchStreamUPtr runAnnotationScan(Request                      request,
                                         Query                        query,
                                         Build                        build,
                                         const std::set<std::string>& projection_hint,
                                         const ExecutionContext&      context,
                                         const uint32_t               page_size)
{
    if (context.scan_projection_explicit && !projection_hint.contains("attributes"))
    {
        auto keys = requestedDynamicAttributeKeys(projection_hint);
        auto page_build = [build, keys = std::move(keys)](const std::vector<Record>& page)
        {
            return build(page, keys);
        };
        return std::make_unique<AnnotationPagedRecordBatchStream<Record, Request, Query, decltype(page_build)>>(
            std::move(request), std::move(query), std::move(page_build), context.cancellation, page_size,
            context.scan_row_limit);
    }
    const auto records = queryAllPages<Record>(std::move(request), query, context.cancellation, page_size,
                                               context.scan_row_limit);
    return materializedStream({build(records, metadataAttributeKeys(records, projection_hint))});
}

std::shared_ptr<mldp_pvxs_driver::util::log::ILogger> makeAnnotationQueryClientLogger()
{
    std::string name = "mldp_annotation_query_client";
    return mldp_pvxs_driver::util::log::newLogger(name);
}

} // namespace

IRecordBatchStreamUPtr MLDPAnnotationQueryClient::executeStream(std::string_view              table_name,
                                                               const std::vector<Predicate>& predicates,
                                                               const std::set<std::string>&  projection_hint,
                                                               const ExecutionContext&       context)
{
    const auto limit = context.join_batch_size;
    if (table_name == "mldp.pv_metadata")
    {
        dp::service::annotation::QueryPvMetadataRequest request;
        for (const auto& predicate : predicates)
        {
            auto*      criterion = request.add_criteria();
            const auto values = stringValues(predicate);
            if (predicate.column == "pv")
            {
                if (predicate.op != PredicateOp::EQ && predicate.op != PredicateOp::IN && predicate.op != PredicateOp::PREFIX && predicate.op != PredicateOp::CONTAINS)
                    throw std::invalid_argument("Unsupported mldp.pv_metadata predicate operator for pv");
                auto* target = criterion->mutable_pvnamecriterion();
                for (const auto& value : values)
                    (predicate.op == PredicateOp::EQ || predicate.op == PredicateOp::IN ? target->add_exact() : predicate.op == PredicateOp::PREFIX ? target->add_prefix()
                                                                                                                                                    : target->add_contains())
                        ->assign(value);
            }
            else if (predicate.column == "alias")
            {
                if (predicate.op != PredicateOp::EQ && predicate.op != PredicateOp::IN && predicate.op != PredicateOp::PREFIX && predicate.op != PredicateOp::CONTAINS)
                    throw std::invalid_argument("Unsupported mldp.pv_metadata predicate operator for alias");
                auto* target = criterion->mutable_aliasescriterion();
                for (const auto& value : values)
                    (predicate.op == PredicateOp::EQ || predicate.op == PredicateOp::IN ? target->add_exact() : predicate.op == PredicateOp::PREFIX ? target->add_prefix()
                                                                                                                                                    : target->add_contains())
                        ->assign(value);
            }
            else if (predicate.column == "tag" && (predicate.op == PredicateOp::EQ || predicate.op == PredicateOp::IN))
            {
                for (const auto& value : values)
                    criterion->mutable_tagscriterion()->add_values(value);
            }
            else if (predicate.column.rfind("attributes.", 0) == 0 && (predicate.op == PredicateOp::EQ || predicate.op == PredicateOp::IN))
            {
                auto* target = criterion->mutable_attributescriterion();
                target->set_key(predicate.column.substr(std::string("attributes.").size()));
                for (const auto& value : values)
                    target->add_values(value);
            }
            else
                throw std::invalid_argument("Unsupported mldp.pv_metadata predicate column or operator: " + predicate.column);
        }
        // Empty criteria does not reliably match all records against the
        // live annotation service despite 1.16.0 docs; keep sending an
        // always-true PV-name prefix criterion when nothing else was pushed.
        if (request.criteria_size() == 0)
            request.add_criteria()->mutable_pvnamecriterion()->add_prefix("");
        return runAnnotationScan<dp::service::common::PvMetadata>(
            std::move(request),
            [this, cancellation = context.cancellation](const auto& page_request)
            {
                return queryPvMetadata(page_request, cancellation);
            },
            [include_attributes = projection_hint.contains("attributes")](
                const std::vector<dp::service::common::PvMetadata>& records,
                const std::set<std::string>&                        attribute_keys)
            {
                return buildPvMetadataBatch(records, attribute_keys, include_attributes);
            },
            projection_hint, context, limit);
    }
    if (table_name == "mldp.configuration")
    {
        dp::service::annotation::QueryConfigurationsRequest request;
        for (const auto& predicate : predicates)
        {
            const auto values = stringValues(predicate);
            auto*      criterion = request.add_criteria();
            if (predicate.column == "name")
            {
                if (predicate.op != PredicateOp::EQ && predicate.op != PredicateOp::IN && predicate.op != PredicateOp::PREFIX && predicate.op != PredicateOp::CONTAINS)
                    throw std::invalid_argument("Unsupported mldp.configuration predicate operator for name");
                auto* target = criterion->mutable_namecriterion();
                for (const auto& value : values)
                    (predicate.op == PredicateOp::EQ || predicate.op == PredicateOp::IN ? target->add_exact() : predicate.op == PredicateOp::PREFIX ? target->add_prefix()
                                                                                                                                                    : target->add_contains())
                        ->assign(value);
            }
            else if (predicate.column == "category" && (predicate.op == PredicateOp::EQ || predicate.op == PredicateOp::IN))
                for (const auto& value : values)
                    criterion->mutable_categorycriterion()->add_values(value);
            else if (predicate.column == "parent" && (predicate.op == PredicateOp::EQ || predicate.op == PredicateOp::IN))
                for (const auto& value : values)
                    criterion->mutable_parentcriterion()->add_values(value);
            else if (predicate.column == "tag" && (predicate.op == PredicateOp::EQ || predicate.op == PredicateOp::IN))
                for (const auto& value : values)
                    criterion->mutable_tagscriterion()->add_values(value);
            else if (predicate.column.rfind("attributes.", 0) == 0 && (predicate.op == PredicateOp::EQ || predicate.op == PredicateOp::IN))
            {
                auto* target = criterion->mutable_attributescriterion();
                target->set_key(predicate.column.substr(std::string("attributes.").size()));
                for (const auto& value : values)
                    target->add_values(value);
            }
            else
                throw std::invalid_argument("Unsupported mldp.configuration predicate column or operator: " + predicate.column);
        }
        // Empty criteria does not reliably match all records against the
        // live annotation service despite 1.16.0 docs; keep sending an
        // always-true name-prefix criterion when nothing else was pushed.
        if (request.criteria_size() == 0)
            request.add_criteria()->mutable_namecriterion()->add_prefix("");
        return runAnnotationScan<dp::service::common::Configuration>(
            std::move(request),
            [this, cancellation = context.cancellation](const auto& page_request)
            {
                return queryConfigurations(page_request, cancellation);
            },
            [include_attributes = projection_hint.contains("attributes")](
                const std::vector<dp::service::common::Configuration>& records,
                const std::set<std::string>&                           attribute_keys)
            {
                return buildConfigurationBatch(records, attribute_keys, include_attributes);
            },
            projection_hint, context, limit);
    }
    if (table_name == "mldp.configuration_activation")
    {
        dp::service::annotation::QueryConfigurationActivationsRequest request;
        for (const auto& predicate : predicates)
        {
            if (predicate.column == "time" || predicate.column == "end_time")
            {
                if (predicate.op != PredicateOp::IS_NULL && predicate.op != PredicateOp::IS_NOT_NULL)
                    (void)timestampValue(predicate);
                continue;
            }
            auto* criterion = request.add_criteria();
            if (predicate.column == "config_name" && (predicate.op == PredicateOp::EQ || predicate.op == PredicateOp::IN))
                for (const auto& value : stringValues(predicate))
                    criterion->mutable_configurationnamecriterion()->add_values(value);
            else if (predicate.column == "activation_id" && (predicate.op == PredicateOp::EQ || predicate.op == PredicateOp::IN))
                for (const auto& value : stringValues(predicate))
                    criterion->mutable_clientactivationidcriterion()->add_values(value);
            else if (predicate.column == "tag" && (predicate.op == PredicateOp::EQ || predicate.op == PredicateOp::IN))
                for (const auto& value : stringValues(predicate))
                    criterion->mutable_tagscriterion()->add_values(value);
            else if (predicate.column.rfind("attributes.", 0) == 0 && (predicate.op == PredicateOp::EQ || predicate.op == PredicateOp::IN))
            {
                auto* target = criterion->mutable_attributescriterion();
                target->set_key(predicate.column.substr(std::string("attributes.").size()));
                for (const auto& value : stringValues(predicate))
                    target->add_values(value);
            }
            else
                throw std::invalid_argument("Unsupported mldp.configuration_activation predicate column or operator: " + predicate.column);
        }
        // The annotation API range has overlap semantics, so it is only a
        // candidate-set optimization. Exact endpoint semantics remain local.
        // With no other criterion the full range is sent so an unfiltered
        // scan returns every activation.
        addActivationTimeRangeCriterion(request, predicates, request.criteria_size() == 0);
        return runAnnotationScan<dp::service::common::ConfigurationActivation>(
            std::move(request),
            [this, cancellation = context.cancellation](const auto& page_request)
            {
                return queryConfigurationActivations(page_request, cancellation);
            },
            [include_attributes = projection_hint.contains("attributes")](
                const std::vector<dp::service::common::ConfigurationActivation>& records,
                const std::set<std::string>&                                     attribute_keys)
            {
                return buildConfigurationActivationBatch(records, attribute_keys, include_attributes);
            },
            projection_hint, context, limit);
    }
    if (table_name == "mldp.active_configurations")
    {
        const Predicate* at = nullptr;
        for (const auto& predicate : predicates)
        {
            if (predicate.column == "at" && predicate.op == PredicateOp::EQ)
            {
                if (at != nullptr)
                    throw std::invalid_argument("mldp.active_configurations requires exactly one at = predicate");
                at = &predicate;
            }
            else
                throw std::invalid_argument("mldp.active_configurations only supports at =");
        }
        if (at == nullptr)
            throw std::invalid_argument("mldp.active_configurations requires exactly one at = predicate");
        dp::service::common::Timestamp timestamp;
        setTimestamp(&timestamp, timestampValue(*at));
        const auto              records = getActiveConfigurations(timestamp, context.cancellation);
        arrow::StringBuilder    name, id;
        arrow::TimestampBuilder time(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
        for (const auto& record : records)
        {
            appendString(name, record.configurationname());
            appendString(id, record.clientactivationid());
            appendTimestamp(time, record.starttime());
        }
        std::shared_ptr<arrow::Array> a, b, c;
        if (!name.Finish(&a).ok() || !id.Finish(&b).ok() || !time.Finish(&c).ok())
            throw std::runtime_error("Failed to finish Arrow active_configurations batch");
        auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("name", a->type()), arrow::field("activation_id", b->type()), arrow::field("time", c->type())}), a->length(), {a, b, c});
        return materializedStream({std::move(batch)});
    }
    throw std::invalid_argument("MLDPAnnotationQueryClient: unknown virtual table '" + std::string(table_name) + "'; supported tables: mldp.pv_metadata, mldp.configuration, mldp.configuration_activation, mldp.active_configurations");
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

MLDPAnnotationQueryClient::MLDPAnnotationQueryClient(
    const util::pool::MLDPGrpcPoolConfig& poolConfig,
    std::shared_ptr<metrics::Metrics>     metrics)
    : logger_(makeAnnotationQueryClientLogger())
    , pool_(MLDPGrpcAnnotationPool::create(poolConfig, std::move(metrics)))
{
}

MLDPAnnotationQueryClient::MLDPAnnotationQueryClient(
    const util::pool::MLDPGrpcAnnotationPoolConfig& poolConfig,
    std::shared_ptr<metrics::Metrics>               metrics)
    : logger_(makeAnnotationQueryClientLogger())
    , pool_(MLDPGrpcAnnotationPool::create(poolConfig, std::move(metrics)))
{
}

MLDPAnnotationQueryClient::MLDPAnnotationQueryClient(
    const config::Config&             cfg,
    std::shared_ptr<metrics::Metrics> m)
    : MLDPAnnotationQueryClient(util::pool::MLDPGrpcAnnotationPoolConfig(cfg), std::move(m))
{
}

// ---------------------------------------------------------------------------
// getPvMetadata
// ---------------------------------------------------------------------------

std::optional<dp::service::common::PvMetadata>
MLDPAnnotationQueryClient::getPvMetadata(const std::string& pvNameOrAlias)
{
    try
    {
        auto                                          handle = pool_->acquire();
        grpc::ClientContext                           ctx;
        dp::service::annotation::GetPvMetadataRequest req;
        req.set_pvnameoralias(pvNameOrAlias);
        dp::service::annotation::GetPvMetadataResponse resp;
        const auto                                     status = handle->stub->getPvMetadata(&ctx, req, &resp);
        if (!status.ok())
        {
            errorf(*logger_, "getPvMetadata failed: {}", status.error_message());
            return std::nullopt;
        }
        if (!resp.has_getpvmetadataresult())
            return std::nullopt;
        const auto& result = resp.getpvmetadataresult();
        if (!result.has_pvmetadata())
            return std::nullopt;
        return result.pvmetadata();
    }
    catch (const std::exception& ex)
    {
        errorf(*logger_, "getPvMetadata exception: {}", ex.what());
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// queryPvMetadata
// ---------------------------------------------------------------------------

std::pair<std::vector<dp::service::common::PvMetadata>, std::string>
MLDPAnnotationQueryClient::queryPvMetadata(
    const dp::service::annotation::QueryPvMetadataRequest& request,
    std::shared_ptr<QueryCancellation> cancellation)
{
    try
    {
        auto                                             handle = pool_->acquire();
        auto                                             ctx = std::make_shared<grpc::ClientContext>();
        dp::service::annotation::QueryPvMetadataResponse resp;
        auto registration = cancellation ? cancellation->onCancel([ctx] { ctx->TryCancel(); }) : QueryCancellation::Registration{};
        const auto                                       status = handle->stub->queryPvMetadata(ctx.get(), request, &resp);
        if (cancellation && cancellation->cancelled()) throw QueryCancelled{};
        if (!status.ok())
        {
            errorf(*logger_, "queryPvMetadata failed: {}", status.error_message());
            return {{}, {}};
        }
        if (!resp.has_pvmetadataresult())
            return {{}, {}};
        const auto&                                  result = resp.pvmetadataresult();
        std::vector<dp::service::common::PvMetadata> records;
        records.reserve(static_cast<std::size_t>(result.pvmetadata_size()));
        for (const auto& m : result.pvmetadata())
            records.push_back(m);
        return {std::move(records), result.nextpagetoken()};
    }
    catch (const std::exception& ex)
    {
        if (cancellation && cancellation->cancelled()) throw;
        errorf(*logger_, "queryPvMetadata exception: {}", ex.what());
        return {{}, {}};
    }
}

// ---------------------------------------------------------------------------
// getConfiguration
// ---------------------------------------------------------------------------

std::optional<dp::service::common::Configuration>
MLDPAnnotationQueryClient::getConfiguration(const std::string& configurationName)
{
    try
    {
        auto                                             handle = pool_->acquire();
        grpc::ClientContext                              ctx;
        dp::service::annotation::GetConfigurationRequest req;
        req.set_configurationname(configurationName);
        dp::service::annotation::GetConfigurationResponse resp;
        const auto                                        status = handle->stub->getConfiguration(&ctx, req, &resp);
        if (!status.ok())
        {
            errorf(*logger_, "getConfiguration failed: {}", status.error_message());
            return std::nullopt;
        }
        if (!resp.has_getconfigurationresult())
            return std::nullopt;
        const auto& result = resp.getconfigurationresult();
        if (!result.has_configuration())
            return std::nullopt;
        return result.configuration();
    }
    catch (const std::exception& ex)
    {
        errorf(*logger_, "getConfiguration exception: {}", ex.what());
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// queryConfigurations
// ---------------------------------------------------------------------------

std::pair<std::vector<dp::service::common::Configuration>, std::string>
MLDPAnnotationQueryClient::queryConfigurations(
    const dp::service::annotation::QueryConfigurationsRequest& request,
    std::shared_ptr<QueryCancellation> cancellation)
{
    try
    {
        auto                                                 handle = pool_->acquire();
        auto                                                 ctx = std::make_shared<grpc::ClientContext>();
        dp::service::annotation::QueryConfigurationsResponse resp;
        auto registration = cancellation ? cancellation->onCancel([ctx] { ctx->TryCancel(); }) : QueryCancellation::Registration{};
        const auto                                           status = handle->stub->queryConfigurations(ctx.get(), request, &resp);
        if (cancellation && cancellation->cancelled()) throw QueryCancelled{};
        if (!status.ok())
        {
            errorf(*logger_, "queryConfigurations failed: {}", status.error_message());
            return {{}, {}};
        }
        if (!resp.has_queryconfigurationsresult())
            return {{}, {}};
        const auto&                                     result = resp.queryconfigurationsresult();
        std::vector<dp::service::common::Configuration> records;
        records.reserve(static_cast<std::size_t>(result.configurations_size()));
        for (const auto& c : result.configurations())
            records.push_back(c);
        return {std::move(records), result.nextpagetoken()};
    }
    catch (const std::exception& ex)
    {
        if (cancellation && cancellation->cancelled()) throw;
        errorf(*logger_, "queryConfigurations exception: {}", ex.what());
        return {{}, {}};
    }
}

// ---------------------------------------------------------------------------
// getConfigurationActivation
// ---------------------------------------------------------------------------

std::optional<dp::service::common::ConfigurationActivation>
MLDPAnnotationQueryClient::getConfigurationActivation(
    const dp::service::annotation::GetConfigurationActivationRequest& request)
{
    try
    {
        auto                                                        handle = pool_->acquire();
        grpc::ClientContext                                         ctx;
        dp::service::annotation::GetConfigurationActivationResponse resp;
        const auto                                                  status = handle->stub->getConfigurationActivation(&ctx, request, &resp);
        if (!status.ok())
        {
            errorf(*logger_, "getConfigurationActivation failed: {}", status.error_message());
            return std::nullopt;
        }
        if (!resp.has_getconfigurationactivationresult())
            return std::nullopt;
        const auto& result = resp.getconfigurationactivationresult();
        if (!result.has_configurationactivation())
            return std::nullopt;
        return result.configurationactivation();
    }
    catch (const std::exception& ex)
    {
        errorf(*logger_, "getConfigurationActivation exception: {}", ex.what());
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// queryConfigurationActivations
// ---------------------------------------------------------------------------

std::pair<std::vector<dp::service::common::ConfigurationActivation>, std::string>
MLDPAnnotationQueryClient::queryConfigurationActivations(
    const dp::service::annotation::QueryConfigurationActivationsRequest& request,
    std::shared_ptr<QueryCancellation> cancellation)
{
    try
    {
        auto                                                           handle = pool_->acquire();
        auto                                                           ctx = std::make_shared<grpc::ClientContext>();
        dp::service::annotation::QueryConfigurationActivationsResponse resp;
        auto registration = cancellation ? cancellation->onCancel([ctx] { ctx->TryCancel(); }) : QueryCancellation::Registration{};
        const auto                                                     status =
            handle->stub->queryConfigurationActivations(ctx.get(), request, &resp);
        if (cancellation && cancellation->cancelled()) throw QueryCancelled{};
        if (!status.ok())
        {
            errorf(*logger_, "queryConfigurationActivations failed: {}", status.error_message());
            return {{}, {}};
        }
        if (!resp.has_queryconfigurationactivationsresult())
            return {{}, {}};
        const auto&                                               result = resp.queryconfigurationactivationsresult();
        std::vector<dp::service::common::ConfigurationActivation> records;
        records.reserve(static_cast<std::size_t>(result.configurationactivations_size()));
        for (const auto& ca : result.configurationactivations())
            records.push_back(ca);
        return {std::move(records), result.nextpagetoken()};
    }
    catch (const std::exception& ex)
    {
        if (cancellation && cancellation->cancelled()) throw;
        errorf(*logger_, "queryConfigurationActivations exception: {}", ex.what());
        return {{}, {}};
    }
}

// ---------------------------------------------------------------------------
// getActiveConfigurations
// ---------------------------------------------------------------------------

std::vector<dp::service::common::ConfigurationActivation>
MLDPAnnotationQueryClient::getActiveConfigurations(
    const dp::service::common::Timestamp& at,
    std::shared_ptr<QueryCancellation> cancellation)
{
    try
    {
        auto                                                    handle = pool_->acquire();
        auto                                                    ctx = std::make_shared<grpc::ClientContext>();
        dp::service::annotation::GetActiveConfigurationsRequest req;
        *req.mutable_timestamp() = at;
        dp::service::annotation::GetActiveConfigurationsResponse resp;
        auto registration = cancellation ? cancellation->onCancel([ctx] { ctx->TryCancel(); }) : QueryCancellation::Registration{};
        const auto                                               status = handle->stub->getActiveConfigurations(ctx.get(), req, &resp);
        if (cancellation && cancellation->cancelled()) throw QueryCancelled{};
        if (!status.ok())
        {
            errorf(*logger_, "getActiveConfigurations failed: {}", status.error_message());
            return {};
        }
        if (!resp.has_getactiveconfigurationsresult())
            return {};
        const auto&                                               result = resp.getactiveconfigurationsresult();
        std::vector<dp::service::common::ConfigurationActivation> records;
        records.reserve(
            static_cast<std::size_t>(result.configurationactivations_size()));
        for (const auto& ca : result.configurationactivations())
            records.push_back(ca);
        return records;
    }
    catch (const std::exception& ex)
    {
        if (cancellation && cancellation->cancelled()) throw;
        errorf(*logger_, "getActiveConfigurations exception: {}", ex.what());
        return {};
    }
}

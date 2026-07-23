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

#include <query/ExecutionContext.h>
#include <query/QueryResult.h>

#include <pool/MLDPGrpcAnnotationPoolConfig.h>
#include <util/log/Logger.h>

#include <annotation.grpc.pb.h>
#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/record_batch.h>
#include <arrow/type.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <stdexcept>
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
    if (table_name == "mldp.pv_metadata")
    {
        return {{"pv", ColumnType::STRING, false, true, stringSearch, {}, "PV name"},
                {"alias", ColumnType::STRING, false, true, stringSearch, {}, "PV alias"},
                {"tag", ColumnType::STRING, false, true, {PredicateOp::EQ, PredicateOp::IN}, {}, "Metadata tag"},
                {"description", ColumnType::STRING, false, true, {}, {}, "Description"},
                {"created_time", ColumnType::TIMESTAMP, false, true, {}, {}, "Created time"},
                {"updated_time", ColumnType::TIMESTAMP, false, true, {}, {}, "Updated time"},
                {"modified_by", ColumnType::STRING, false, true, {}, {}, "Last modifier"}};
    }
    if (table_name == "mldp.configuration")
    {
        return {{"name", ColumnType::STRING, false, true, stringSearch, {}, "Configuration name"},
                {"category", ColumnType::STRING, false, true, {PredicateOp::EQ, PredicateOp::IN}, {}, "Configuration category"},
                {"parent", ColumnType::STRING, false, true, {PredicateOp::EQ, PredicateOp::IN}, {}, "Parent configuration"},
                {"description", ColumnType::STRING, false, true, {}, {}, "Description"},
                {"created_time", ColumnType::TIMESTAMP, false, true, {}, {}, "Created time"},
                {"updated_time", ColumnType::TIMESTAMP, false, true, {}, {}, "Updated time"},
                {"modified_by", ColumnType::STRING, false, true, {}, {}, "Last modifier"}};
    }
    if (table_name == "mldp.configuration_activation")
    {
        return {{"time", ColumnType::TIMESTAMP, false, true, {PredicateOp::EQ, PredicateOp::GTE, PredicateOp::LTE}, {}, "Activation time"},
                {"config_name", ColumnType::STRING, false, true, {PredicateOp::EQ, PredicateOp::IN}, {}, "Configuration name"},
                {"activation_id", ColumnType::STRING, false, true, {PredicateOp::EQ, PredicateOp::IN}, {}, "Activation identifier"},
                {"description", ColumnType::STRING, false, true, {}, {}, "Description"},
                {"created_time", ColumnType::TIMESTAMP, false, true, {}, {}, "Created time"},
                {"updated_time", ColumnType::TIMESTAMP, false, true, {}, {}, "Updated time"}};
    }
    if (table_name == "mldp.active_configurations")
    {
        return {{"at", ColumnType::TIMESTAMP, true, false, {PredicateOp::EQ}, {}, "Requested point in time"},
                {"name", ColumnType::STRING, false, true, {}, {}, "Active configuration name"},
                {"activation_id", ColumnType::STRING, false, true, {}, {}, "Activation identifier"},
                {"time", ColumnType::TIMESTAMP, false, true, {}, {}, "Activation start time"}};
    }
    throw std::invalid_argument("MLDPAnnotationQueryClient: unknown virtual table: " + std::string(table_name));
}

namespace {

int64_t timestampToNanoseconds(const dp::service::common::Timestamp& timestamp)
{
    return static_cast<int64_t>(timestamp.epochseconds()) * 1'000'000'000LL + static_cast<int64_t>(timestamp.nanoseconds());
}

void setTimestamp(dp::service::common::Timestamp* target, int64_t seconds)
{
    target->set_epochseconds(static_cast<uint64_t>(seconds));
    target->set_nanoseconds(0);
}

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

void appendTimestamp(arrow::TimestampBuilder& builder, const dp::service::common::Timestamp& timestamp)
{
    if (!builder.Append(timestampToNanoseconds(timestamp)).ok())
        throw std::runtime_error("Failed to build Arrow annotation timestamp column");
}

std::shared_ptr<mldp_pvxs_driver::util::log::ILogger> makeAnnotationQueryClientLogger()
{
    std::string name = "mldp_annotation_query_client";
    return mldp_pvxs_driver::util::log::newLogger(name);
}

} // namespace

QueryResult MLDPAnnotationQueryClient::execute(std::string_view              table_name,
                                               const std::vector<Predicate>& predicates,
                                               const std::set<std::string>&,
                                               const ExecutionContext& context,
                                               std::string_view        page_token)
{
    const auto limit = context.join_batch_size;
    if (table_name == "mldp.pv_metadata")
    {
        dp::service::annotation::QueryPvMetadataRequest request;
        request.set_limit(limit);
        request.set_pagetoken(std::string(page_token));
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
            else
                throw std::invalid_argument("Unsupported mldp.pv_metadata predicate column or operator: " + predicate.column);
        }
        if (request.criteria_size() == 0)
            throw std::invalid_argument("mldp.pv_metadata requires at least one pushable predicate");
        const auto [records, next] = queryPvMetadata(request);
        arrow::StringBuilder    pv, alias, tag, description, modified_by;
        arrow::TimestampBuilder created(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
        arrow::TimestampBuilder updated(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
        for (const auto& record : records)
        {
            const auto aliases = std::max(1, record.aliases_size());
            const auto tags = std::max(1, record.tags_size());
            for (int a = 0; a < aliases; ++a)
                for (int t = 0; t < tags; ++t)
                {
                    pv.Append(record.pvname());
                    a < record.aliases_size() ? alias.Append(record.aliases(a)) : alias.AppendNull();
                    t < record.tags_size() ? tag.Append(record.tags(t)) : tag.AppendNull();
                    description.Append(record.description());
                    modified_by.Append(record.modifiedby());
                    appendTimestamp(created, record.createdtime());
                    appendTimestamp(updated, record.updatedtime());
                }
        }
        std::shared_ptr<arrow::Array> a, b, c, d, e, f, g;
        pv.Finish(&a);
        alias.Finish(&b);
        tag.Finish(&c);
        description.Finish(&d);
        modified_by.Finish(&e);
        created.Finish(&f);
        updated.Finish(&g);
        return {.batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", a->type()), arrow::field("alias", b->type()), arrow::field("tag", c->type()), arrow::field("description", d->type()), arrow::field("modified_by", e->type()), arrow::field("created_time", f->type()), arrow::field("updated_time", g->type())}), a->length(), {a, b, c, d, e, f, g}), .next_page_token = next};
    }
    if (table_name == "mldp.configuration")
    {
        dp::service::annotation::QueryConfigurationsRequest request;
        request.set_limit(limit);
        request.set_pagetoken(std::string(page_token));
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
            else
                throw std::invalid_argument("Unsupported mldp.configuration predicate column or operator: " + predicate.column);
        }
        if (request.criteria_size() == 0)
            throw std::invalid_argument("mldp.configuration requires at least one pushable predicate");
        const auto [records, next] = queryConfigurations(request);
        arrow::StringBuilder    name, category, parent, description, modified_by;
        arrow::TimestampBuilder created(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool()), updated(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
        for (const auto& record : records)
        {
            name.Append(record.configurationname());
            category.Append(record.category());
            parent.Append(record.parentconfigurationname());
            description.Append(record.description());
            modified_by.Append(record.modifiedby());
            appendTimestamp(created, record.createdtime());
            appendTimestamp(updated, record.updatedtime());
        }
        std::shared_ptr<arrow::Array> a, b, c, d, e, f, g;
        name.Finish(&a);
        category.Finish(&b);
        parent.Finish(&c);
        description.Finish(&d);
        modified_by.Finish(&e);
        created.Finish(&f);
        updated.Finish(&g);
        return {.batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("name", a->type()), arrow::field("category", b->type()), arrow::field("parent", c->type()), arrow::field("description", d->type()), arrow::field("modified_by", e->type()), arrow::field("created_time", f->type()), arrow::field("updated_time", g->type())}), a->length(), {a, b, c, d, e, f, g}), .next_page_token = next};
    }
    if (table_name == "mldp.configuration_activation")
    {
        dp::service::annotation::QueryConfigurationActivationsRequest request;
        request.set_limit(limit);
        request.set_pagetoken(std::string(page_token));
        for (const auto& predicate : predicates)
        {
            auto* criterion = request.add_criteria();
            if (predicate.column == "time" && predicate.op == PredicateOp::EQ)
                setTimestamp(criterion->mutable_timestampcriterion()->mutable_timestamp(), timestampValue(predicate));
            else if (predicate.column == "config_name" && (predicate.op == PredicateOp::EQ || predicate.op == PredicateOp::IN))
                for (const auto& value : stringValues(predicate))
                    criterion->mutable_configurationnamecriterion()->add_values(value);
            else if (predicate.column == "activation_id" && (predicate.op == PredicateOp::EQ || predicate.op == PredicateOp::IN))
                for (const auto& value : stringValues(predicate))
                    criterion->mutable_clientactivationidcriterion()->add_values(value);
            else
                throw std::invalid_argument("Unsupported mldp.configuration_activation predicate column or operator: " + predicate.column);
        }
        if (request.criteria_size() == 0)
            throw std::invalid_argument("mldp.configuration_activation requires at least one pushable predicate");
        const auto [records, next] = queryConfigurationActivations(request);
        arrow::TimestampBuilder time(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
        arrow::StringBuilder    config, id, description;
        for (const auto& record : records)
        {
            appendTimestamp(time, record.starttime());
            config.Append(record.configurationname());
            id.Append(record.clientactivationid());
            description.Append(record.description());
        }
        std::shared_ptr<arrow::Array> a, b, c, d;
        time.Finish(&a);
        config.Finish(&b);
        id.Finish(&c);
        description.Finish(&d);
        return {.batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("time", a->type()), arrow::field("config_name", b->type()), arrow::field("activation_id", c->type()), arrow::field("description", d->type())}), a->length(), {a, b, c, d}), .next_page_token = next};
    }
    if (table_name == "mldp.active_configurations")
    {
        if (!page_token.empty())
            throw std::invalid_argument("mldp.active_configurations does not support continuation tokens");
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
        const auto              records = getActiveConfigurations(timestamp);
        arrow::StringBuilder    name, id;
        arrow::TimestampBuilder time(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
        for (const auto& record : records)
        {
            name.Append(record.configurationname());
            id.Append(record.clientactivationid());
            appendTimestamp(time, record.starttime());
        }
        std::shared_ptr<arrow::Array> a, b, c;
        name.Finish(&a);
        id.Finish(&b);
        time.Finish(&c);
        return {.batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("name", a->type()), arrow::field("activation_id", b->type()), arrow::field("time", c->type())}), a->length(), {a, b, c}), .next_page_token = {}};
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
    : MLDPAnnotationQueryClient(util::pool::MLDPGrpcPoolConfig(cfg), std::move(m))
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
    const dp::service::annotation::QueryPvMetadataRequest& request)
{
    try
    {
        auto                                             handle = pool_->acquire();
        grpc::ClientContext                              ctx;
        dp::service::annotation::QueryPvMetadataResponse resp;
        const auto                                       status = handle->stub->queryPvMetadata(&ctx, request, &resp);
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
    const dp::service::annotation::QueryConfigurationsRequest& request)
{
    try
    {
        auto                                                 handle = pool_->acquire();
        grpc::ClientContext                                  ctx;
        dp::service::annotation::QueryConfigurationsResponse resp;
        const auto                                           status = handle->stub->queryConfigurations(&ctx, request, &resp);
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
    const dp::service::annotation::QueryConfigurationActivationsRequest& request)
{
    try
    {
        auto                                                           handle = pool_->acquire();
        grpc::ClientContext                                            ctx;
        dp::service::annotation::QueryConfigurationActivationsResponse resp;
        const auto                                                     status =
            handle->stub->queryConfigurationActivations(&ctx, request, &resp);
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
        errorf(*logger_, "queryConfigurationActivations exception: {}", ex.what());
        return {{}, {}};
    }
}

// ---------------------------------------------------------------------------
// getActiveConfigurations
// ---------------------------------------------------------------------------

std::vector<dp::service::common::ConfigurationActivation>
MLDPAnnotationQueryClient::getActiveConfigurations(
    const dp::service::common::Timestamp& at)
{
    try
    {
        auto                                                    handle = pool_->acquire();
        grpc::ClientContext                                     ctx;
        dp::service::annotation::GetActiveConfigurationsRequest req;
        *req.mutable_timestamp() = at;
        dp::service::annotation::GetActiveConfigurationsResponse resp;
        const auto                                               status = handle->stub->getActiveConfigurations(&ctx, req, &resp);
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
        errorf(*logger_, "getActiveConfigurations exception: {}", ex.what());
        return {};
    }
}

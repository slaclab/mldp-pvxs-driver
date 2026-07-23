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
#include <grpcpp/grpcpp.h>

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
                {"tag", ColumnType::STRING, false, true, {PredicateOp::EQ, PredicateOp::IN}, {}, "Metadata tag"}};
    }
    if (table_name == "mldp.configuration")
    {
        return {{"name", ColumnType::STRING, false, true, stringSearch, {}, "Configuration name"},
                {"category", ColumnType::STRING, false, true, {PredicateOp::EQ, PredicateOp::IN}, {}, "Configuration category"},
                {"parent", ColumnType::STRING, false, true, {PredicateOp::EQ, PredicateOp::IN}, {}, "Parent configuration"}};
    }
    if (table_name == "mldp.configuration_activation")
    {
        return {{"time", ColumnType::TIMESTAMP, false, true, {PredicateOp::EQ, PredicateOp::GTE, PredicateOp::LTE}, {}, "Activation time"},
                {"config_name", ColumnType::STRING, false, true, {PredicateOp::EQ, PredicateOp::IN}, {}, "Configuration name"},
                {"activation_id", ColumnType::STRING, false, true, {PredicateOp::EQ, PredicateOp::IN}, {}, "Activation identifier"}};
    }
    if (table_name == "mldp.active_configurations")
    {
        return {{"at", ColumnType::TIMESTAMP, true, false, {PredicateOp::EQ}, {}, "Requested point in time"},
                {"name", ColumnType::STRING, false, true, {}, {}, "Active configuration name"}};
    }
    throw std::invalid_argument("MLDPAnnotationQueryClient: unknown virtual table: " + std::string(table_name));
}

QueryResult MLDPAnnotationQueryClient::execute(std::string_view,
                                               const std::vector<Predicate>&,
                                               const std::set<std::string>&,
                                               const ExecutionContext&)
{
    throw std::logic_error("MLDP annotation query execution is not implemented yet (Phase 4)");
}

namespace {

std::shared_ptr<mldp_pvxs_driver::util::log::ILogger> makeAnnotationQueryClientLogger()
{
    std::string name = "mldp_annotation_query_client";
    return mldp_pvxs_driver::util::log::newLogger(name);
}

} // namespace

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

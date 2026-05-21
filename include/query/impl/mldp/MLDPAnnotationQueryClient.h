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

#include <annotation.grpc.pb.h>
#include <config/Config.h>
#include <metrics/Metrics.h>
#include <pool/MLDPGrpcAnnotationPool.h>
#include <pool/MLDPGrpcPoolConfig.h>
#include <query/IQueryable.h>
#include <util/log/Logger.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mldp_pvxs_driver::query::impl::mldp {

/**
 * @brief Annotation service query client.
 *
 * Wraps MLDPGrpcAnnotationPool and exposes all DpAnnotationService RPC
 * methods as plain synchronous calls.  Registered in the controller
 * factory dispatch map under the "mldp-annotation" type key.
 *
 * Typical use — integration tests and diagnostic tools:
 * @code
 * MLDPAnnotationQueryClient client(pool_config);
 * auto meta = client.getPvMetadata("MY:PV");
 * @endcode
 */
class MLDPAnnotationQueryClient : public IQueryable
{
public:
    /**
     * @brief Construct and immediately initialise the underlying annotation pool.
     *
     * @param poolConfig  Connection parameters; annotation-url must be set.
     * @param metrics     Optional shared metrics collector.
     */
    explicit MLDPAnnotationQueryClient(
        const util::pool::MLDPGrpcPoolConfig& poolConfig,
        std::shared_ptr<metrics::Metrics>     metrics = nullptr);

    /**
     * @brief Construct from a Config tree; delegates to the pool-config ctor.
     *
     * Required by QueryableFactory::prepare<MLDPAnnotationQueryClient>(cfg).
     *
     * @param cfg     Root config containing pool parameters (annotation-url, etc.).
     * @param metrics Optional shared metrics collector.
     */
    explicit MLDPAnnotationQueryClient(
        const config::Config&             cfg,
        std::shared_ptr<metrics::Metrics> metrics = nullptr);

    ~MLDPAnnotationQueryClient() override = default;

    // Non-copyable, movable
    MLDPAnnotationQueryClient(const MLDPAnnotationQueryClient&)            = delete;
    MLDPAnnotationQueryClient& operator=(const MLDPAnnotationQueryClient&) = delete;
    MLDPAnnotationQueryClient(MLDPAnnotationQueryClient&&)                 = default;
    MLDPAnnotationQueryClient& operator=(MLDPAnnotationQueryClient&&)      = default;

    // -----------------------------------------------------------------------
    // PV metadata
    // -----------------------------------------------------------------------

    /**
     * @brief Retrieve metadata for a single PV by name or alias.
     *
     * @param pvNameOrAlias  PV name or alias string.
     * @return Matching PvMetadata, or std::nullopt on error / not found.
     */
    std::optional<dp::service::common::PvMetadata>
    getPvMetadata(const std::string& pvNameOrAlias);

    /**
     * @brief Query PV metadata with a full QueryPvMetadataRequest.
     *
     * @param request  Pre-built query request.
     * @return Pair of matching PvMetadata records and a next-page token
     *         (empty string when there are no more pages).
     */
    std::pair<std::vector<dp::service::common::PvMetadata>, std::string>
    queryPvMetadata(const dp::service::annotation::QueryPvMetadataRequest& request);

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------

    /**
     * @brief Retrieve a single configuration by name.
     *
     * @param configurationName  Configuration name.
     * @return Matching Configuration, or std::nullopt on error / not found.
     */
    std::optional<dp::service::common::Configuration>
    getConfiguration(const std::string& configurationName);

    /**
     * @brief Query configurations with a full QueryConfigurationsRequest.
     *
     * @param request  Pre-built query request.
     * @return Pair of matching Configuration records and a next-page token.
     */
    std::pair<std::vector<dp::service::common::Configuration>, std::string>
    queryConfigurations(const dp::service::annotation::QueryConfigurationsRequest& request);

    // -----------------------------------------------------------------------
    // Configuration activations
    // -----------------------------------------------------------------------

    /**
     * @brief Retrieve a single configuration activation.
     *
     * @param request  Pre-built get request (identifies activation by composite
     *                 key or client-activation-id).
     * @return Matching ConfigurationActivation, or std::nullopt on error / not found.
     */
    std::optional<dp::service::common::ConfigurationActivation>
    getConfigurationActivation(
        const dp::service::annotation::GetConfigurationActivationRequest& request);

    /**
     * @brief Query configuration activations with a full request.
     *
     * @param request  Pre-built query request.
     * @return Pair of matching ConfigurationActivation records and a next-page token.
     */
    std::pair<std::vector<dp::service::common::ConfigurationActivation>, std::string>
    queryConfigurationActivations(
        const dp::service::annotation::QueryConfigurationActivationsRequest& request);

    /**
     * @brief Retrieve all active configurations at a given point in time.
     *
     * @param at  Timestamp (epoch seconds / nanoseconds) for the query.
     * @return Vector of active ConfigurationActivation records (empty on error).
     */
    std::vector<dp::service::common::ConfigurationActivation>
    getActiveConfigurations(const dp::service::common::Timestamp& at);

private:
    std::shared_ptr<util::log::ILogger>                                   logger_;
    util::pool::MLDPGrpcAnnotationPool::MLDPGrpcAnnotationPoolShrdPtr     pool_;
};

} // namespace mldp_pvxs_driver::query::impl::mldp

#pragma once

#include <config/Config.h>

#include <stdexcept>
#include <string>

namespace mldp_pvxs_driver::util::pool {

/**
 * @brief Typed view of the MLDP gRPC pool configuration.
 *
 * The driver expects the YAML block describing the pool to contain:
 * @code{.yaml}
 * mldp_pool:
 *   provider_name: pvxs_provider
 *   url: https://mldp.example:443
 *   min_conn: 1
 *   max_conn: 4
 * @endcode
 *
 * This helper validates the schema and exposes strongly typed accessors.
 */
class MLDPGrpcPoolConfig
{
public:
    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    MLDPGrpcPoolConfig();
    explicit MLDPGrpcPoolConfig(const config::Config& root);

    bool               valid() const;
    const std::string& providerName() const;
    const std::string& url() const;
    int                minConnections() const;
    int                maxConnections() const;

private:
    void parse(const config::Config& root);

    bool        valid_ = false; ///< Tracks whether parsing succeeded.
    std::string provider_name_;
    std::string url_;
    int         min_conn_ = 0;
    int         max_conn_ = 0;
};

} // namespace mldp_pvxs_driver::util::pool

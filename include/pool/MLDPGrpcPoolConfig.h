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

#include <config/Config.h>

#include <grpcpp/grpcpp.h>
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
 *   provider_description: "PVXS-based data provider"
 *   url: https://mldp.example:443
 *   min_conn: 1
 *   max_conn: 4
 *   credentials: ssl  # or 'none' for insecure, or a map for custom TLS
 * @endcode
 *
 * For custom TLS configuration, credentials can be a map with file paths:
 * @code{.yaml}
 * mldp_pool:
 *   provider_name: pvxs_provider
 *   provider_description: "PVXS-based data provider"
 *   url: https://mldp.example:443
 *   min_conn: 1
 *   max_conn: 4
 *   credentials:
 *     pem_cert_chain: /etc/certs/client.crt   # optional; file contents are loaded
 *     pem_private_key: /etc/certs/client.key  # optional
 *     pem_root_certs: /etc/certs/ca.crt       # optional
 * @endcode
 *
 * This helper validates the schema and exposes strongly typed accessors.
 * File paths provided in the credentials map are read during parsing, and
 * their contents are stored in the SSL options structure.
 */
class MLDPGrpcPoolConfig
{
public:
    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    /**
     * @brief Credential configuration for gRPC connections.
     *
     * Credentials can be:
     * - Insecure: no TLS (for development/testing)
     * - SSL: with optional custom certificate paths
     *
     * When type is SSL and ssl_options contains file paths, the file
     * contents are loaded during configuration parsing.
     */
    struct Credentials
    {
        enum class Type
        {
            Insecure,  ///< No TLS encryption
            Ssl        ///< TLS with optional custom certificates
        };

        Type                           type{Type::Insecure};
        grpc::SslCredentialsOptions    ssl_options{};  ///< Populated from file paths if provided
    };

    MLDPGrpcPoolConfig();
    explicit MLDPGrpcPoolConfig(const config::Config& root);

    bool               valid() const;
    const std::string& providerName() const;
    const std::string& providerDescription() const;
    const std::string& url() const;
    int                minConnections() const;
    int                maxConnections() const;
    const Credentials& credentials() const;

private:
    void parse(const config::Config& root);
    static std::string readFile(const std::string& path);

    bool        valid_ = false; ///< Tracks whether parsing succeeded.
    std::string provider_name_;
    std::string provider_description_;
    std::string url_;
    int         min_conn_ = 0;
    int         max_conn_ = 0;
    Credentials credentials_;
};

} // namespace mldp_pvxs_driver::util::pool

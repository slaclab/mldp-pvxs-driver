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

inline constexpr char ProviderNameKey[]       = "provider-name";
inline constexpr char ProviderDescriptionKey[] = "provider-description";
inline constexpr char IngestionUrlKey[]        = "ingestion-url";
inline constexpr char QueryUrlKey[]            = "query-url";
inline constexpr char AnnotationUrlKey[]       = "annotation-url";
inline constexpr char MinConnKey[]             = "min-conn";
inline constexpr char MaxConnKey[]             = "max-conn";
inline constexpr char CredentialsKey[]         = "credentials";
inline constexpr char PemCertChainKey[]        = "pem-cert-chain";
inline constexpr char PemPrivateKeyKey[]       = "pem-private-key";
inline constexpr char PemRootCertsKey[]        = "pem-root-certs";

/**
 * @brief Typed view of the MLDP gRPC pool configuration (ingestion + query + annotation).
 *
 * Requires provider-name, ingestion-url, min-conn, max-conn.
 * query-url is optional (not used by the ingestion writer).
 * annotation-url and credentials are optional.
 *
 * Subclasses (e.g. MLDPGrpcAnnotationPoolConfig) may call the protected
 * default constructor and use the protected setters to populate only the
 * fields they need.
 */
class MLDPGrpcPoolConfig
{
public:
    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    struct Credentials
    {
        enum class Type { Insecure, Ssl };
        Type                        type{Type::Insecure};
        grpc::SslCredentialsOptions ssl_options{};
    };

    /** Default ctor — produces an invalid (valid_==false) config. */
    MLDPGrpcPoolConfig() = default;

    /** Full constructor — parses all required fields from YAML. */
    explicit MLDPGrpcPoolConfig(const config::Config& root);

    bool               valid()               const;
    const std::string& providerName()        const;
    const std::string& providerDescription() const;
    const std::string& ingestionUrl()        const;
    const std::string& queryUrl()            const;
    const std::string& annotationUrl()       const;
    int                minConnections()      const;
    int                maxConnections()      const;
    const Credentials& credentials()         const;

protected:
    /** Protected setters — used by subclasses that populate only a subset of fields. */
    void setValid(bool v)                { valid_           = v; }
    void setAnnotationUrl(std::string u) { annotation_url_  = std::move(u); }
    void setMinConnections(int v)        { min_conn_        = v; }
    void setMaxConnections(int v)        { max_conn_        = v; }
    void setCredentials(Credentials c)   { credentials_     = std::move(c); }

private:
    void               parse(const config::Config& root);
    static std::string readFile(const std::string& path);

    bool        valid_{false};
    std::string provider_name_;
    std::string provider_description_;
    std::string ingestion_url_;
    std::string query_url_;
    std::string annotation_url_;
    int         min_conn_{0};
    int         max_conn_{0};
    Credentials credentials_;
};

} // namespace mldp_pvxs_driver::util::pool

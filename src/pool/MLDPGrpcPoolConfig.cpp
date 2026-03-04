//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <pool/MLDPGrpcPoolConfig.h>

#include <fstream>
#include <sstream>

using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::util::pool;

namespace {
std::string pickKey(const Config& cfg, const std::string& dashKey, const std::string& underscoreKey)
{
    if (cfg.hasChild(dashKey))
    {
        return dashKey;
    }
    return underscoreKey;
}
} // namespace

MLDPGrpcPoolConfig::MLDPGrpcPoolConfig() = default;

MLDPGrpcPoolConfig::MLDPGrpcPoolConfig(const config::Config& root)
{
    parse(root);
}

bool MLDPGrpcPoolConfig::valid() const
{
    return valid_;
}

const std::string& MLDPGrpcPoolConfig::providerName() const
{
    return provider_name_;
}

const std::string& MLDPGrpcPoolConfig::ingestionUrl() const
{
    return ingestion_url_;
}

const std::string& MLDPGrpcPoolConfig::queryUrl() const
{
    return query_url_;
}

const std::string& MLDPGrpcPoolConfig::providerDescription() const
{
    return provider_description_;
}

int MLDPGrpcPoolConfig::minConnections() const
{
    return min_conn_;
}

int MLDPGrpcPoolConfig::maxConnections() const
{
    return max_conn_;
}

const MLDPGrpcPoolConfig::Credentials& MLDPGrpcPoolConfig::credentials() const
{
    return credentials_;
}

void MLDPGrpcPoolConfig::parse(const config::Config& root)
{
    if (!root.valid())
    {
        throw Error("mldp_pool configuration node is invalid");
    }
    const auto providerNameKey = pickKey(root, "provider-name", "provider_name");
    if (!root.hasChild(providerNameKey))
    {
        throw Error(makeMissingFieldMessage(providerNameKey));
    }
    provider_name_ = root.get(providerNameKey);
    if (provider_name_.empty())
    {
        throw Error("mldp_pool." + providerNameKey + " must not be empty");
    }
    const auto providerDescriptionKey = pickKey(root, "provider-description", "provider_description");
    if (root.hasChild(providerDescriptionKey))
    {
        provider_description_ = root.get(providerDescriptionKey);
    }
    else
    {
        provider_description_ = provider_name_;
    }

    const auto ingestionUrlKey = root.hasChild("ingestion-url")      ? "ingestion-url"
                                 : root.hasChild("ingestion_url")    ? "ingestion_url"
                                 : root.hasChild("url")              ? "url"
                                                                      : "ingestion-url";
    if (!root.hasChild(ingestionUrlKey))
    {
        throw Error(makeMissingFieldMessage(ingestionUrlKey));
    }
    ingestion_url_ = root.get(ingestionUrlKey);
    if (ingestion_url_.empty())
    {
        throw Error("mldp_pool." + ingestionUrlKey + " must not be empty");
    }
    query_url_ = ingestion_url_;
    const auto queryUrlKey = pickKey(root, "query-url", "query_url");
    if (root.hasChild(queryUrlKey))
    {
        query_url_ = root.get(queryUrlKey);
        if (query_url_.empty())
        {
            throw Error("mldp_pool." + queryUrlKey + " must not be empty when provided");
        }
    }

    const auto minConnKey = pickKey(root, "min-conn", "min_conn");
    if (!root.hasChild(minConnKey))
    {
        throw Error(makeMissingFieldMessage(minConnKey));
    }
    min_conn_ = root.getInt(minConnKey);
    if (min_conn_ <= 0)
    {
        throw Error("mldp_pool." + minConnKey + " must be greater than zero");
    }

    const auto maxConnKey = pickKey(root, "max-conn", "max_conn");
    if (!root.hasChild(maxConnKey))
    {
        throw Error(makeMissingFieldMessage(maxConnKey));
    }
    max_conn_ = root.getInt(maxConnKey);
    if (max_conn_ <= 0)
    {
        throw Error("mldp_pool." + maxConnKey + " must be greater than zero");
    }
    if (max_conn_ < min_conn_)
    {
        throw Error("mldp_pool." + maxConnKey + " must be greater than or equal to " + minConnKey);
    }

    credentials_ = Credentials{};
    if (root.hasChild("credentials"))
    {
        const auto credentialNodes = root.subConfig("credentials");
        if (credentialNodes.empty())
        {
            throw Error("mldp_pool.credentials is present but empty");
        }

        const auto& credentialsNode = credentialNodes.front();
        const auto  credentialsTree = credentialsNode.raw();
        if (!credentialsTree.is_map())
        {
            std::string credentialsType;
            credentialsNode >> credentialsType;
            if (credentialsType == "ssl")
            {
                credentials_.type = Credentials::Type::Ssl;
            }
            else if (credentialsType == "none" || credentialsType.empty())
            {
                credentials_.type = Credentials::Type::Insecure;
            }
            else
            {
                throw Error("mldp_pool.credentials must be 'none', 'ssl', or a map of TLS options");
            }
        }
        else
        {
            credentials_.type = Credentials::Type::Ssl;

            const char* const certChainKey = credentialsTree.has_child("pem-cert-chain") ? "pem-cert-chain"
                                            : "pem_cert_chain";
            const char* const privateKeyKey = credentialsTree.has_child("pem-private-key") ? "pem-private-key"
                                             : "pem_private_key";
            const char* const rootCertsKey = credentialsTree.has_child("pem-root-certs") ? "pem-root-certs"
                                            : "pem_root_certs";

            if (credentialsTree.has_child(certChainKey))
            {
                std::string path;
                credentialsTree[certChainKey] >> path;
                credentials_.ssl_options.pem_cert_chain = readFile(path);
            }
            if (credentialsTree.has_child(privateKeyKey))
            {
                std::string path;
                credentialsTree[privateKeyKey] >> path;
                credentials_.ssl_options.pem_private_key = readFile(path);
            }
            if (credentialsTree.has_child(rootCertsKey))
            {
                std::string path;
                credentialsTree[rootCertsKey] >> path;
                credentials_.ssl_options.pem_root_certs = readFile(path);
            }
        }
    }

    valid_ = true;
}

std::string MLDPGrpcPoolConfig::readFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        std::ostringstream oss;
        oss << "Failed to read credentials file at '" << path << "'";
        throw Error(oss.str());
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    
    if (file.bad())
    {
        std::ostringstream oss;
        oss << "Error reading credentials file at '" << path << "'";
        throw Error(oss.str());
    }
    
    return buffer.str();
}

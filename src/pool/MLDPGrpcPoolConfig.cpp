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
    using namespace mldp_pvxs_driver::util::pool;

    if (!root.valid())
    {
        throw Error("mldp-pool configuration node is invalid");
    }
    if (!root.hasChild(ProviderNameKey))
    {
        throw Error(makeMissingFieldMessage(ProviderNameKey));
    }
    provider_name_ = root.get(ProviderNameKey);
    if (provider_name_.empty())
    {
        throw Error(std::string("mldp-pool.") + ProviderNameKey + " must not be empty");
    }
    if (root.hasChild(ProviderDescriptionKey))
    {
        provider_description_ = root.get(ProviderDescriptionKey);
    }
    else
    {
        provider_description_ = provider_name_;
    }

    if (!root.hasChild(IngestionUrlKey))
    {
        throw Error(makeMissingFieldMessage(IngestionUrlKey));
    }
    ingestion_url_ = root.get(IngestionUrlKey);
    if (ingestion_url_.empty())
    {
        throw Error(std::string("mldp-pool.") + IngestionUrlKey + " must not be empty");
    }
    if (!root.hasChild(QueryUrlKey))
    {
        throw Error(makeMissingFieldMessage(QueryUrlKey));
    }
    query_url_ = root.get(QueryUrlKey);
    if (query_url_.empty())
    {
        throw Error(std::string("mldp-pool.") + QueryUrlKey + " must not be empty");
    }
    if (query_url_ == ingestion_url_)
    {
        throw Error(std::string("mldp-pool.") + QueryUrlKey + " must not be equal to ingestion-url");
    }

    if (!root.hasChild(MinConnKey))
    {
        throw Error(makeMissingFieldMessage(MinConnKey));
    }
    min_conn_ = root.getInt(MinConnKey);
    if (min_conn_ <= 0)
    {
        throw Error(std::string("mldp-pool.") + MinConnKey + " must be greater than zero");
    }

    if (!root.hasChild(MaxConnKey))
    {
        throw Error(makeMissingFieldMessage(MaxConnKey));
    }
    max_conn_ = root.getInt(MaxConnKey);
    if (max_conn_ <= 0)
    {
        throw Error(std::string("mldp-pool.") + MaxConnKey + " must be greater than zero");
    }
    if (max_conn_ < min_conn_)
    {
        throw Error(std::string("mldp-pool.") + MaxConnKey + " must be greater than or equal to " + MinConnKey);
    }

    credentials_ = Credentials{};
    if (root.hasChild(CredentialsKey))
    {
        const auto credentialNodes = root.subConfig(CredentialsKey);
        if (credentialNodes.empty())
        {
            throw Error(std::string("mldp-pool.") + CredentialsKey + " is present but empty");
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
                throw Error("mldp-pool.credentials must be 'none', 'ssl', or a map of TLS options");
            }
        }
        else
        {
            credentials_.type = Credentials::Type::Ssl;

            if (credentialsTree.has_child(PemCertChainKey))
            {
                std::string path;
                credentialsTree[PemCertChainKey] >> path;
                credentials_.ssl_options.pem_cert_chain = readFile(path);
            }
            if (credentialsTree.has_child(PemPrivateKeyKey))
            {
                std::string path;
                credentialsTree[PemPrivateKeyKey] >> path;
                credentials_.ssl_options.pem_private_key = readFile(path);
            }
            if (credentialsTree.has_child(PemRootCertsKey))
            {
                std::string path;
                credentialsTree[PemRootCertsKey] >> path;
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

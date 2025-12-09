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

const std::string& MLDPGrpcPoolConfig::url() const
{
    return url_;
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
    if (!root.hasChild("provider_name"))
    {
        throw Error(makeMissingFieldMessage("provider_name"));
    }
    provider_name_ = root.get("provider_name");
    if (provider_name_.empty())
    {
        throw Error("mldp_pool.provider_name must not be empty");
    }

    if (!root.hasChild("url"))
    {
        throw Error(makeMissingFieldMessage("url"));
    }
    url_ = root.get("url");
    if (url_.empty())
    {
        throw Error("mldp_pool.url must not be empty");
    }

    if (!root.hasChild("min_conn"))
    {
        throw Error(makeMissingFieldMessage("min_conn"));
    }
    min_conn_ = root.getInt("min_conn");
    if (min_conn_ <= 0)
    {
        throw Error("mldp_pool.min_conn must be greater than zero");
    }

    if (!root.hasChild("max_conn"))
    {
        throw Error(makeMissingFieldMessage("max_conn"));
    }
    max_conn_ = root.getInt("max_conn");
    if (max_conn_ <= 0)
    {
        throw Error("mldp_pool.max_conn must be greater than zero");
    }
    if (max_conn_ < min_conn_)
    {
        throw Error("mldp_pool.max_conn must be greater than or equal to min_conn");
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

            if (credentialsTree.has_child("pem_cert_chain"))
            {
                std::string path;
                credentialsTree["pem_cert_chain"] >> path;
                credentials_.ssl_options.pem_cert_chain = readFile(path);
            }
            if (credentialsTree.has_child("pem_private_key"))
            {
                std::string path;
                credentialsTree["pem_private_key"] >> path;
                credentials_.ssl_options.pem_private_key = readFile(path);
            }
            if (credentialsTree.has_child("pem_root_certs"))
            {
                std::string path;
                credentialsTree["pem_root_certs"] >> path;
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

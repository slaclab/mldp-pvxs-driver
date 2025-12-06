#include <util/pool/MLDPGrpcPoolConfig.h>

#include <sstream>

namespace {
std::string missingField(const std::string& field)
{
    std::ostringstream oss;
    oss << "Missing required field '" << field << "' in mldp_pool configuration";
    return oss.str();
}
} // namespace

namespace mldp_pvxs_driver::util::pool {

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

void MLDPGrpcPoolConfig::parse(const config::Config& root)
{
    if (!root.valid())
    {
        throw Error("mldp_pool configuration node is invalid");
    }
    if (!root.hasChild("provider_name"))
    {
        throw Error(missingField("provider_name"));
    }
    provider_name_ = root.get("provider_name");
    if (provider_name_.empty())
    {
        throw Error("mldp_pool.provider_name must not be empty");
    }

    if (!root.hasChild("url"))
    {
        throw Error(missingField("url"));
    }
    url_ = root.get("url");
    if (url_.empty())
    {
        throw Error("mldp_pool.url must not be empty");
    }

    if (!root.hasChild("min_conn"))
    {
        throw Error(missingField("min_conn"));
    }
    min_conn_ = root.getInt("min_conn");
    if (min_conn_ <= 0)
    {
        throw Error("mldp_pool.min_conn must be greater than zero");
    }

    if (!root.hasChild("max_conn"))
    {
        throw Error(missingField("max_conn"));
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

    valid_ = true;
}

} // namespace mldp_pvxs_driver::util::pool

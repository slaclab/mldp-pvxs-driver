#include <controller/MLDPPVXSControllerConfig.h>

#include <sstream>

namespace {
std::string missingField(const std::string& field)
{
    std::ostringstream oss;
    oss << "Missing required field '" << field << "' in controller config";
    return oss.str();
}
} // namespace

namespace mldp_pvxs_driver::controller {

MLDPPVXSControllerConfig::MLDPPVXSControllerConfig() = default;

MLDPPVXSControllerConfig::MLDPPVXSControllerConfig(const ::mldp_pvxs_driver::config::Config& root)
{
    if (!root.valid())
    {
        throw Error("Controller configuration node is invalid");
    }

    parse(root);
}

bool MLDPPVXSControllerConfig::valid() const
{
    return valid_;
}

const MLDPPVXSControllerConfig::PoolConfig& MLDPPVXSControllerConfig::pool() const
{
    return pool_;
}

const std::string& MLDPPVXSControllerConfig::providerName() const
{
    return pool_.provider_name;
}

int MLDPPVXSControllerConfig::controllerThreadPoolSize() const
{
    return controllerThreadPoolSize_;
}

const std::vector<reader::impl::epics::EpicsReaderConfig>&
MLDPPVXSControllerConfig::epicsReaders() const
{
    return epicsReaders_;
}

const std::optional<metrics::MetricsConfig>& MLDPPVXSControllerConfig::metricsConfig() const
{
    return metricsConfig_;
}

void MLDPPVXSControllerConfig::parse(const ::mldp_pvxs_driver::config::Config& root)
{
    parseThreadPool(root);
    parsePool(root);
    parseReaders(root);
    parseMetrics(root);
    valid_ = true;
}

void MLDPPVXSControllerConfig::parseThreadPool(const ::mldp_pvxs_driver::config::Config& root)
{
    if (!root.hasChild("controller_thread_pool"))
    {
        throw Error(missingField("controller_thread_pool"));
    }

    const auto threadPoolNodes = root.subConfig("controller_thread_pool");
    if (threadPoolNodes.empty())
    {
        throw Error(missingField("controller_thread_pool"));
    }

    const auto& threadPoolNode = threadPoolNodes.front();
    if (!threadPoolNode.raw().has_val())
    {
        throw Error("controller_thread_pool must be a scalar");
    }

    threadPoolNode >> controllerThreadPoolSize_;
    if (controllerThreadPoolSize_ <= 0)
    {
        throw Error("controller_thread_pool must be greater than zero");
    }
}

void MLDPPVXSControllerConfig::parsePool(const ::mldp_pvxs_driver::config::Config& root)
{
    if (!root.hasChild("mldp_pool"))
    {
        throw Error(missingField("mldp_pool"));
    }

    const auto poolNodes = root.subConfig("mldp_pool");
    if (poolNodes.empty())
    {
        throw Error(missingField("mldp_pool"));
    }

    const auto& poolNode = poolNodes.front();
    if (!poolNode.raw().is_map())
    {
        throw Error("mldp_pool must be a map");
    }

    if (!poolNode.hasChild("provider_name"))
    {
        throw Error(missingField("mldp_pool.provider_name"));
    }

    const auto providerNodes = poolNode.subConfig("provider_name");
    if (providerNodes.empty())
    {
        throw Error(missingField("mldp_pool.provider_name"));
    }

    const auto& providerNode = providerNodes.front();
    if (!providerNode.raw().has_val())
    {
        throw Error("mldp_pool.provider_name must be a scalar");
    }

    providerNode >> pool_.provider_name;
    if (pool_.provider_name.empty())
    {
        throw Error("mldp_pool.provider_name must not be empty");
    }

    if (!poolNode.hasChild("url"))
    {
        throw Error(missingField("mldp_pool.url"));
    }

    const auto urlNodes = poolNode.subConfig("url");
    if (urlNodes.empty())
    {
        throw Error(missingField("mldp_pool.url"));
    }

    const auto& urlNode = urlNodes.front();
    if (!urlNode.raw().has_val())
    {
        throw Error("mldp_pool.url must be a scalar");
    }

    urlNode >> pool_.url;
    if (pool_.url.empty())
    {
        throw Error("mldp_pool.url must not be empty");
    }

    if (!poolNode.hasChild("min_conn"))
    {
        throw Error(missingField("mldp_pool.min_conn"));
    }

    const auto minConnNodes = poolNode.subConfig("min_conn");
    if (minConnNodes.empty())
    {
        throw Error(missingField("mldp_pool.min_conn"));
    }

    const auto& minConnNode = minConnNodes.front();
    if (!minConnNode.raw().has_val())
    {
        throw Error("mldp_pool.min_conn must be a scalar");
    }

    minConnNode >> pool_.min_conn;
    if (pool_.min_conn <= 0)
    {
        throw Error("mldp_pool.min_conn must be greater than zero");
    }

    if (!poolNode.hasChild("max_conn"))
    {
        throw Error(missingField("mldp_pool.max_conn"));
    }

    const auto maxConnNodes = poolNode.subConfig("max_conn");
    if (maxConnNodes.empty())
    {
        throw Error(missingField("mldp_pool.max_conn"));
    }

    const auto& maxConnNode = maxConnNodes.front();
    if (!maxConnNode.raw().has_val())
    {
        throw Error("mldp_pool.max_conn must be a scalar");
    }

    maxConnNode >> pool_.max_conn;
    if (pool_.max_conn <= 0)
    {
        throw Error("mldp_pool.max_conn must be greater than zero");
    }

    if (pool_.max_conn < pool_.min_conn)
    {
        throw Error("mldp_pool.max_conn must be greater than or equal to min_conn");
    }
}

void MLDPPVXSControllerConfig::parseReaders(const ::mldp_pvxs_driver::config::Config& root)
{
    epicsReaders_.clear();

    if (!root.hasChild("reader"))
    {
        return;
    }

    if (!root.isSequence("reader"))
    {
        throw Error("reader must be a sequence");
    }

    const auto readerBlocks = root.subConfig("reader");
    for (const auto& readerBlock : readerBlocks)
    {
        if (!readerBlock.raw().is_map())
        {
            throw Error("Each entry in reader must be a map");
        }

        bool handledType = false;

        if (readerBlock.hasChild("epics"))
        {
            handledType = true;

            if (!readerBlock.isSequence("epics"))
            {
                throw Error("reader[].epics must be a sequence");
            }

            const auto epicsNodes = readerBlock.subConfig("epics");
            for (const auto& epicsNode : epicsNodes)
            {
                epicsReaders_.emplace_back(epicsNode);
            }
        }

        if (!handledType)
        {
            throw Error("reader entry does not specify a supported type (expected 'epics')");
        }
    }
}

void MLDPPVXSControllerConfig::parseMetrics(const ::mldp_pvxs_driver::config::Config& root)
{
    metricsConfig_.reset();
    if (!root.hasChild("metrics"))
    {
        return;
    }

    const auto metricsNodes = root.subConfig("metrics");
    if (metricsNodes.empty())
    {
        throw Error("metrics block is present but empty");
    }

    metricsConfig_.emplace(metricsNodes.front());
}

} // namespace mldp_pvxs_driver::controller

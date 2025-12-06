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

const util::pool::MLDPGrpcPoolConfig& MLDPPVXSControllerConfig::pool() const
{
    return pool_;
}

const std::string& MLDPPVXSControllerConfig::providerName() const
{
    return pool_.providerName();
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

    try
    {
        pool_ = util::pool::MLDPGrpcPoolConfig(poolNodes.front());
    }
    catch (const util::pool::MLDPGrpcPoolConfig::Error& e)
    {
        throw Error(e.what());
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

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

const std::vector<reader::impl::epics::EpicsReaderConfig>&
MLDPPVXSControllerConfig::epicsReaders() const
{
    return epicsReaders_;
}

void MLDPPVXSControllerConfig::parse(const ::mldp_pvxs_driver::config::Config& root)
{
    parsePool(root);
    parseReaders(root);
    valid_ = true;
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

} // namespace mldp_pvxs_driver::controller


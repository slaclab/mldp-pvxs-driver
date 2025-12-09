// ReaderFactory.cpp
#include <reader/ReaderFactory.h>

using namespace mldp_pvxs_driver::reader;
using namespace mldp_pvxs_driver::util::bus;

std::unordered_map<std::string, ReaderFactory::CreatorFn>&
ReaderFactory::registry()
{
    static std::unordered_map<std::string, CreatorFn> instance;
    return instance;
}

void ReaderFactory::registerType(const std::string& type, CreatorFn fn)
{
    registry()[type] = std::move(fn);
}

std::unique_ptr<Reader> ReaderFactory::create(
    const std::string&                                            type,
    std::shared_ptr<::mldp_pvxs_driver::util::bus::IEventBusPush> bus,
    const ::mldp_pvxs_driver::config::Config&                     cfg,
    std::shared_ptr<mldp_pvxs_driver::metrics::Metrics>           metrics)
{
    auto& reg = registry();
    auto  it = reg.find(type);
    if (it == reg.end())
    {
        throw std::runtime_error("Unknown reader type: " + type);
    }
    return it->second(std::move(bus), std::move(metrics), cfg);
}

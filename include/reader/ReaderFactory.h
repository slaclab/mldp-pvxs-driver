// ReaderFactory.hpp
#pragma once

#include <bus/IEventBusPush.h>
#include <config/Config.h>
#include <reader/Reader.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace mldp_pvxs_driver::reader {

class ReaderFactory
{
public:
    using CreatorFn =
        std::function<std::unique_ptr<Reader>(std::shared_ptr<mldp_pvxs_driver::bus::IEventBusPush>, const mldp_pvxs_driver::config::Config&)>;

    static void registerType(const std::string& type, CreatorFn fn);

    static std::unique_ptr<Reader> create(
        const std::string&                                      type,
        std::shared_ptr<::mldp_pvxs_driver::bus::IEventBusPush> bus,
        const mldp_pvxs_driver::config::Config&                 cfg);

private:
    static std::unordered_map<std::string, CreatorFn>& registry();
};

template <typename ReaderT>
class ReaderRegistrator
{
public:
    explicit ReaderRegistrator(const char* typeName)
    {
        ReaderFactory::registerType(
            typeName,
            [](std::shared_ptr<mldp_pvxs_driver::bus::IEventBusPush> bus, const mldp_pvxs_driver::config::Config& cfg)
            {
                return std::make_unique<ReaderT>(std::move(bus), cfg);
            });
    }
};

#define REGISTER_READER(TYPE_STRING, CLASSNAME) \
    static inline ReaderRegistrator<CLASSNAME> registrator_{TYPE_STRING};
} // namespace mldp_pvxs_driver::reader

// Reader.hpp
#pragma once

#include <memory>
#include <string>

namespace mldp_pvxs_driver::bus {
class IEventBusPush;
}

namespace mldp_pvxs_driver::reader {

class Reader
{
public:
    explicit Reader(std::shared_ptr<bus::IEventBusPush> bus)
        : bus_(std::move(bus)) {}

    virtual ~Reader() = default;

    virtual std::string name() const = 0;

protected:
    std::shared_ptr<bus::IEventBusPush> bus_;
};

} // namespace mldp_pvxs_driver::reader

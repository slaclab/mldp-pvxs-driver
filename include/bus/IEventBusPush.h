// IEventBusPush.hpp
#pragma once

namespace mldp_pvxs_driver::bus{

class IEventBusPush {
public:
    virtual ~IEventBusPush() = default;

    virtual bool push(/*Event evt*/) = 0;
};

}


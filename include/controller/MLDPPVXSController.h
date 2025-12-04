#pragma once

#include <config/Config.h>

namespace mldp_pvxs_driver::controller {

class MLDPPVXSController
{
public:
    MLDPPVXSController(config::Config config);
    ~MLDPPVXSController();
    void start();
    void stop();

private:
    bool running_;
};

} // namespace mldp_pvxs_driver::controller
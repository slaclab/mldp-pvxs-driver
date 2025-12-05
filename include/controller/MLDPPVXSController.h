#pragma once

#include <config/Config.h>
#include <controller/MLDPPVXSControllerConfig.h>
#include <metrics/Metrics.h>
#include <util/bus/IEventBusPush.h>
#include <util/pool/MLDPGrpcPool.h>

#include <BS_thread_pool.hpp>

namespace mldp_pvxs_driver::controller {

class MLDPPVXSController : public util::bus::IEventBusPush
{
public:
    explicit MLDPPVXSController(const config::Config& config);
    ~MLDPPVXSController() override;
    void start();
    void stop();
    bool push(EventValue data_value) override;

private:
    MLDPPVXSControllerConfig                      config_;
    std::shared_ptr<BS::light_thread_pool>        thread_pool_;
    std::shared_ptr<metrics::Metrics>             metrics_;
    util::pool::MLDPGrpcPool::MLDPGrpcPoolShrdPtr mldp_pool_;
    bool                                          running_;
};

} // namespace mldp_pvxs_driver::controller

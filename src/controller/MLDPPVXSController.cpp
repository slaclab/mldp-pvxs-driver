#include <controller/MLDPPVXSController.h>

using namespace mldp_pvxs_driver::controller;

MLDPPVXSController::MLDPPVXSController(config::Config config)
    : running_(false)
{
    // Constructor implementation
}

MLDPPVXSController::~MLDPPVXSController()
{
    // Destructor implementation
}

void MLDPPVXSController::start()
{
    running_ = true;
    // Start controller logic
}

void MLDPPVXSController::stop()
{
    running_ = false;
    // Stop controller logic
}
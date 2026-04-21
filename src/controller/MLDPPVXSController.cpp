//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include "util/log/Logger.h"
#include <controller/MLDPPVXSController.h>
#include <future>
#include <memory>
#include <reader/ReaderFactory.h>
#include <util/StringFormat.h>
#include <writer/WriterFactory.h>

#include <stdexcept>

using namespace mldp_pvxs_driver::metrics;
using namespace mldp_pvxs_driver::controller;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::reader;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::writer;

namespace {
// Creates a dedicated logger for controller lifecycle and bus operations.
std::shared_ptr<mldp_pvxs_driver::util::log::ILogger> makeControllerLogger(const std::string& name)
{
    return mldp_pvxs_driver::util::log::newLogger("controller." + name);
}
} // namespace

std::shared_ptr<MLDPPVXSController> MLDPPVXSController::create(const config::Config& config)
{
    return std::shared_ptr<MLDPPVXSController>(new MLDPPVXSController(config));
}

MLDPPVXSController::MLDPPVXSController(const config::Config& config)
    : config_(config)
    , logger_(makeControllerLogger(config_.name()))
    , thread_pool_(std::make_shared<BS::light_thread_pool>(1)) // resized in start()
    , metrics_(std::make_shared<metrics::Metrics>(*config_.metricsConfig(), config_.name()))
    , running_(false)
{
}

MLDPPVXSController::~MLDPPVXSController()
{
    if (running_.load())
    {
        stop();
    }
    thread_pool_.reset();
    metrics_.reset();
}

void MLDPPVXSController::start()
{
    if (running_.load())
    {
        warnf(*logger_, "Controller is already started");
        return;
    }

    // Validate minimal requirements before allocating any resources.
    if (config_.writerEntries().empty())
    {
        throw std::runtime_error("Controller: no writers are configured; add at least one writer instance under 'writer:'");
    }
    if (config_.readerEntries().empty())
    {
        throw std::runtime_error("Controller: no readers are configured; add at least one reader instance under 'reader:'");
    }

    running_.store(true);
    infof(*logger_, "Controller is starting");

    // Resize the fan-out thread pool to match the number of writer instances.
    const std::size_t numWriters = config_.writerEntries().size();
    thread_pool_ = std::make_shared<BS::light_thread_pool>(
        std::max(numWriters, std::size_t{1}),
        [](std::size_t i) { BS::this_thread::set_os_thread_name("ctrl-pool-" + std::to_string(i)); });

    // -- Build writers via factory from configured entries --
    for (const auto& [type, writerNode] : config_.writerEntries())
    {
        auto w = WriterFactory::create(type, writerNode, metrics_);
        w->start();
        writers_.push_back(std::move(w));
    }

    // -- Readers --
    infof(*logger_, "Starting readers");
    for (const auto& entry : config_.readerEntries())
    {
        const auto& type = entry.first;
        const auto& readerConfig = entry.second;
        auto        reader = ReaderFactory::create(type, shared_from_this(), readerConfig, metrics_);
        readers_.push_back(std::move(reader));
    }
    infof(*logger_, "Controller started");
}

void MLDPPVXSController::stop()
{
    if (!running_.load())
    {
        warnf(*logger_, "Controller already stopped");
        return;
    }
    infof(*logger_, "Controller is stopping");
    running_.store(false);

    readers_.clear();

    for (auto& w : writers_)
    {
        w->stop();
    }
    writers_.clear();

    infof(*logger_, "Controller stopped");
}

bool MLDPPVXSController::push(EventBatch batch_values)
{
    if (!running_.load())
    {
        return false;
    }

    if (batch_values.root_source.empty())
    {
        warnf(*logger_, "Received batch with empty root source, skipping push.");
        return false;
    }

    if (batch_values.frames.empty() && !batch_values.end_of_source_update)
    {
        warnf(*logger_, "Received empty batch for root source {}, skipping push.", batch_values.root_source);
        return false;
    }

    // Parallel fan-out: submit one task per writer to the thread pool so all
    // writers run concurrently.  Every writer receives its own copy of the
    // batch;
    const std::string rootSource = batch_values.root_source;
    const std::size_t n = writers_.size();

    std::vector<std::future<bool>> futures;
    futures.reserve(n);

    for (std::size_t i = 0; i < n; ++i)
    {
        // Capture writer pointer and a copy of the batch per task.
        auto*      writerPtr = writers_[i].get();
        EventBatch batchCopy = batch_values; // explicit copy for each task
        futures.push_back(
            thread_pool_->submit_task([writerPtr, b = std::move(batchCopy)]() mutable -> bool
                                      {
                                          return writerPtr->push(std::move(b));
                                      }));
    }

    // Collect results; warn for any writer that rejected the batch.
    bool anyAccepted = false;
    for (std::size_t i = 0; i < n; ++i)
    {
        const bool ok = futures[i].get();
        if (!ok)
        {
            warnf(*logger_, "Writer '{}' rejected batch for source {}",
                  writers_[i]->name(), rootSource);
        }
        anyAccepted = anyAccepted || ok;
    }
    return anyAccepted;
}

Metrics& MLDPPVXSController::metrics() const
{
    if (!metrics_)
    {
        throw std::runtime_error("Metrics not configured for controller");
    }
    return *metrics_;
}

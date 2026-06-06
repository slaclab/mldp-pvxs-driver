//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <processor/ChannelProcessor.h>

#include <util/log/Logger.h>

#include <exception>
#include <utility>
#include <variant>

namespace mldp_pvxs_driver::processor {

namespace {

std::shared_ptr<util::log::ILogger> makeProcessorLogger(const std::string& name)
{
    return util::log::newLogger("processor." + name);
}

} // namespace

ChannelProcessor::ChannelProcessor(MLDPChannelProcessorConfig               config,
                                   IAlgorithmUPtr                           algorithm,
                                   std::shared_ptr<util::bus::IDataBus>     bus,
                                   std::shared_ptr<metrics::Metrics>        metrics,
                                   std::shared_ptr<BS::light_thread_pool>   thread_pool)
    : config_(std::move(config))
    , algorithm_(std::move(algorithm))
    , bus_(std::move(bus))
    , metrics_(std::move(metrics))
    , logger_(makeProcessorLogger(config_.name()))
    , thread_pool_(std::move(thread_pool))
    , buffer_(config_.sources(), config_.alignment())
{
}

ChannelProcessor::~ChannelProcessor()
{
    stop();
}

std::string ChannelProcessor::name() const
{
    return config_.name();
}

void ChannelProcessor::start()
{
    if (running_.exchange(true))
    {
        util::log::warnf(*logger_, "ChannelProcessor '{}' already started", config_.name());
        return;
    }

    util::log::infof(*logger_, "ChannelProcessor '{}' started", config_.name());
}

void ChannelProcessor::stop() noexcept
{
    if (!running_.exchange(false))
    {
        return;
    }

    {
        std::unique_lock<std::mutex> lock(drain_mutex_);
        drain_cv_.wait(lock, [this] {
            return pending_tasks_.load(std::memory_order_acquire) == 0;
        });
    }

    buffer_.clear();
    util::log::infof(*logger_, "ChannelProcessor '{}' stopped", config_.name());
}

bool ChannelProcessor::push(util::bus::IDataBus::EventBatch batch) noexcept
{
    ++pending_tasks_;
    thread_pool_->detach_task(
        [this, b = std::move(batch)]() mutable
        {
            if (running_.load(std::memory_order_relaxed) &&
                std::holds_alternative<util::bus::TimeSeriesPayload>(b.payload))
            {
                processTask(std::move(b));
            }
            if (--pending_tasks_ == 0)
                drain_cv_.notify_all();
        });
    return true;
}

void ChannelProcessor::processTask(util::bus::IDataBus::EventBatch batch) noexcept
{
    const auto& ts = std::get<util::bus::TimeSeriesPayload>(batch.payload);

    buffer_.ingest(ts.root_source_name, ts);

    if (config_.trigger() == TriggerPolicy::Interval)
        return;

    auto snapshot = buffer_.trySnapshot(config_.trigger());
    if (!snapshot.has_value())
        return;

    buffer_.resetFreshFlags();
    fireCompute(*snapshot);
}

bool ChannelProcessor::acceptsPayload(const util::bus::BatchPayload& payload) const noexcept
{
    return std::holds_alternative<util::bus::TimeSeriesPayload>(payload);
}

bool ChannelProcessor::supports_multi_root_source() const noexcept
{
    return true;
}

const std::string& ChannelProcessor::outputReaderName() const noexcept
{
    return config_.name();
}

std::vector<std::string> ChannelProcessor::outputSourceNames() const noexcept
{
    return algorithm_->outputSources();
}

const std::vector<std::string>& ChannelProcessor::inputSourceNames() const noexcept
{
    return config_.sources();
}

void ChannelProcessor::fireCompute(const AlignedSnapshot& snapshot) noexcept
{
    std::vector<AlgorithmOutput> outputs;
    try
    {
        outputs = algorithm_->compute(snapshot);
    }
    catch (const std::exception& ex)
    {
        util::log::warnf(*logger_, "ChannelProcessor '{}' compute failed: {}", config_.name(), ex.what());
        return;
    }
    catch (...)
    {
        util::log::warnf(*logger_, "ChannelProcessor '{}' compute failed with unknown exception", config_.name());
        return;
    }

    for (auto& output : outputs)
    {
        util::bus::IDataBus::EventBatch batch;
        batch.reader_name = config_.name();
        batch.payload = std::move(output.payload);
        if (bus_)
        {
            bus_->push(std::move(batch));
        }
    }
}

} // namespace mldp_pvxs_driver::processor

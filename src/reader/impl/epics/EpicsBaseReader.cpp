//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics/EpicsBaseReader.h>

#include <config/Config.h>
#include <metrics/Metrics.h>
#include <reader/impl/epics/EpicsPVDataConversion.h>
#include <util/log/Logger.h>

#include <chrono>

using namespace mldp_pvxs_driver::reader::impl::epics;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::metrics;
using namespace mldp_pvxs_driver::util::bus;

namespace {
std::shared_ptr<ILogger> makeLogger(const std::string& readerName)
{
    std::string loggerName = "epics_reader_base";
    if (!readerName.empty())
    {
        loggerName += ":";
        loggerName += readerName;
    }
    return mldp_pvxs_driver::util::log::newLogger(loggerName);
}
} // namespace

EpicsBaseReader::EpicsBaseReader(std::shared_ptr<util::bus::IDataBus> bus,
                                 std::shared_ptr<metrics::Metrics>         metrics,
                                 const config::Config&                     cfg)
    : EpicsReaderBase(std::move(bus), std::move(metrics), EpicsReaderConfig(cfg), makeLogger(cfg.get("name")))
{
    addPV(pvNames());
}

EpicsBaseReader::~EpicsBaseReader()
{
    running_ = false;
    epics_base_poller_.reset();
}

void EpicsBaseReader::addPV(const PVSet& pvNames)
{
    const std::vector<std::string> pv_list(pvNames.begin(), pvNames.end());
    epics_base_poller_ = std::make_unique<EpicsBaseMonitorPoller>(
        pv_list,
        config_.monitorPollThreads(),
        config_.monitorPollIntervalMs(),
        [this]()
        {
            drainEpicsBaseQueue();
        },
        logger_);
}

void EpicsBaseReader::drainEpicsBaseQueue()
{
    if (!epics_base_poller_)
    {
        return;
    }

    std::lock_guard<std::mutex> lk(epics_base_drain_mutex_);
    epics_base_poller_->drain([this](const std::string& pvName, ::epics::pvData::PVStructurePtr value)
                              {
                                  metric_call(metrics_, [&](auto& m)
                                              {
                                                  m.incrementReaderEventsReceived(1.0, {{"source", pvName}});
                                              });
                                  std::string pv = pvName;
                                  reader_pool_->detach_task(
                                      [this, n = std::move(pv), v = std::move(value)]() mutable
                                      {
                                          processEvent(std::move(n), std::move(v));
                                      });
                                  metric_call(metrics_, [&](auto& m)
                                              {
                                                  m.setReaderPoolQueueDepth(
                                                      static_cast<double>(reader_pool_->get_tasks_queued()),
                                                      {{"reader", name_}});
                                              });
                              });
}

void EpicsBaseReader::processEvent(std::string pvName, ::epics::pvData::PVStructurePtr epics_value)
{
    if (!running_.load())
    {
        return;
    }

    metric_call(metrics_, [&](auto& m)
                {
                    m.setReaderPoolQueueDepth(
                        static_cast<double>(reader_pool_->get_tasks_queued()),
                        {{"reader", name_}});
                });

    const prometheus::Labels sourceTag{{"source", pvName}};
    try
    {
        const auto processing_start = std::chrono::steady_clock::now();

        const auto* runtimeCfg = runtimeConfigFor(pvName);
        const auto  mode = runtimeCfg ? runtimeCfg->mode : PVRuntimeConfig::Mode::Default;

        size_t emitted = 0;

        switch (mode)
        {
        case PVRuntimeConfig::Mode::SlacBsasTable:
            {
                const std::size_t colBatchSize = config_.columnBatchSize();
                IDataBus::EventBatch tableBatch;
                tableBatch.root_source = pvName;
                tableBatch.tags.push_back(pvName);
                std::size_t colsInBatch = 0;

                if (!EpicsPVDataConversion::tryBuildNtTableRowTsBatch(
                        *logger_, pvName, epics_value,
                        runtimeCfg ? runtimeCfg->tsSecondsField : "secondsPastEpoch",
                        runtimeCfg ? runtimeCfg->tsNanosField : "nanoseconds",
                        [&](std::string colName, std::vector<IDataBus::EventValue> events) {
                            tableBatch.values[std::move(colName)] = std::move(events);
                            ++colsInBatch;
                            if (colBatchSize > 0 && colsInBatch >= colBatchSize)
                            {
                                bus_->push(std::move(tableBatch));
                                tableBatch = IDataBus::EventBatch{};
                                tableBatch.root_source = pvName;
                                tableBatch.tags.push_back(pvName);
                                colsInBatch = 0;
                            }
                        },
                        emitted))
                {
                    errorf(*logger_, "Error converting PV {} to MLDP SLAC BSAS table batch on reader {}.", pvName, name_);
                    metric_call(metrics_, [&](auto& m)
                                {
                                    m.incrementReaderErrors(1.0, sourceTag);
                                });
                }
                else if (!tableBatch.values.empty())
                {
                    bus_->push(std::move(tableBatch));
                }
            }
            break;

        case PVRuntimeConfig::Mode::Default:
        default:
            {
                IDataBus::EventBatch batch;
                batch.root_source = pvName;
                uint64_t epoch_seconds = 0;
                uint64_t nanoseconds = 0;
                bool setEpoch = false;

                if (epics_value)
                {
                    if (auto timeStamp = epics_value->getSubField<::epics::pvData::PVStructure>("timeStamp"))
                    {
                        if (auto secondsField = timeStamp->getSubField<::epics::pvData::PVScalar>("secondsPastEpoch"))
                        {
                            epoch_seconds = secondsField->getAs<uint64_t>();
                            setEpoch = true;
                        }
                        if (auto nanosField = timeStamp->getSubField<::epics::pvData::PVScalar>("nanoseconds"))
                        {
                            nanoseconds = nanosField->getAs<uint64_t>();
                        }
                    }
                }
                if (!setEpoch)
                {
                    const auto now = std::chrono::system_clock::now().time_since_epoch();
                    epoch_seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
                }

                auto event_value = IDataBus::MakeEventValue(epoch_seconds, nanoseconds);

                if (epics_value)
                {
                    auto valueField = epics_value->getSubField("value");
                    if (valueField)
                    {
                        const bool isStructPayload =
                            (std::dynamic_pointer_cast<::epics::pvData::PVStructure>(valueField) != nullptr);
                        const std::string columnName = isStructPayload ? "value" : pvName;
                        EpicsPVDataConversion::convertPVToProtoValue(
                            *valueField, &event_value->data_value, columnName);
                    }
                    else
                    {
                        EpicsPVDataConversion::convertPVToProtoValue(
                            *epics_value, &event_value->data_value, pvName);
                    }
                }

                batch.tags.push_back(pvName);
                batch.values[pvName].emplace_back(std::move(event_value));
                emitted = 1;
                bus_->push(std::move(batch));
            }
            break;
        }

        const auto   processing_end = std::chrono::steady_clock::now();
        const double processing_ms = std::chrono::duration<double, std::milli>(processing_end - processing_start).count();
        metric_call(metrics_, [&](auto& m)
                    {
                        m.observeReaderProcessingTimeMs(processing_ms, sourceTag);
                    });

        if (emitted > 0)
        {
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementReaderEvents(static_cast<double>(1.0), sourceTag);
                        });
            tracef(*logger_, "[{}/{}] event published", name_, pvName);
        }
    }
    catch (const std::exception& e)
    {
        errorf(*logger_, "Error when reading PV {} on reader {}: {}", pvName, name_, e.what());
        metric_call(metrics_, [&](auto& m)
                    {
                        m.incrementReaderErrors(1.0, sourceTag);
                    });
    }
}

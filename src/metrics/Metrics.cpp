//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <metrics/Metrics.h>


#include <utility>

namespace {

prometheus::Family<prometheus::Counter>&
makeCounterFamily(prometheus::Registry& registry, std::string name, std::string help)
{
    return prometheus::BuildCounter()
        .Name(std::move(name))
        .Help(std::move(help))
        .Register(registry);
}

prometheus::Family<prometheus::Gauge>&
makeGaugeFamily(prometheus::Registry& registry, std::string name, std::string help)
{
    return prometheus::BuildGauge()
        .Name(std::move(name))
        .Help(std::move(help))
        .Register(registry);
}

} // namespace

namespace mldp_pvxs_driver::metrics {

Metrics::Metrics(const MetricsConfig& config)
    : config_(config)
    , registry_(std::make_shared<prometheus::Registry>())
    , reader_events_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_reader_events_total", "Total events processed by readers."))
    , reader_errors_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_reader_errors_total", "Total reader failures observed when pulling data."))
    , pool_connections_in_use_family_(makeGaugeFamily(*registry_, "mldp_pvxs_driver_pool_connections_in_use", "Number of MLDP gRPC connections currently in use."))
    , pool_connections_available_family_(makeGaugeFamily(*registry_, "mldp_pvxs_driver_pool_connections_available", "Idle MLDP gRPC connections ready for use."))
    , bus_push_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_bus_push_total", "Number of events pushed onto the bus."))
    , bus_failure_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_bus_failure_total", "Number of bus push failures reported by the MLDP gRPC API."))
    , bus_payload_bytes_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_bus_payload_bytes_total", "Total protobuf payload bytes written to the MLDP ingestion stream."))
    , bus_payload_bytes_per_second_family_(makeGaugeFamily(*registry_, "mldp_pvxs_driver_bus_payload_bytes_per_second", "Bytes/second for the most recent successful ingestion batch (protobuf payload bytes / end-to-end batch duration)."))
{
    if (config_.valid() && !config_.endpoint().empty())
    {
        exposer_ = std::make_unique<prometheus::Exposer>(config_.endpoint());
        exposer_->RegisterCollectable(registry_);
    }
}

std::shared_ptr<prometheus::Registry> Metrics::registry() const
{
    return registry_;
}

void Metrics::incrementReaderEvents(double value, prometheus::Labels tags)
{

    reader_events_family_.Add(std::move(tags)).Increment(value);
}

void Metrics::incrementReaderErrors(double value, prometheus::Labels tags)
{

    reader_errors_family_.Add(std::move(tags)).Increment(value);
}

void Metrics::setPoolConnectionsInUse(double value, prometheus::Labels tags)
{
    pool_connections_in_use_family_.Add(std::move(tags)).Set(value);
}

void Metrics::setPoolConnectionsAvailable(double value, prometheus::Labels tags)
{
    pool_connections_available_family_.Add(std::move(tags)).Set(value);
}

double Metrics::poolConnectionsInUse(prometheus::Labels tags) const
{
    return pool_connections_in_use_family_.Add(std::move(tags)).Value();
}

double Metrics::poolConnectionsAvailable(prometheus::Labels tags) const
{
    return pool_connections_available_family_.Add(std::move(tags)).Value();
}

void Metrics::incrementBusPushes(double value, prometheus::Labels tags)
{
    bus_push_family_.Add(std::move(tags)).Increment(value);
}

void Metrics::incrementBusFailures(double value, prometheus::Labels tags)
{
    bus_failure_family_.Add(std::move(tags)).Increment(value);
}

void Metrics::incrementBusPayloadBytes(double value, prometheus::Labels tags)
{
    bus_payload_bytes_family_.Add(std::move(tags)).Increment(value);
}

void Metrics::setBusPayloadBytesPerSecond(double value, prometheus::Labels tags)
{
    bus_payload_bytes_per_second_family_.Add(std::move(tags)).Set(value);
}

double Metrics::busPushTotal(prometheus::Labels tags) const
{
    return bus_push_family_.Add(std::move(tags)).Value();
}

double Metrics::busFailuresTotal(prometheus::Labels tags) const
{
    return bus_failure_family_.Add(std::move(tags)).Value();
}

double Metrics::busPayloadBytesTotal(prometheus::Labels tags) const
{
    return bus_payload_bytes_family_.Add(std::move(tags)).Value();
}

double Metrics::busPayloadBytesPerSecond(prometheus::Labels tags) const
{
    return bus_payload_bytes_per_second_family_.Add(std::move(tags)).Value();
}

} // namespace mldp_pvxs_driver::metrics

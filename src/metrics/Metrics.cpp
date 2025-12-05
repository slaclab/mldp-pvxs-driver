#include <metrics/Metrics.h>




namespace mldp_pvxs_driver::metrics {

namespace {

prometheus::Counter* makeCounter(prometheus::Registry& registry,
                                 std::string          name,
                                 std::string          help)
{
    auto& family = prometheus::BuildCounter()
                       .Name(std::move(name))
                       .Help(std::move(help))
                       .Register(registry);
    return &family.Add({});
}

prometheus::Gauge* makeGauge(prometheus::Registry& registry,
                             std::string          name,
                             std::string          help)
{
    auto& family = prometheus::BuildGauge()
                       .Name(std::move(name))
                       .Help(std::move(help))
                       .Register(registry);
    return &family.Add({});
}

} // namespace

Metrics::Metrics()
    : config_()
    , registry_(std::make_shared<prometheus::Registry>())
    , reader_events_total_(makeCounter(*registry_, "mldp_pvxs_driver_reader_events_total", "Total EPICS events processed by readers."))
    , reader_errors_total_(makeCounter(*registry_, "mldp_pvxs_driver_reader_errors_total", "Total reader failures observed when pulling EPICS data."))
    , pool_connections_in_use_(makeGauge(*registry_, "mldp_pvxs_driver_pool_connections_in_use", "Number of MLDP gRPC connections currently in use."))
    , pool_connections_available_(makeGauge(*registry_, "mldp_pvxs_driver_pool_connections_available", "Idle MLDP gRPC connections ready for use."))
    , bus_push_total_(makeCounter(*registry_, "mldp_pvxs_driver_bus_push_total", "Number of events pushed onto the bus."))
    , bus_failure_total_(makeCounter(*registry_, "mldp_pvxs_driver_bus_failure_total", "Number of bus push failures reported by the MLDP gRPC API."))
{
}

Metrics::Metrics(const MetricsConfig& config)
    : config_(config)
    , registry_(std::make_shared<prometheus::Registry>())
    , reader_events_total_(makeCounter(*registry_, "mldp_pvxs_driver_reader_events_total", "Total EPICS events processed by readers."))
    , reader_errors_total_(makeCounter(*registry_, "mldp_pvxs_driver_reader_errors_total", "Total reader failures observed when pulling EPICS data."))
    , pool_connections_in_use_(makeGauge(*registry_, "mldp_pvxs_driver_pool_connections_in_use", "Number of MLDP gRPC connections currently in use."))
    , pool_connections_available_(makeGauge(*registry_, "mldp_pvxs_driver_pool_connections_available", "Idle MLDP gRPC connections ready for use."))
    , bus_push_total_(makeCounter(*registry_, "mldp_pvxs_driver_bus_push_total", "Number of events pushed onto the bus."))
    , bus_failure_total_(makeCounter(*registry_, "mldp_pvxs_driver_bus_failure_total", "Number of bus push failures reported by the MLDP gRPC API."))
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

void Metrics::incrementReaderEvents(double value)
{
    reader_events_total_->Increment(value);
}

void Metrics::incrementReaderErrors(double value)
{
    reader_errors_total_->Increment(value);
}

double Metrics::readerEventsTotal() const
{
    return reader_events_total_->Value();
}

double Metrics::readerErrorsTotal() const
{
    return reader_errors_total_->Value();
}

void Metrics::setPoolConnectionsInUse(double value)
{
    pool_connections_in_use_->Set(value);
}

void Metrics::setPoolConnectionsAvailable(double value)
{
    pool_connections_available_->Set(value);
}

double Metrics::poolConnectionsInUse() const
{
    return pool_connections_in_use_->Value();
}

double Metrics::poolConnectionsAvailable() const
{
    return pool_connections_available_->Value();
}

void Metrics::incrementBusPushes(double value)
{
    bus_push_total_->Increment(value);
}

void Metrics::incrementBusFailures(double value)
{
    bus_failure_total_->Increment(value);
}

double Metrics::busPushTotal() const
{
    return bus_push_total_->Value();
}

double Metrics::busFailuresTotal() const
{
    return bus_failure_total_->Value();
}

} // namespace mldp_pvxs_driver::metrics

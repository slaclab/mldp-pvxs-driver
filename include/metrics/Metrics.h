#pragma once

#include <memory>

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/registry.h>
#include <prometheus/exposer.h>

#include <metrics/MetricsConfig.h>

namespace mldp_pvxs_driver::metrics {

/**
 * @brief Metrics collector that exposes counters and gauges for the driver.
 *
 * The collector groups metrics around the three main areas of the driver:
 * - Readers: track events flowing from EPICS into the system.
 * - Pool: monitor connection pool pressure.
 * - Bus: track pushes forwarded to MLDP.
 */
class Metrics
{
public:
    Metrics();
    explicit Metrics(const MetricsConfig& config);

    /** @return Registry that can be scraped/exported by HTTP exposers. */
    std::shared_ptr<prometheus::Registry> registry() const;

    // Reader metrics ------------------------------------------------------
    void   incrementReaderEvents(double value = 1.0);
    void   incrementReaderErrors(double value = 1.0);
    double readerEventsTotal() const;
    double readerErrorsTotal() const;

    // Pool metrics --------------------------------------------------------
    void   setPoolConnectionsInUse(double value);
    void   setPoolConnectionsAvailable(double value);
    double poolConnectionsInUse() const;
    double poolConnectionsAvailable() const;

    // Bus metrics ---------------------------------------------------------
    void   incrementBusPushes(double value = 1.0);
    void   incrementBusFailures(double value = 1.0);
    double busPushTotal() const;
    double busFailuresTotal() const;

private:
    MetricsConfig                         config_;
    std::shared_ptr<prometheus::Registry> registry_;

    prometheus::Counter* reader_events_total_;
    prometheus::Counter* reader_errors_total_;

    prometheus::Gauge* pool_connections_in_use_;
    prometheus::Gauge* pool_connections_available_;

    prometheus::Counter* bus_push_total_;
    prometheus::Counter* bus_failure_total_;

    std::unique_ptr<prometheus::Exposer> exposer_;
};

} // namespace mldp_pvxs_driver::metrics

#define MLDP_METRICS_CALL(METRICS_PTR, METHOD_CALL) \
    do                                              \
    {                                               \
        if ((METRICS_PTR))                          \
        {                                           \
            (METRICS_PTR)->METHOD_CALL;             \
        }                                           \
    } while (false)

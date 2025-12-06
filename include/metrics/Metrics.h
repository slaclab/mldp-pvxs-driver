#pragma once

#include <memory>
#include <string>

#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/labels.h>
#include <prometheus/registry.h>

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
    /** @brief Construct a collector with an optional metrics configuration. */
    Metrics() = delete;
    explicit Metrics(const MetricsConfig& config);

    /** @return Registry that can be scraped/exported by HTTP exposers. */
    std::shared_ptr<prometheus::Registry> registry() const;

    // Reader metrics ------------------------------------------------------
    void   incrementReaderEvents(double value = 1.0, prometheus::Labels tags = {});
    void   incrementReaderErrors(double value = 1.0, prometheus::Labels tags = {});
    double readerEventsTotal() const;
    double readerErrorsTotal() const;

    // Pool metrics --------------------------------------------------------
    /**
     * @brief Gauge for connections currently checked out of the pool.
     */
    void setPoolConnectionsInUse(double value, prometheus::Labels tags = {});
    /**
     * @brief Gauge for idle connections available in the pool.
     */
    void setPoolConnectionsAvailable(double value, prometheus::Labels tags = {});
    double poolConnectionsInUse(prometheus::Labels tags = {}) const;
    double poolConnectionsAvailable(prometheus::Labels tags = {}) const;

    // Bus metrics ---------------------------------------------------------
    void   incrementBusPushes(double value = 1.0, prometheus::Labels tags = {});
    void   incrementBusFailures(double value = 1.0, prometheus::Labels tags = {});
    double busPushTotal(prometheus::Labels tags = {}) const;
    double busFailuresTotal(prometheus::Labels tags = {}) const;

private:
    MetricsConfig                         config_;
    std::shared_ptr<prometheus::Registry> registry_;

    prometheus::Family<prometheus::Counter>& reader_events_family_;
    prometheus::Family<prometheus::Counter>& reader_errors_family_;

    prometheus::Family<prometheus::Gauge>& pool_connections_in_use_family_;
    prometheus::Family<prometheus::Gauge>& pool_connections_available_family_;

    prometheus::Family<prometheus::Counter>& bus_push_family_;
    prometheus::Family<prometheus::Counter>& bus_failure_family_;

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

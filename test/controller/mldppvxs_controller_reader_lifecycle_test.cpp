//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <gtest/gtest.h>

#include <controller/MLDPPVXSController.h>
#include <reader/ReaderFactory.h>
#include <writer/WriterFactory.h>

#include "../config/test_config_helpers.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::controller::MLDPPVXSController;
using mldp_pvxs_driver::util::bus::ConfigurationPayload;
using mldp_pvxs_driver::util::bus::IDataBus;

namespace mldp_pvxs_driver::reader {

class TestLifecycleReader final : public Reader
{
    REGISTER_READER("test-lifecycle", TestLifecycleReader)

public:
    TestLifecycleReader(std::shared_ptr<util::bus::IDataBus> bus,
                        std::shared_ptr<metrics::Metrics>    metrics,
                        const config::Config&                cfg)
        : Reader(std::move(bus), std::move(metrics))
        , name_(cfg.get("name", "test-lifecycle-reader"))
        , complete_(cfg.getBool("complete", true))
        , delay_ms_(cfg.getInt("delay-ms", 0))
    {
        worker_ = std::thread([this] {
            if (delay_ms_ > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
            }

            ConfigurationPayload payload;
            payload.root_source_name = name_;
            payload.configuration_name = name_ + "-cfg";
            payload.category = "test";

            IDataBus::EventBatch batch;
            batch.reader_name = name_;
            batch.payload = std::move(payload);
            if (bus_)
            {
                bus_->push(std::move(batch));
            }

            if (complete_)
            {
                signalCompleted();
            }
        });
    }

    ~TestLifecycleReader() override
    {
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

    std::string name() const override { return name_; }

private:
    std::string name_;
    bool        complete_{true};
    int         delay_ms_{0};
    std::thread worker_;
};

} // namespace mldp_pvxs_driver::reader

namespace mldp_pvxs_driver::writer {

class TestLifecycleCaptureWriter final : public IWriter
{
    REGISTER_WRITER("test-lifecycle-capture", TestLifecycleCaptureWriter)

public:
    explicit TestLifecycleCaptureWriter(const config::Config&             cfg,
                                        std::shared_ptr<metrics::Metrics> /*metrics*/ = nullptr)
        : name_(cfg.get("name", "test-lifecycle-capture-writer"))
    {
    }

    std::string name() const override { return name_; }

    void start() override
    {
        start_calls.fetch_add(1, std::memory_order_relaxed);
    }

    void stop() noexcept override
    {
        stop_calls.fetch_add(1, std::memory_order_relaxed);
    }

    bool push(util::bus::IDataBus::EventBatch batch) noexcept override
    {
        std::lock_guard<std::mutex> lock(mu_);
        pushed_reader_names.push_back(batch.reader_name);
        return true;
    }

    static void reset()
    {
        start_calls.store(0, std::memory_order_relaxed);
        stop_calls.store(0, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(mu_);
        pushed_reader_names.clear();
    }

    static std::atomic<int>         start_calls;
    static std::atomic<int>         stop_calls;
    static std::mutex               mu_;
    static std::vector<std::string> pushed_reader_names;

private:
    std::string name_;
};

std::atomic<int>         TestLifecycleCaptureWriter::start_calls{0};
std::atomic<int>         TestLifecycleCaptureWriter::stop_calls{0};
std::mutex               TestLifecycleCaptureWriter::mu_;
std::vector<std::string> TestLifecycleCaptureWriter::pushed_reader_names;

} // namespace mldp_pvxs_driver::writer

namespace {

bool waitForValue(const std::atomic<int>& counter, int target, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (counter.load(std::memory_order_relaxed) >= target)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return counter.load(std::memory_order_relaxed) >= target;
}

std::string makeControllerYaml(const std::string& reader_block)
{
    return "name: lifecycle-controller-test\n"
           "writer:\n"
           "  test-lifecycle-capture:\n"
           "    - name: lifecycle-capture\n"
           "reader:\n" +
           reader_block;
}

} // namespace

TEST(MLDPPVXSControllerReaderLifecycleTest, AutoStopsAfterSingleOneShotReaderCompletes)
{
    mldp_pvxs_driver::writer::TestLifecycleCaptureWriter::reset();

    auto controller = MLDPPVXSController::create(makeConfigFromYaml(makeControllerYaml(R"yaml(
  - test-lifecycle:
      - name: oneshot-reader
        complete: true
        delay-ms: 0
)yaml")));

    controller->start();

    ASSERT_TRUE(waitForValue(mldp_pvxs_driver::writer::TestLifecycleCaptureWriter::stop_calls,
                             1,
                             std::chrono::milliseconds(5000)))
        << "Controller did not auto-stop after the last one-shot reader completed";

    EXPECT_EQ(mldp_pvxs_driver::writer::TestLifecycleCaptureWriter::start_calls.load(), 1);
}

TEST(MLDPPVXSControllerReaderLifecycleTest, WaitsForAllOneShotReadersBeforeStopping)
{
    mldp_pvxs_driver::writer::TestLifecycleCaptureWriter::reset();

    auto controller = MLDPPVXSController::create(makeConfigFromYaml(makeControllerYaml(R"yaml(
  - test-lifecycle:
      - name: fast-reader
        complete: true
        delay-ms: 0
      - name: slow-reader
        complete: true
        delay-ms: 200
)yaml")));

    controller->start();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(mldp_pvxs_driver::writer::TestLifecycleCaptureWriter::stop_calls.load(), 0)
        << "Controller stopped before all one-shot readers completed";

    ASSERT_TRUE(waitForValue(mldp_pvxs_driver::writer::TestLifecycleCaptureWriter::stop_calls,
                             1,
                             std::chrono::milliseconds(5000)))
        << "Controller did not stop after all one-shot readers completed";
}

TEST(MLDPPVXSControllerReaderLifecycleTest, DoesNotAutoStopWhileNonCompletingReaderRemains)
{
    mldp_pvxs_driver::writer::TestLifecycleCaptureWriter::reset();

    auto controller = MLDPPVXSController::create(makeConfigFromYaml(makeControllerYaml(R"yaml(
  - test-lifecycle:
      - name: oneshot-reader
        complete: true
        delay-ms: 0
      - name: persistent-reader
        complete: false
        delay-ms: 0
)yaml")));

    controller->start();

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(mldp_pvxs_driver::writer::TestLifecycleCaptureWriter::stop_calls.load(), 0)
        << "Controller auto-stopped even though one reader never signaled completion";

    controller->stop();
    EXPECT_TRUE(waitForValue(mldp_pvxs_driver::writer::TestLifecycleCaptureWriter::stop_calls,
                             1,
                             std::chrono::milliseconds(2000)));
}

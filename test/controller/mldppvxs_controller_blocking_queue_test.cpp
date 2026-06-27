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
#include <condition_variable>
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

class TestQueueReader final : public Reader
{
    REGISTER_READER("test-queue", TestQueueReader)

public:
    TestQueueReader(std::shared_ptr<util::bus::IDataBus> bus,
                    std::shared_ptr<metrics::Metrics>    metrics,
                    const config::Config&                cfg)
        : Reader(std::move(bus), std::move(metrics))
        , name_(cfg.get("name", "test-queue-reader"))
        , batch_count_(cfg.getInt("batch-count", 10))
        , delay_between_ms_(cfg.getInt("delay-between-ms", 0))
        , complete_(cfg.getBool("complete", true))
    {
        worker_ = std::thread([this] { run(); });
    }

    ~TestQueueReader() override
    {
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

    std::string name() const override { return name_; }

    static std::atomic<int>& pushSuccessCount() { static std::atomic<int> c{0}; return c; }
    static std::atomic<int>& pushFailCount() { static std::atomic<int> c{0}; return c; }

    static void resetCounters()
    {
        pushSuccessCount().store(0);
        pushFailCount().store(0);
    }

private:
    void run()
    {
        for (int i = 0; i < batch_count_; ++i)
        {
            if (delay_between_ms_ > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_between_ms_));
            }

            ConfigurationPayload payload;
            payload.root_source_name = name_ + "-src-" + std::to_string(i);
            payload.configuration_name = name_ + "-cfg-" + std::to_string(i);
            payload.category = "test";

            IDataBus::EventBatch batch;
            batch.reader_name = name_;
            batch.payload = std::move(payload);

            if (bus_)
            {
                bool ok = bus_->push(std::move(batch));
                if (ok)
                    pushSuccessCount().fetch_add(1, std::memory_order_relaxed);
                else
                    pushFailCount().fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (complete_)
        {
            signalCompleted();
        }
    }

    std::string name_;
    int         batch_count_;
    int         delay_between_ms_;
    bool        complete_;
    std::thread worker_;
};

} // namespace mldp_pvxs_driver::reader

namespace mldp_pvxs_driver::writer {

class TestQueueCaptureWriter final : public IWriter
{
    REGISTER_WRITER("test-queue-capture", TestQueueCaptureWriter)

public:
    explicit TestQueueCaptureWriter(const config::Config&             cfg,
                                    std::shared_ptr<metrics::Metrics> /*metrics*/ = nullptr)
        : name_(cfg.get("name", "test-queue-capture-writer"))
        , push_delay_ms_(cfg.getInt("push-delay-ms", 0))
    {
    }

    std::string name() const override { return name_; }
    void start() override {}
    void stop() noexcept override {}

    bool push(util::bus::IDataBus::EventBatch batch) noexcept override
    {
        if (push_delay_ms_ > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(push_delay_ms_));
        }

        std::lock_guard<std::mutex> lock(mu_);
        received_batches_.push_back(std::move(batch));
        return true;
    }

    static std::mutex                                    mu_;
    static std::vector<util::bus::IDataBus::EventBatch>  received_batches_;

    static void reset()
    {
        std::lock_guard<std::mutex> lock(mu_);
        received_batches_.clear();
    }

    static std::size_t receivedCount()
    {
        std::lock_guard<std::mutex> lock(mu_);
        return received_batches_.size();
    }

private:
    std::string name_;
    int         push_delay_ms_;
};

std::mutex                                    TestQueueCaptureWriter::mu_;
std::vector<util::bus::IDataBus::EventBatch>  TestQueueCaptureWriter::received_batches_;

} // namespace mldp_pvxs_driver::writer

namespace {

std::string makeQueueTestYaml(const std::string& reader_block,
                              int queue_capacity = 4096,
                              int push_timeout_ms = 5000,
                              int writer_push_delay_ms = 0)
{
    return "name: queue-test-controller\n"
           "queue_capacity: " + std::to_string(queue_capacity) + "\n"
           "push_timeout_ms: " + std::to_string(push_timeout_ms) + "\n"
           "writer:\n"
           "  test-queue-capture:\n"
           "    - name: queue-capture\n"
           "      push-delay-ms: " + std::to_string(writer_push_delay_ms) + "\n"
           "reader:\n" +
           reader_block;
}

bool waitForReceived(std::size_t target, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (mldp_pvxs_driver::writer::TestQueueCaptureWriter::receivedCount() >= target)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return mldp_pvxs_driver::writer::TestQueueCaptureWriter::receivedCount() >= target;
}

} // namespace

class MLDPPVXSControllerBlockingQueueTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        mldp_pvxs_driver::reader::TestQueueReader::resetCounters();
        mldp_pvxs_driver::writer::TestQueueCaptureWriter::reset();
    }
};

TEST_F(MLDPPVXSControllerBlockingQueueTest, AllBatchesDeliveredUnderNormalLoad)
{
    constexpr int batch_count = 20;

    auto controller = MLDPPVXSController::create(makeConfigFromYaml(makeQueueTestYaml(
        R"yaml(
  - test-queue:
      - name: normal-reader
        batch-count: 20
        delay-between-ms: 5
        complete: true
)yaml",
        4096, 5000, 0)));

    controller->start();

    ASSERT_TRUE(waitForReceived(batch_count, std::chrono::milliseconds(5000)))
        << "Expected " << batch_count << " batches, got "
        << mldp_pvxs_driver::writer::TestQueueCaptureWriter::receivedCount();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(mldp_pvxs_driver::reader::TestQueueReader::pushSuccessCount().load(), batch_count);
    EXPECT_EQ(mldp_pvxs_driver::reader::TestQueueReader::pushFailCount().load(), 0);
}

TEST_F(MLDPPVXSControllerBlockingQueueTest, PushBlocksWhenQueueFull)
{
    constexpr int batch_count = 10;
    constexpr int queue_capacity = 2;
    constexpr int writer_delay_ms = 50;

    auto controller = MLDPPVXSController::create(makeConfigFromYaml(makeQueueTestYaml(
        R"yaml(
  - test-queue:
      - name: fast-producer
        batch-count: 10
        delay-between-ms: 0
        complete: true
)yaml",
        queue_capacity, 5000, writer_delay_ms)));

    controller->start();

    ASSERT_TRUE(waitForReceived(batch_count, std::chrono::milliseconds(10000)))
        << "Expected all " << batch_count << " batches delivered (blocking backpressure), got "
        << mldp_pvxs_driver::writer::TestQueueCaptureWriter::receivedCount();

    EXPECT_EQ(mldp_pvxs_driver::reader::TestQueueReader::pushSuccessCount().load(), batch_count);
    EXPECT_EQ(mldp_pvxs_driver::reader::TestQueueReader::pushFailCount().load(), 0);
}

TEST_F(MLDPPVXSControllerBlockingQueueTest, PushDropsOnTimeout)
{
    constexpr int batch_count = 20;
    constexpr int queue_capacity = 2;
    constexpr int push_timeout_ms = 50;
    constexpr int writer_delay_ms = 200;

    auto controller = MLDPPVXSController::create(makeConfigFromYaml(makeQueueTestYaml(
        R"yaml(
  - test-queue:
      - name: overflow-producer
        batch-count: 20
        delay-between-ms: 0
        complete: true
)yaml",
        queue_capacity, push_timeout_ms, writer_delay_ms)));

    controller->start();

    std::this_thread::sleep_for(std::chrono::milliseconds(3000));

    int successes = mldp_pvxs_driver::reader::TestQueueReader::pushSuccessCount().load();
    int failures = mldp_pvxs_driver::reader::TestQueueReader::pushFailCount().load();

    EXPECT_EQ(successes + failures, batch_count);
    EXPECT_GT(failures, 0) << "Expected some pushes to be dropped due to timeout";
    EXPECT_GT(successes, 0) << "Expected some pushes to succeed";

    controller->stop();

    auto received = mldp_pvxs_driver::writer::TestQueueCaptureWriter::receivedCount();
    EXPECT_EQ(received, static_cast<std::size_t>(successes))
        << "Writer should receive exactly the number of accepted pushes";
}

TEST_F(MLDPPVXSControllerBlockingQueueTest, PushReturnsFalseAfterStop)
{
    auto controller = MLDPPVXSController::create(makeConfigFromYaml(makeQueueTestYaml(
        R"yaml(
  - test-queue:
      - name: delayed-reader
        batch-count: 5
        delay-between-ms: 200
        complete: false
)yaml",
        4096, 5000, 0)));

    controller->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    controller->stop();

    int failures = mldp_pvxs_driver::reader::TestQueueReader::pushFailCount().load();
    EXPECT_GT(failures, 0) << "Pushes after stop should return false";
}

TEST_F(MLDPPVXSControllerBlockingQueueTest, ForceStopTerminatesWithoutDrain)
{
    constexpr int batch_count = 50;
    constexpr int writer_delay_ms = 100;

    auto controller = MLDPPVXSController::create(makeConfigFromYaml(makeQueueTestYaml(
        R"yaml(
  - test-queue:
      - name: slow-drain-reader
        batch-count: 50
        delay-between-ms: 0
        complete: false
)yaml",
        4096, 5000, writer_delay_ms)));

    controller->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto before_force = std::chrono::steady_clock::now();
    controller->forceStop();
    controller->stop();
    auto elapsed = std::chrono::steady_clock::now() - before_force;

    auto received = mldp_pvxs_driver::writer::TestQueueCaptureWriter::receivedCount();
    EXPECT_LT(received, static_cast<std::size_t>(batch_count))
        << "forceStop should discard queued items, not drain all " << batch_count;

    EXPECT_LT(elapsed, std::chrono::milliseconds(2000))
        << "forceStop+stop should return quickly without draining";
}

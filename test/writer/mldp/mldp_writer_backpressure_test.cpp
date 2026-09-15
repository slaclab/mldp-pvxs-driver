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

#include <writer/WriterFactory.h>
#include <writer/mldp/MLDPWriter.h>
#include <writer/mldp/MLDPWriterConfig.h>
#include <util/bus/IDataBus.h>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <ingestion.grpc.pb.h>

#include "../../config/test_config_helpers.h"

#include <atomic>
#include <chrono>
#include <future>
#include <sstream>
#include <thread>
#include <vector>

using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::util::bus;
using mldp_pvxs_driver::config::makeConfigFromYaml;

namespace {

// Slow ingestion service: introduces configurable delay per message to simulate
// a slow consumer. This causes the MLDP writer's internal queue to fill up.
class SlowIngestionService final
    : public dp::service::ingestion::DpIngestionService::Service
{
public:
    std::atomic<int>                    consumedCount{0};
    std::chrono::milliseconds           perMessageDelay{50};

    grpc::Status registerProvider(
        grpc::ServerContext*,
        const dp::service::ingestion::RegisterProviderRequest*,
        dp::service::ingestion::RegisterProviderResponse* response) override
    {
        auto* result = response->mutable_registrationresult();
        result->set_providerid("bp-test-provider-id");
        result->set_providername("bp-test-provider");
        result->set_isnewprovider(true);
        return grpc::Status::OK;
    }

    grpc::Status ingestDataStream(
        grpc::ServerContext*,
        grpc::ServerReader<dp::service::ingestion::IngestDataRequest>* reader,
        dp::service::ingestion::IngestDataStreamResponse*) override
    {
        dp::service::ingestion::IngestDataRequest request;
        while (reader->Read(&request))
        {
            std::this_thread::sleep_for(perMessageDelay);
            consumedCount.fetch_add(1, std::memory_order_relaxed);
        }
        return grpc::Status::OK;
    }
};

bool waitForCount(std::atomic<int>& counter, int target, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (counter.load(std::memory_order_relaxed) >= target)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return counter.load(std::memory_order_relaxed) >= target;
}

IDataBus::EventBatch makeBatch(int index)
{
    DataBatch frame;
    frame.timestamps.push_back({static_cast<uint64_t>(1700000000 + index), 0});
    DataColumn col;
    col.name   = "SIGNAL";
    col.values = std::vector<double>{static_cast<double>(index)};
    frame.columns.push_back(std::move(col));
    IDataBus::EventBatch batch;
    TimeSeriesPayload ts;
    ts.root_source_name = "BP:MLDP:PV";
    ts.frames.push_back(std::move(frame));
    batch.payload = std::move(ts);
    return batch;
}

} // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Backpressure blocks push() when queue full (slow consumer). All items
// eventually get through — no data dropped.
TEST(MLDPWriterBackpressureTest, BlockingPushNoDataLoss)
{
    SlowIngestionService service;
    service.perMessageDelay = std::chrono::milliseconds(20);

    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    ASSERT_TRUE(server);
    ASSERT_GT(port, 0);

    constexpr int kTotalBatches  = 50;
    constexpr int kQueueCapacity = 4;

    std::ostringstream yaml;
    yaml << "name: mldp_bp_test\n"
         << "thread-pool: 1\n"
         << "queue-capacity: " << kQueueCapacity << "\n"
         << "mldp-pool:\n"
         << "  provider-name: bp-test-provider\n"
         << "  ingestion-url: 127.0.0.1:" << port << "\n"
         << "  query-url: localhost:" << port << "\n"
         << "  min-conn: 1\n"
         << "  max-conn: 1\n";

    const auto cfg = makeConfigFromYaml(yaml.str());
    ASSERT_TRUE(cfg.valid());

    auto writer = WriterFactory::create("mldp", cfg, nullptr);
    ASSERT_TRUE(writer);
    writer->start();

    std::atomic<int> pushSuccessCount{0};

    auto producer = std::async(std::launch::async, [&]() {
        for (int i = 0; i < kTotalBatches; ++i)
        {
            bool ok = writer->push(makeBatch(i));
            EXPECT_TRUE(ok) << "push(" << i << ") must not fail with backpressure";
            if (ok) pushSuccessCount.fetch_add(1, std::memory_order_relaxed);
        }
    });

    producer.get();

    // Wait for slow consumer to finish processing all items
    ASSERT_TRUE(waitForCount(service.consumedCount, kTotalBatches, std::chrono::seconds(30)))
        << "Consumer only processed " << service.consumedCount.load()
        << "/" << kTotalBatches << " items";

    writer->stop();
    server->Shutdown();

    EXPECT_EQ(pushSuccessCount.load(), kTotalBatches)
        << "All pushes must succeed — backpressure blocks, never drops";
    EXPECT_EQ(service.consumedCount.load(), kTotalBatches)
        << "Consumer must receive all items — no data loss";
}

// Concurrent producers with backpressure — all items delivered, no drops.
TEST(MLDPWriterBackpressureTest, ConcurrentProducersNoDataLoss)
{
    SlowIngestionService service;
    service.perMessageDelay = std::chrono::milliseconds(10);

    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    ASSERT_TRUE(server);
    ASSERT_GT(port, 0);

    constexpr int kProducers      = 4;
    constexpr int kBatchesPerProd = 15;
    constexpr int kTotalBatches   = kProducers * kBatchesPerProd;
    constexpr int kQueueCapacity  = 4;

    std::ostringstream yaml;
    yaml << "name: mldp_bp_concurrent_test\n"
         << "thread-pool: 2\n"
         << "queue-capacity: " << kQueueCapacity << "\n"
         << "mldp-pool:\n"
         << "  provider-name: bp-concurrent-provider\n"
         << "  ingestion-url: 127.0.0.1:" << port << "\n"
         << "  query-url: localhost:" << port << "\n"
         << "  min-conn: 1\n"
         << "  max-conn: 2\n";

    const auto cfg = makeConfigFromYaml(yaml.str());
    ASSERT_TRUE(cfg.valid());

    auto writer = WriterFactory::create("mldp", cfg, nullptr);
    ASSERT_TRUE(writer);
    writer->start();

    std::atomic<int> totalPushed{0};
    std::vector<std::future<void>> futures;

    for (int p = 0; p < kProducers; ++p)
    {
        futures.push_back(std::async(std::launch::async, [&, p]() {
            for (int i = 0; i < kBatchesPerProd; ++i)
            {
                bool ok = writer->push(makeBatch(p * kBatchesPerProd + i));
                EXPECT_TRUE(ok) << "Producer " << p << " push " << i << " failed";
                if (ok) totalPushed.fetch_add(1, std::memory_order_relaxed);
            }
        }));
    }

    for (auto& f : futures) f.get();

    ASSERT_TRUE(waitForCount(service.consumedCount, kTotalBatches, std::chrono::seconds(30)))
        << "Consumer only processed " << service.consumedCount.load()
        << "/" << kTotalBatches;

    writer->stop();
    server->Shutdown();

    EXPECT_EQ(totalPushed.load(), kTotalBatches);
    EXPECT_EQ(service.consumedCount.load(), kTotalBatches);
}

// High-volume backpressure: push 500 items with tiny queue, all must arrive.
TEST(MLDPWriterBackpressureTest, HighVolumeAllDataMigrated)
{
    SlowIngestionService service;
    service.perMessageDelay = std::chrono::milliseconds(5);

    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    ASSERT_TRUE(server);
    ASSERT_GT(port, 0);

    constexpr int kTotalBatches  = 500;
    constexpr int kQueueCapacity = 8;

    std::ostringstream yaml;
    yaml << "name: mldp_bp_highvol_test\n"
         << "thread-pool: 2\n"
         << "queue-capacity: " << kQueueCapacity << "\n"
         << "mldp-pool:\n"
         << "  provider-name: bp-highvol-provider\n"
         << "  ingestion-url: 127.0.0.1:" << port << "\n"
         << "  query-url: localhost:" << port << "\n"
         << "  min-conn: 1\n"
         << "  max-conn: 2\n";

    const auto cfg = makeConfigFromYaml(yaml.str());
    ASSERT_TRUE(cfg.valid());

    auto writer = WriterFactory::create("mldp", cfg, nullptr);
    ASSERT_TRUE(writer);
    writer->start();

    std::atomic<int> pushSuccessCount{0};

    // 4 producers each pushing 125 batches = 500 total
    constexpr int kProducers      = 4;
    constexpr int kBatchesPerProd = kTotalBatches / kProducers;
    std::vector<std::future<void>> futures;

    for (int p = 0; p < kProducers; ++p)
    {
        futures.push_back(std::async(std::launch::async, [&, p]() {
            for (int i = 0; i < kBatchesPerProd; ++i)
            {
                bool ok = writer->push(makeBatch(p * kBatchesPerProd + i));
                EXPECT_TRUE(ok) << "Producer " << p << " push " << i << " failed";
                if (ok) pushSuccessCount.fetch_add(1, std::memory_order_relaxed);
            }
        }));
    }

    for (auto& f : futures) f.get();

    EXPECT_EQ(pushSuccessCount.load(), kTotalBatches)
        << "All pushes must succeed with backpressure blocking";

    // Wait for all items to be consumed by the server
    ASSERT_TRUE(waitForCount(service.consumedCount, kTotalBatches, std::chrono::seconds(60)))
        << "Consumer only processed " << service.consumedCount.load()
        << "/" << kTotalBatches << " — data was lost";

    writer->stop();
    server->Shutdown();

    EXPECT_EQ(service.consumedCount.load(), kTotalBatches)
        << "All " << kTotalBatches << " items must migrate to MLDP — zero loss";
}

// push() returns false immediately after forceStop().
TEST(MLDPWriterBackpressureTest, PushUnblocksOnStop)
{
    SlowIngestionService service;

    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    ASSERT_TRUE(server);
    ASSERT_GT(port, 0);

    std::ostringstream yaml;
    yaml << "name: mldp_bp_stop_test\n"
         << "thread-pool: 1\n"
         << "queue-capacity: 2\n"
         << "mldp-pool:\n"
         << "  provider-name: bp-stop-provider\n"
         << "  ingestion-url: 127.0.0.1:" << port << "\n"
         << "  query-url: localhost:" << port << "\n"
         << "  min-conn: 1\n"
         << "  max-conn: 1\n";

    const auto cfg = makeConfigFromYaml(yaml.str());
    ASSERT_TRUE(cfg.valid());

    auto writer = WriterFactory::create("mldp", cfg, nullptr);
    ASSERT_TRUE(writer);
    writer->start();

    writer->forceStop();

    bool result = writer->push(makeBatch(0));
    EXPECT_FALSE(result) << "push() must return false after forceStop()";

    writer->stop();
    server->Shutdown();
}

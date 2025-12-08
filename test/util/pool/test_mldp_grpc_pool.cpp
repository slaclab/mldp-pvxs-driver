#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>
#include <metrics/Metrics.h>
#include <metrics/MetricsConfig.h>
#include <pool/MLDPGrpcPool.h>

#include "../../config/test_config_helpers.h"

#include <atomic>
#include <future>
#include <sstream>
#include <thread>

using namespace mldp_pvxs_driver::util::pool;
using mldp_pvxs_driver::config::makeConfigFromYaml;


static MLDPGrpcPoolConfig make_pool_config(int min_conn, int max_conn)
{
    std::ostringstream yaml;
    yaml << "provider_name: test_provider\n"
         << "url: dp-ingestion:50051\n"
         << "min_conn: " << min_conn << "\n"
         << "max_conn: " << max_conn << "\n";
    return MLDPGrpcPoolConfig(makeConfigFromYaml(yaml.str()));
}

TEST(MLDPGrpcPoolTest, AcquireBlocksUntilReleased)
{
    // Scope: Verifies that when the pool has a single object (min=max=1),
    // a second caller blocks in `acquire()` until the first holder
    // releases the object. This ensures the pool enforces exclusive use
    // of pooled objects and that `acquire()` correctly waits for
    // availability.

    // Note: the test uses real grpc::Channel objects created with
    // InsecureChannelCredentials but does not depend on a server being
    // available; we only check acquisition/release semantics and pointer
    // identities.

    auto pool = MLDPGrpcPool::create(make_pool_config(1, 1));

    // Acquire first handle
    auto h1 = pool->acquire();
    ASSERT_TRUE(h1);

    std::atomic<bool>  acquired{false};
    std::promise<void> releaseSignal;

    auto fut = std::async(std::launch::async, [&]()
                          {
                              auto h2 = pool->acquire();
                              acquired.store(true);
                              // hold h2 until signaled
                              releaseSignal.get_future().wait();
                              return;
                          });

    // short wait: second acquire should be blocked while h1 holds the object
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(acquired.load());

    // release first handle
    h1.reset();

    // Wait until we observe the pool reports an available object (release observed),
    // or the background thread has acquired it. Poll with timeout to avoid hangs.
    bool releasedSeen = false;
    for (int i = 0; i < 50; ++i)
    {
        if (pool->available() > 0)
        {
            releasedSeen = true;
            break;
        }
        if (acquired.load())
        {
            releasedSeen = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(releasedSeen) << "Expected pool to report available object after release";

    // wait for thread to acquire (if it hasn't yet)
    for (int i = 0; i < 50 && !acquired.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    EXPECT_TRUE(acquired.load());

    // signal thread to release and join
    releaseSignal.set_value();
    fut.get();
}

TEST(MLDPGrpcPoolTest, MultipleObjectsHaveSeparateChannels)
{
    // Scope: Verifies that when the pool is allowed to grow (max_size=2),
    // two concurrently-acquired pooled objects each own distinct
    // `grpc::Channel` instances. This confirms the pool creates separate
    // transport connections for separate pooled objects (transport
    // isolation).

    // Note: we only compare channel pointer identities; the test does not
    // require a live gRPC server.
    auto pool = MLDPGrpcPool::create(make_pool_config(1, 2));
    {
        auto h1 = pool->acquire();
        ASSERT_TRUE(h1);

        auto h2 = pool->acquire();
        ASSERT_TRUE(h2);

        // Channels should be different objects (separate connections)
        EXPECT_NE(h1->channel.get(), h2->channel.get());

        // avalable must be 0 now
        EXPECT_EQ(pool->available(), 0u);
    }
    // cleanup: handles go out of scope and return to pool
    // available must be 2 now
    EXPECT_EQ(pool->available(), 2u);
}

TEST(MLDPGrpcPoolTest, UpdatesMetricsWhenConnectionsMove)
{
    auto metrics = std::make_shared<mldp_pvxs_driver::metrics::Metrics>(mldp_pvxs_driver::metrics::MetricsConfig());
    auto pool = MLDPGrpcPool::create(make_pool_config(1, 1), metrics);

    EXPECT_DOUBLE_EQ(metrics->poolConnectionsAvailable(), 1.0);
    EXPECT_DOUBLE_EQ(metrics->poolConnectionsInUse(), 0.0);

    {
        auto handle = pool->acquire();
        ASSERT_TRUE(handle);
        EXPECT_DOUBLE_EQ(metrics->poolConnectionsAvailable(), 0.0);
        EXPECT_DOUBLE_EQ(metrics->poolConnectionsInUse(), 1.0);

        // send fake data
        dp::service::ingestion::IngestDataRequest request;
        request.set_providerid("test-provider");
        request.set_clientrequestid("req-123");
        request.add_tags("pv1");
        auto* frame = request.mutable_ingestiondataframe();
        auto* column = frame->add_datacolumns();
        column->set_name("pv1");
        auto* value = column->add_datavalues();
        value->set_intvalue(42);

        {
            grpc::ClientContext                        context;
            dp::service::ingestion::IngestDataResponse response;
            const auto                                 status = handle->stub->ingestData(&context, request, &response);
            EXPECT_TRUE(status.ok());
        }
    }

    EXPECT_DOUBLE_EQ(metrics->poolConnectionsAvailable(), 1.0);
    EXPECT_DOUBLE_EQ(metrics->poolConnectionsInUse(), 0.0);
}

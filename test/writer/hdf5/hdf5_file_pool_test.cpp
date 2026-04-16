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

#ifdef MLDP_PVXS_HDF5_ENABLED

    #include <writer/hdf5/HDF5FilePool.h>
    #include <writer/hdf5/HDF5WriterConfig.h>

    #include <H5Cpp.h>

    #include <chrono>
    #include <filesystem>
    #include <thread>
    #include <vector>

using namespace mldp_pvxs_driver::writer;

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
class HDF5FilePoolTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        tempDir_ = std::filesystem::temp_directory_path() / "hdf5_file_pool_test_hdf5pool";
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override { std::filesystem::remove_all(tempDir_); }

    /// Build a config pointing at tempDir_.
    HDF5WriterConfig makeConfig(std::chrono::seconds maxFileAge    = std::chrono::seconds{3600},
                                uint64_t             maxFileSizeMB = 512)
    {
        HDF5WriterConfig cfg;
        cfg.basePath      = tempDir_.string();
        cfg.name          = "test_pool";
        cfg.maxFileAge    = maxFileAge;
        cfg.maxFileSizeMB = maxFileSizeMB;
        return cfg;
    }

    std::filesystem::path tempDir_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

/// acquire() must return a non-null entry whose file exists on disk as a .h5.
TEST_F(HDF5FilePoolTest, AcquireCreatesFileOnDisk)
{
    HDF5FilePool pool(makeConfig());
    auto         entry = pool.acquire("SRC:PV:1");

    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(std::filesystem::exists(entry->path));
    EXPECT_EQ(entry->path.extension().string(), ".h5");
}

/// Two successive acquire() calls for the same source return the same entry (same path).
TEST_F(HDF5FilePoolTest, AcquireReturnsSameEntryForSameSource)
{
    HDF5FilePool pool(makeConfig());
    auto         entry1 = pool.acquire("SRC:PV:1");
    auto         entry2 = pool.acquire("SRC:PV:1");

    ASSERT_NE(entry1, nullptr);
    ASSERT_NE(entry2, nullptr);
    EXPECT_EQ(entry1->path, entry2->path);
}

/// acquire() for different sources must produce different file paths.
TEST_F(HDF5FilePoolTest, AcquireDifferentEntriesForDifferentSources)
{
    HDF5FilePool pool(makeConfig());
    auto         entryA = pool.acquire("SRC:A");
    auto         entryB = pool.acquire("SRC:B");

    ASSERT_NE(entryA, nullptr);
    ASSERT_NE(entryB, nullptr);
    EXPECT_NE(entryA->path, entryB->path);
}

/// recordWrite() increments bytesWritten on the entry.
TEST_F(HDF5FilePoolTest, RecordWriteUpdatesBytesWritten)
{
    const std::string sourceName = "SRC:PV:1";
    HDF5FilePool      pool(makeConfig());

    pool.acquire(sourceName);
    pool.recordWrite(sourceName, 1024);

    auto entry = pool.acquire(sourceName);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->bytesWritten, 1024u);
}

/// When bytesWritten exceeds maxFileSizeMB the next acquire() rotates to a new file.
TEST_F(HDF5FilePoolTest, RotatesFileWhenSizeExceeded)
{
    const std::string sourceName = "SRC:PV:1";
    // 1 MiB limit
    HDF5FilePool pool(makeConfig(std::chrono::seconds{3600}, 1));

    auto firstEntry = pool.acquire(sourceName);
    ASSERT_NE(firstEntry, nullptr);

    // Record more than 1 MiB
    pool.recordWrite(sourceName, 2u * 1024u * 1024u);

    auto secondEntry = pool.acquire(sourceName);
    ASSERT_NE(secondEntry, nullptr);
    // Rotation proof: new entry resets bytesWritten to 0.
    // Do NOT compare paths — nowUtcFileSuffix() has 1-second resolution, so both
    // files can get the same timestamp within a fast test run.
    EXPECT_EQ(secondEntry->bytesWritten, 0u) << "Expected rotation — new entry must have bytesWritten=0";
    EXPECT_NE(secondEntry.get(), firstEntry.get()) << "Expected new FileEntry pointer after rotation";
}

/// When file age exceeds maxFileAge the next acquire() rotates to a new file.
TEST_F(HDF5FilePoolTest, RotatesFileWhenAgeExceeded)
{
    const std::string sourceName = "SRC:PV:1";
    // Zero-second max age — file is immediately stale on next acquire.
    HDF5FilePool pool(makeConfig(std::chrono::seconds{0}, 512));

    auto firstEntry = pool.acquire(sourceName);
    ASSERT_NE(firstEntry, nullptr);

    // Give steady_clock at least one tick so age > 0s
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    auto secondEntry = pool.acquire(sourceName);
    ASSERT_NE(secondEntry, nullptr);
    // Rotation proof: new entry resets bytesWritten to 0.
    // Do NOT compare paths — nowUtcFileSuffix() has 1-second resolution so both
    // files can get the same timestamp within a fast test run.
    EXPECT_EQ(secondEntry->bytesWritten, 0u) << "Expected rotation — new entry must have bytesWritten=0";
    EXPECT_NE(secondEntry.get(), firstEntry.get()) << "Expected new FileEntry pointer after rotation";
}

/// flushAll() must not throw even when files are open.
TEST_F(HDF5FilePoolTest, FlushAllDoesNotThrow)
{
    HDF5FilePool pool(makeConfig());
    pool.acquire("SRC:PV:1");
    EXPECT_NO_THROW(pool.flushAll());
}

/// closeAll() must not throw even when files are open.
TEST_F(HDF5FilePoolTest, CloseAllDoesNotThrow)
{
    HDF5FilePool pool(makeConfig());
    pool.acquire("SRC:PV:1");
    EXPECT_NO_THROW(pool.closeAll());
}

/// Smoke test: concurrent acquire+recordWrite from multiple threads must not crash.
TEST_F(HDF5FilePoolTest, ThreadSafetySmoke)
{
    HDF5FilePool pool(makeConfig());

    constexpr int kThreads    = 4;
    constexpr int kIterations = 10;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    ASSERT_NO_THROW({
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&pool, t]() {
                const std::string src = "src_" + std::to_string(t);
                for (int i = 0; i < kIterations; ++i) {
                    auto entry = pool.acquire(src);
                    if (entry) {
                        pool.recordWrite(src, 128);
                    }
                }
            });
        }
        for (auto& th : threads) { th.join(); }
    });
}

#endif // MLDP_PVXS_HDF5_ENABLED

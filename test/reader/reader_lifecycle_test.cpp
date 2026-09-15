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

#include <reader/IReader.h>
#include <reader/IReaderLifecycle.h>

#include <memory>
#include <string>

namespace mldp_pvxs_driver::test {

class MockLifecycleObserver final : public reader::IReaderLifecycle
{
public:
    void onReaderCompleted(const std::string& reader_name) override
    {
        last_completed_name_ = reader_name;
        ++completed_count_;
    }

    const std::string& lastCompletedName() const { return last_completed_name_; }
    int                completedCount() const { return completed_count_; }

private:
    std::string last_completed_name_;
    int         completed_count_{0};
};

class TestReader final : public reader::Reader
{
public:
    using reader::Reader::Reader;

    std::string name() const override { return "test-reader"; }

    void simulateWorkDone() { signalCompleted(); }
};

TEST(ReaderLifecycleTest, SignalCompletedNotifiesObserver)
{
    auto observer = std::make_shared<MockLifecycleObserver>();
    auto reader = std::make_unique<TestReader>(nullptr, nullptr);
    reader->setLifecycleObserver(observer);

    reader->simulateWorkDone();

    EXPECT_EQ(observer->completedCount(), 1);
    EXPECT_EQ(observer->lastCompletedName(), "test-reader");
}

TEST(ReaderLifecycleTest, SignalCompletedWithoutObserverDoesNotCrash)
{
    auto reader = std::make_unique<TestReader>(nullptr, nullptr);
    EXPECT_NO_THROW(reader->simulateWorkDone());
}

TEST(ReaderLifecycleTest, SignalCompletedWithExpiredObserverDoesNotCrash)
{
    auto reader = std::make_unique<TestReader>(nullptr, nullptr);
    {
        auto observer = std::make_shared<MockLifecycleObserver>();
        reader->setLifecycleObserver(observer);
    }

    EXPECT_NO_THROW(reader->simulateWorkDone());
}

} // namespace mldp_pvxs_driver::test

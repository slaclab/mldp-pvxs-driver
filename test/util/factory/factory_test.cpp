//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <util/factory/Factory.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Self-contained types for testing
// ---------------------------------------------------------------------------

struct TestProduct {
    virtual ~TestProduct() = default;
    virtual std::string name() const = 0;
};

struct FooProduct : TestProduct {
    std::string name() const override { return "foo"; }
};

struct BarProduct : TestProduct {
    std::string name() const override { return "bar"; }
};

class TestFactory
    : public mldp_pvxs_driver::util::factory::Factory<TestFactory, TestProduct>
{
public:
    static constexpr std::string_view kTypeName = "test";
};

// ---------------------------------------------------------------------------
// Static registrations — Meyers-singleton registry is already alive by the
// time these objects are constructed, so ordering is safe.
// ---------------------------------------------------------------------------
namespace {
    mldp_pvxs_driver::util::factory::FactoryRegistrator<TestFactory, FooProduct> reg_foo{"foo"};
    mldp_pvxs_driver::util::factory::FactoryRegistrator<TestFactory, BarProduct> reg_bar{"bar"};
} // namespace

// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------

TEST(FactoryTest, DispatchesToCorrectType) {
    auto foo = TestFactory::create("foo");
    ASSERT_NE(foo, nullptr);
    EXPECT_EQ(foo->name(), "foo");

    auto bar = TestFactory::create("bar");
    ASSERT_NE(bar, nullptr);
    EXPECT_EQ(bar->name(), "bar");
}

TEST(FactoryTest, UnknownTypeThrowsRuntimeError) {
    EXPECT_THROW(TestFactory::create("unknown"), std::runtime_error);

    try {
        TestFactory::create("unknown");
        FAIL() << "Expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("Unknown test type: unknown"), std::string::npos);
    }
}

TEST(FactoryTest, RegisteredTypesListsExactlyRegisteredNames) {
    auto types = TestFactory::registeredTypes();
    EXPECT_EQ(types.size(), 2u);
    std::sort(types.begin(), types.end());
    std::vector<std::string> expected{"bar", "foo"};
    EXPECT_EQ(types, expected);
}

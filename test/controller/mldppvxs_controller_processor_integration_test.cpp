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

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::controller::MLDPPVXSController;
using mldp_pvxs_driver::util::bus::DataBatch;
using mldp_pvxs_driver::util::bus::DataColumn;
using mldp_pvxs_driver::util::bus::EventBatchStruct;
using mldp_pvxs_driver::util::bus::TimeSeriesPayload;

// ---------------------------------------------------------------------------
// Test reader: pushes one pre-built batch on construction
// ---------------------------------------------------------------------------
namespace mldp_pvxs_driver::reader {

class TestPushReader final : public Reader
{
    REGISTER_READER("test-push", TestPushReader)

public:
    TestPushReader(std::shared_ptr<util::bus::IDataBus> bus,
                   std::shared_ptr<metrics::Metrics>    metrics,
                   const config::Config&                cfg)
        : Reader(std::move(bus), std::move(metrics))
        , name_(cfg.get("name", "test-push-reader"))
    {
        EventBatchStruct batch;
        batch.reader_name = name_;

        DataBatch frame;
        frame.timestamps.push_back({5, 0});
        frame.columns.push_back(DataColumn{"value", std::vector<double>{cfg.getDouble("value", 0.0)}});

        TimeSeriesPayload payload;
        payload.root_source_name = cfg.get("source", "SRC:A");
        payload.frames.push_back(std::move(frame));
        batch.payload = std::move(payload);

        last_push_ok = bus_->push(std::move(batch));
    }

    std::string name() const override { return name_; }

    static bool last_push_ok;

private:
    std::string name_;
};

bool TestPushReader::last_push_ok = false;

} // namespace mldp_pvxs_driver::reader

// ---------------------------------------------------------------------------
// Test writer: captures every received root_source_name
// ---------------------------------------------------------------------------
namespace mldp_pvxs_driver::writer {

class TestCaptureWriter final : public IWriter
{
    REGISTER_WRITER("test-capture", TestCaptureWriter)

public:
    explicit TestCaptureWriter(const config::Config&             cfg,
                               std::shared_ptr<metrics::Metrics> /*metrics*/ = nullptr)
        : name_(cfg.get("name", "test-capture-writer"))
    {
    }

    std::string name() const override { return name_; }
    void        start() override {}
    void        stop() noexcept override {}

    bool push(util::bus::IDataBus::EventBatch batch) noexcept override
    {
        const auto* ts = std::get_if<util::bus::TimeSeriesPayload>(&batch.payload);
        if (!ts) return true;

        std::lock_guard<std::mutex> lk(mutex_);
        received_sources.push_back(ts->root_source_name);
        return true;
    }

    // shared across instances via static so the test can read results
    static std::mutex              mutex_;
    static std::vector<std::string> received_sources;

private:
    std::string name_;
};

std::mutex              TestCaptureWriter::mutex_;
std::vector<std::string> TestCaptureWriter::received_sources;

} // namespace mldp_pvxs_driver::writer

// ---------------------------------------------------------------------------
// YAML helper
// ---------------------------------------------------------------------------
namespace {

std::string makeControllerYaml()
{
    return R"yaml(
name: controller-test
writer:
  test-capture:
    - name: capture-writer
reader:
  - test-push:
      - name: test-reader
        source: SRC:A
        value: 4.5
processors:
  - type: linear-transform
    name: linear-proc
    sources:
      - SRC:A
    alignment: latest-value
    trigger: any-update
    output-source: VIRTUAL:LINEAR:OUT
    coefficients:
      - 1.0
    bias: 0.0
    output-column: val
routing:
  linear-proc:
    from: [test-reader]
    include:
      - SRC:A
  capture-writer:
    from: [linear-proc]
    include:
      - VIRTUAL:LINEAR:OUT
)yaml";
}

std::string makeCollisionYaml()
{
    return R"yaml(
name: controller-test
writer:
  test-capture:
    - name: capture-writer
reader:
  - test-push:
      - name: VIRTUAL:LINEAR:OUT
        source: SRC:A
        value: 4.5
processors:
  - type: linear-transform
    name: linear-proc
    sources:
      - SRC:A
    alignment: latest-value
    trigger: any-update
    output-source: VIRTUAL:LINEAR:OUT
    coefficients:
      - 1.0
    bias: 0.0
    output-column: val
routing:
  linear-proc:
    from: [VIRTUAL:LINEAR:OUT]
    include:
      - SRC:A
  capture-writer:
    from: [linear-proc]
    include:
      - VIRTUAL:LINEAR:OUT
)yaml";
}

std::string makeAcyclicChainYaml()
{
    return R"yaml(
name: controller-test
writer:
  test-capture:
    - name: capture-writer
reader:
  - test-push:
      - name: test-reader
        source: SRC:A
        value: 4.5
processors:
  - type: linear-transform
    name: proc-a
    sources:
      - SRC:A
    alignment: latest-value
    trigger: any-update
    output-source: VIRTUAL:A
    coefficients:
      - 1.0
    bias: 0.0
    output-column: val
  - type: linear-transform
    name: proc-b
    sources:
      - VIRTUAL:A
    alignment: latest-value
    trigger: any-update
    output-source: VIRTUAL:B
    coefficients:
      - 1.0
    bias: 1.0
    output-column: val
routing:
  proc-a:
    from: [test-reader]
    include:
      - SRC:A
  proc-b:
    from: [proc-a]
    include:
      - VIRTUAL:A
  capture-writer:
    from: [proc-b]
    include:
      - VIRTUAL:B
)yaml";
}

std::string makeCyclicChainYaml()
{
    return R"yaml(
name: controller-test
writer:
  test-capture:
    - name: capture-writer
reader:
  - test-push:
      - name: test-reader
        source: SRC:SEED
        value: 4.5
processors:
  - type: linear-transform
    name: proc-a
    sources:
      - VIRTUAL:B
    alignment: latest-value
    trigger: any-update
    output-source: VIRTUAL:A
    coefficients:
      - 1.0
    bias: 0.0
    output-column: val
  - type: linear-transform
    name: proc-b
    sources:
      - VIRTUAL:A
    alignment: latest-value
    trigger: any-update
    output-source: VIRTUAL:B
    coefficients:
      - 1.0
    bias: 1.0
    output-column: val
routing:
  proc-a:
    from: [proc-b]
    include:
      - VIRTUAL:B
  proc-b:
    from: [proc-a]
    include:
      - VIRTUAL:A
  capture-writer:
    from: [proc-b]
    include:
      - VIRTUAL:B
)yaml";
}

} // namespace

// ---------------------------------------------------------------------------
// Test
// ---------------------------------------------------------------------------
TEST(MLDPPVXSControllerProcessorIntegrationTest, ProcessorOutputReachesWriter)
{
    mldp_pvxs_driver::reader::TestPushReader::last_push_ok = false;
    {
        std::lock_guard<std::mutex> lk(mldp_pvxs_driver::writer::TestCaptureWriter::mutex_);
        mldp_pvxs_driver::writer::TestCaptureWriter::received_sources.clear();
    }

    auto controller = MLDPPVXSController::create(makeConfigFromYaml(makeControllerYaml()));
    controller->start();

    ASSERT_TRUE(mldp_pvxs_driver::reader::TestPushReader::last_push_ok);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline)
    {
        {
            std::lock_guard<std::mutex> lk(mldp_pvxs_driver::writer::TestCaptureWriter::mutex_);
            const auto& src = mldp_pvxs_driver::writer::TestCaptureWriter::received_sources;
            if (std::find(src.begin(), src.end(), "VIRTUAL:LINEAR:OUT") != src.end())
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    {
        std::lock_guard<std::mutex> lk(mldp_pvxs_driver::writer::TestCaptureWriter::mutex_);
        const auto& sources = mldp_pvxs_driver::writer::TestCaptureWriter::received_sources;
        ASSERT_FALSE(sources.empty());
        EXPECT_NE(std::find(sources.begin(), sources.end(), "VIRTUAL:LINEAR:OUT"), sources.end())
            << "Expected VIRTUAL:LINEAR:OUT in received sources";
    }

    controller->stop();
}

TEST(MLDPPVXSControllerProcessorIntegrationTest, OutputSourceCollidesWithReaderNameThrows)
{
    auto controller = MLDPPVXSController::create(makeConfigFromYaml(makeCollisionYaml()));
    EXPECT_THROW(controller->start(), std::runtime_error);
}

TEST(MLDPPVXSControllerProcessorIntegrationTest, ProcessorChainNoCycleOk)
{
    auto controller = MLDPPVXSController::create(makeConfigFromYaml(makeAcyclicChainYaml()));
    EXPECT_NO_THROW(controller->start());
    controller->stop();
}

TEST(MLDPPVXSControllerProcessorIntegrationTest, ProcessorChainCycleThrows)
{
    auto controller = MLDPPVXSController::create(makeConfigFromYaml(makeCyclicChainYaml()));
    EXPECT_THROW(controller->start(), std::runtime_error);
}

#ifdef BUILD_PYTHON_PROCESSOR
TEST(MLDPPVXSControllerProcessorIntegrationTest, UsesDefaultAlgorithmsDirectoryForPythonPlugin)
{
    const auto original_path = std::filesystem::current_path();
    const auto working_path = std::filesystem::temp_directory_path() / ("proc-default-dir-" + std::to_string(std::rand()));
    std::filesystem::create_directories(working_path / "python-plugins");

    {
        std::ofstream script(working_path / "python-plugins" / "custom_compute.py");
        script << R"py(
config = {
    "name": "custom-compute",
    "sources": ["SRC:A"],
    "alignment": "latest-value",
    "trigger": "any-update",
    "output_source": "VIRTUAL:CUSTOM:OUT",
}
def compute(snapshot):
    import mldp
    return mldp.timeseries("VIRTUAL:CUSTOM:OUT", {"value": 42.0})
)py";
    }

    struct CwdGuard
    {
        std::filesystem::path path;
        ~CwdGuard()
        {
            std::error_code ec;
            std::filesystem::current_path(path, ec);
        }
    } guard{original_path};

    std::filesystem::current_path(working_path);

    {
        std::lock_guard<std::mutex> lk(mldp_pvxs_driver::writer::TestCaptureWriter::mutex_);
        mldp_pvxs_driver::writer::TestCaptureWriter::received_sources.clear();
    }

    const std::string yaml = R"yaml(
name: controller-default-algo-test
writer:
  test-capture:
    - name: capture-writer
reader:
  - test-push:
      - name: test-reader
        source: SRC:A
        value: 7.0
processors:
  custom-proc:
    type: custom_compute
routing:
  custom-compute:
    from: [test-reader]
    include:
      - SRC:A
  capture-writer:
    from: [custom-compute]
    include:
      - VIRTUAL:CUSTOM:OUT
)yaml";

    auto controller = MLDPPVXSController::create(makeConfigFromYaml(yaml));
    controller->start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline)
    {
        {
            std::lock_guard<std::mutex> lk(mldp_pvxs_driver::writer::TestCaptureWriter::mutex_);
            const auto& src = mldp_pvxs_driver::writer::TestCaptureWriter::received_sources;
            if (std::find(src.begin(), src.end(), "VIRTUAL:CUSTOM:OUT") != src.end())
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    {
        std::lock_guard<std::mutex> lk(mldp_pvxs_driver::writer::TestCaptureWriter::mutex_);
        const auto& sources = mldp_pvxs_driver::writer::TestCaptureWriter::received_sources;
        ASSERT_FALSE(sources.empty());
        EXPECT_NE(std::find(sources.begin(), sources.end(), "VIRTUAL:CUSTOM:OUT"), sources.end())
            << "Expected VIRTUAL:CUSTOM:OUT from Python plugin in default algorithms/ directory";
    }

    controller->stop();

    std::error_code ec;
    std::filesystem::remove_all(working_path, ec);
}
#endif

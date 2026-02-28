#include <gtest/gtest.h>

#include <controller/MLDPPVXSControllerConfig.h>
#include <reader/impl/epics/EpicsReaderConfig.h>

#include "test_config_helpers.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace mldp_pvxs_driver::controller {

using mldp_pvxs_driver::config::makeConfigFromYaml;

TEST(MLDPPVXSControllerConfigTest, ParsesValidConfig)
{
    const std::string yaml = R"(
controller_thread_pool: 2
controller_stream_max_bytes: 1048576
controller_stream_max_age_ms: 250
mldp_pool:
  provider_name: pvxs_provider
  url: https://mldp.example:443
  min_conn: 1
  max_conn: 4
reader:
  - epics-pvxs:
      - name: epics_1
        pvs:
          - name: pv1
            option: chan://one
          - name: pv2
metrics:
  endpoint: 0.0.0.0:9464
)";

    const auto               cfg = makeConfigFromYaml(yaml);
    MLDPPVXSControllerConfig controllerCfg(cfg);

    ASSERT_TRUE(controllerCfg.valid());
    // controller thread pool size
    EXPECT_EQ(2, controllerCfg.controllerThreadPoolSize());
    EXPECT_EQ(1048576u, controllerCfg.controllerStreamMaxBytes());
    EXPECT_EQ(std::chrono::milliseconds(250), controllerCfg.controllerStreamMaxAge());

    // pool config
    EXPECT_EQ("pvxs_provider", controllerCfg.pool().providerName());
    EXPECT_EQ(1, controllerCfg.pool().minConnections());
    EXPECT_EQ("https://mldp.example:443", controllerCfg.pool().url());
    EXPECT_EQ("https://mldp.example:443", controllerCfg.pool().queryUrl());
    EXPECT_EQ(4, controllerCfg.pool().maxConnections());

    // epics reader config
    ASSERT_EQ(1u, controllerCfg.readerConfigs().size());
    const reader::impl::epics::EpicsReaderConfig epicsReader(controllerCfg.readerConfigs().front());
    EXPECT_EQ("epics_1", epicsReader.name());
    ASSERT_EQ(2u, epicsReader.pvs().size());
    EXPECT_EQ("pv1", epicsReader.pvs()[0].name);
    EXPECT_EQ("chan://one", epicsReader.pvs()[0].option);
    EXPECT_EQ("pv2", epicsReader.pvs()[1].name);
    ASSERT_TRUE(controllerCfg.metricsConfig().has_value());

    // metrics config
    EXPECT_EQ("0.0.0.0:9464", controllerCfg.metricsConfig()->endpoint());
}

TEST(MLDPPVXSControllerConfigTest, ParsesTlsCredentialsBlock)
{
    using util::pool::MLDPGrpcPoolConfig;

    const auto tempDir = std::filesystem::temp_directory_path() / "mldp_pool_credentials_test";
    std::filesystem::create_directories(tempDir);

    const auto certPath = tempDir / "client.crt";
    const auto keyPath = tempDir / "client.key";
    const auto caPath = tempDir / "ca.crt";

    {
        std::ofstream(certPath) << "CERTDATA";
        std::ofstream(keyPath) << "KEYDATA";
        std::ofstream(caPath) << "CADATA";
    }

    std::ostringstream yaml;
    yaml << "controller_thread_pool: 1\n"
         << "mldp_pool:\n"
         << "  provider_name: pvxs_provider\n"
         << "  url: https://mldp.example:443\n"
         << "  min_conn: 1\n"
         << "  max_conn: 1\n"
         << "  credentials:\n"
         << "    pem_cert_chain: " << certPath.string() << "\n"
         << "    pem_private_key: " << keyPath.string() << "\n"
         << "    pem_root_certs: " << caPath.string() << "\n"
         << "reader: []\n";

    const auto cfg = makeConfigFromYaml(yaml.str());
    MLDPPVXSControllerConfig controllerCfg(cfg);

    const auto& creds = controllerCfg.pool().credentials();
    EXPECT_EQ(MLDPGrpcPoolConfig::Credentials::Type::Ssl, creds.type);
    EXPECT_EQ("CERTDATA", creds.ssl_options.pem_cert_chain);
    EXPECT_EQ("KEYDATA", creds.ssl_options.pem_private_key);
    EXPECT_EQ("CADATA", creds.ssl_options.pem_root_certs);

    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
}

TEST(MLDPPVXSControllerConfigTest, ParsesMultipleEpicsReaders)
{
    const std::string yaml = R"(
controller_thread_pool: 3
mldp_pool:
  provider_name: pvxs_provider
  url: https://mldp.example:443
  min_conn: 2
  max_conn: 2
reader:
  - epics-pvxs:
      - name: epics_1
        pvs:
          - name: pv1
      - name: epics_2
        pvs:
          - name: pv2
)";

    const auto               cfg = makeConfigFromYaml(yaml);
    MLDPPVXSControllerConfig controllerCfg(cfg);

    ASSERT_TRUE(controllerCfg.valid());
    EXPECT_EQ("pvxs_provider", controllerCfg.pool().providerName());
    EXPECT_EQ(3, controllerCfg.controllerThreadPoolSize());
    EXPECT_EQ(2, controllerCfg.pool().minConnections());
    ASSERT_EQ(2u, controllerCfg.readerConfigs().size());
    const reader::impl::epics::EpicsReaderConfig epicsReader0(controllerCfg.readerConfigs()[0]);
    EXPECT_EQ("epics_1", epicsReader0.name());
    ASSERT_EQ(1u, epicsReader0.pvs().size());
    EXPECT_EQ("pv1", epicsReader0.pvs()[0].name);
    const reader::impl::epics::EpicsReaderConfig epicsReader1(controllerCfg.readerConfigs()[1]);
    EXPECT_EQ("epics_2", epicsReader1.name());
    ASSERT_EQ(1u, epicsReader1.pvs().size());
    EXPECT_EQ("pv2", epicsReader1.pvs()[0].name);
}

TEST(MLDPPVXSControllerConfigTest, ParsesOptionalQueryUrl)
{
    const std::string yaml = R"(
controller_thread_pool: 1
mldp_pool:
  provider_name: pvxs_provider
  provider_description: "PVXS-based data provider"
  url: https://mldp-ingestion.example:50051
  query_url: https://mldp-query.example:50052
  min_conn: 1
  max_conn: 2
reader: []
)";

    const auto               cfg = makeConfigFromYaml(yaml);
    MLDPPVXSControllerConfig controllerCfg(cfg);

    ASSERT_TRUE(controllerCfg.valid());
    EXPECT_EQ("https://mldp-ingestion.example:50051", controllerCfg.pool().url());
    EXPECT_EQ("https://mldp-query.example:50052", controllerCfg.pool().queryUrl());
}

TEST(MLDPPVXSControllerConfigTest, ThrowsWhenProviderNameMissing)
{
    const std::string yaml = R"(
controller_thread_pool: 1
mldp_pool:
  url: https://mldp.example:443
  min_conn: 1
  max_conn: 1
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(MLDPPVXSControllerConfig(cfg)), MLDPPVXSControllerConfig::Error);
}

TEST(MLDPPVXSControllerConfigTest, ThrowsWhenMinConnMissing)
{
    const std::string yaml = R"(
controller_thread_pool: 1
mldp_pool:
  provider_name: pvxs_provider
  url: https://mldp.example:443
  max_conn: 2
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(MLDPPVXSControllerConfig(cfg)), MLDPPVXSControllerConfig::Error);
}

TEST(MLDPPVXSControllerConfigTest, ThrowsWhenMinConnExceedsMax)
{
    const std::string yaml = R"(
controller_thread_pool: 1
mldp_pool:
  provider_name: pvxs_provider
  url: https://mldp.example:443
  min_conn: 5
  max_conn: 2
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(MLDPPVXSControllerConfig(cfg)), MLDPPVXSControllerConfig::Error);
}

TEST(MLDPPVXSControllerConfigTest, ThrowsWithoutPoolConfig)
{
    const std::string yaml = R"(
controller_thread_pool: 1
reader: []
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(MLDPPVXSControllerConfig(cfg)), MLDPPVXSControllerConfig::Error);
}

TEST(MLDPPVXSControllerConfigTest, ThrowsWhenReaderIsNotSequence)
{
    const std::string yaml = R"(
controller_thread_pool: 1
mldp_pool:
  provider_name: pvxs_provider
  url: https://mldp.example
  min_conn: 1
  max_conn: 1
reader: invalid
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(MLDPPVXSControllerConfig(cfg)), MLDPPVXSControllerConfig::Error);
}

TEST(MLDPPVXSControllerConfigTest, ThrowsForUnsupportedReaderType)
{
    const std::string yaml = R"(
controller_thread_pool: 1
mldp_pool:
  provider_name: pvxs_provider
  url: https://mldp.example
  min_conn: 1
  max_conn: 1
reader:
  - foo:
      - bar
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(MLDPPVXSControllerConfig(cfg)), MLDPPVXSControllerConfig::Error);
}

TEST(MLDPPVXSControllerConfigTest, ThrowsWhenThreadPoolMissing)
{
    const std::string yaml = R"(
mldp_pool:
  provider_name: pvxs_provider
  url: https://mldp.example
  min_conn: 1
  max_conn: 1
reader: []
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(MLDPPVXSControllerConfig(cfg)), MLDPPVXSControllerConfig::Error);
}

} // namespace mldp_pvxs_driver::controller

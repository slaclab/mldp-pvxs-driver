#include <gtest/gtest.h>

#include <controller/MLDPPVXSControllerConfig.h>
#include <reader/impl/epics/shared/EpicsReaderConfig.h>
#include <writer/mldp/MLDPWriterConfig.h>

#include "test_config_helpers.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace mldp_pvxs_driver::controller {

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::writer::MLDPWriterConfig;

TEST(MLDPPVXSControllerConfigTest, ParsesValidConfig)
{
    const std::string yaml = R"(
writer:
  mldp:
    - name: mldp_main
      thread-pool: 2
      stream-max-bytes: 1048576
      stream-max-age-ms: 250
      mldp-pool:
        provider-name: pvxs_provider
        ingestion-url: https://mldp-ingestion.example:443
        query-url: https://mldp-query.example:443
        min-conn: 1
        max-conn: 4
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
    ASSERT_EQ(1u, controllerCfg.writerEntries().size());

    const auto mldpCfg = MLDPWriterConfig::parse(controllerCfg.writerEntries()[0].second);

    EXPECT_EQ("mldp_main", mldpCfg.name);
    EXPECT_EQ(2, mldpCfg.threadPoolSize);
    EXPECT_EQ(1048576u, mldpCfg.streamMaxBytes);
    EXPECT_EQ(std::chrono::milliseconds(250), mldpCfg.streamMaxAge);

    EXPECT_EQ("pvxs_provider", mldpCfg.poolConfig.providerName());
    EXPECT_EQ(1, mldpCfg.poolConfig.minConnections());
    EXPECT_EQ("https://mldp-ingestion.example:443", mldpCfg.poolConfig.ingestionUrl());
    EXPECT_EQ("https://mldp-query.example:443", mldpCfg.poolConfig.queryUrl());
    EXPECT_EQ(4, mldpCfg.poolConfig.maxConnections());

    ASSERT_EQ(1u, controllerCfg.readerConfigs().size());
    const reader::impl::epics::EpicsReaderConfig epicsReader(controllerCfg.readerConfigs().front());
    EXPECT_EQ("epics_1", epicsReader.name());
    ASSERT_EQ(2u, epicsReader.pvs().size());
    EXPECT_EQ("pv1", epicsReader.pvs()[0].name);
    EXPECT_EQ("chan://one", epicsReader.pvs()[0].option);
    EXPECT_EQ("pv2", epicsReader.pvs()[1].name);

    ASSERT_TRUE(controllerCfg.metricsConfig().has_value());
    EXPECT_EQ("0.0.0.0:9464", controllerCfg.metricsConfig()->endpoint());
}

TEST(MLDPPVXSControllerConfigTest, ParsesMultipleGrpcInstances)
{
    const std::string yaml = R"(
writer:
  mldp:
    - name: mldp_primary
      thread-pool: 2
      mldp-pool:
        provider-name: pvxs_provider
        ingestion-url: https://mldp-ingestion.example:443
        query-url: https://mldp-query.example:443
        min-conn: 1
        max-conn: 4
    - name: mldp_secondary
      thread-pool: 1
      mldp-pool:
        provider-name: pvxs_provider_backup
        ingestion-url: https://mldp-backup.example:443
        query-url: https://mldp-backup-query.example:443
        min-conn: 1
        max-conn: 2
reader: []
)";

    const auto               cfg = makeConfigFromYaml(yaml);
    MLDPPVXSControllerConfig controllerCfg(cfg);

    ASSERT_TRUE(controllerCfg.valid());
    ASSERT_EQ(2u, controllerCfg.writerEntries().size());

    const auto cfg0 = MLDPWriterConfig::parse(controllerCfg.writerEntries()[0].second);
    const auto cfg1 = MLDPWriterConfig::parse(controllerCfg.writerEntries()[1].second);

    EXPECT_EQ("mldp_primary", cfg0.name);
    EXPECT_EQ(2, cfg0.threadPoolSize);

    EXPECT_EQ("mldp_secondary", cfg1.name);
    EXPECT_EQ(1, cfg1.threadPoolSize);
}

TEST(MLDPPVXSControllerConfigTest, ParsesTlsCredentialsBlock)
{
    using util::pool::MLDPGrpcPoolConfig;

    const auto tempDir = std::filesystem::temp_directory_path() / "mldp-pool_credentials_test";
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
    yaml << "writer:\n"
         << "  mldp:\n"
         << "    - name: mldp_tls\n"
         << "      thread-pool: 1\n"
         << "      mldp-pool:\n"
         << "        provider-name: pvxs_provider\n"
         << "        ingestion-url: https://mldp-ingestion.example:443\n"
         << "        query-url: https://mldp-query.example:443\n"
         << "        min-conn: 1\n"
         << "        max-conn: 1\n"
         << "        credentials:\n"
         << "          pem-cert-chain: " << certPath.string() << "\n"
         << "          pem-private-key: " << keyPath.string() << "\n"
         << "          pem-root-certs: " << caPath.string() << "\n"
         << "reader: []\n";

    const auto               cfg = makeConfigFromYaml(yaml.str());
    MLDPPVXSControllerConfig controllerCfg(cfg);

    ASSERT_EQ(1u, controllerCfg.writerEntries().size());
    const auto mldpCfg = MLDPWriterConfig::parse(controllerCfg.writerEntries()[0].second);
    const auto& creds = mldpCfg.poolConfig.credentials();
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
writer:
  mldp:
    - name: mldp_main
      thread-pool: 3
      mldp-pool:
        provider-name: pvxs_provider
        ingestion-url: https://mldp-ingestion.example:443
        query-url: https://mldp-query.example:443
        min-conn: 2
        max-conn: 2
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
    ASSERT_EQ(1u, controllerCfg.writerEntries().size());
    const auto mldpCfg = MLDPWriterConfig::parse(controllerCfg.writerEntries()[0].second);
    EXPECT_EQ("mldp_main", mldpCfg.name);
    EXPECT_EQ("pvxs_provider", mldpCfg.poolConfig.providerName());
    EXPECT_EQ(3, mldpCfg.threadPoolSize);
    EXPECT_EQ(2, mldpCfg.poolConfig.minConnections());
    ASSERT_EQ(2u, controllerCfg.readerConfigs().size());
    const reader::impl::epics::EpicsReaderConfig epicsReader0(controllerCfg.readerConfigs()[0]);
    EXPECT_EQ("epics_1", epicsReader0.name());
    const reader::impl::epics::EpicsReaderConfig epicsReader1(controllerCfg.readerConfigs()[1]);
    EXPECT_EQ("epics_2", epicsReader1.name());
}

TEST(MLDPPVXSControllerConfigTest, ParsesOptionalQueryUrl)
{
    const std::string yaml = R"(
writer:
  mldp:
    - name: mldp_main
      thread-pool: 1
      mldp-pool:
        provider-name: pvxs_provider
        provider-description: "PVXS-based data provider"
        ingestion-url: https://mldp-ingestion.example:50051
        query-url: https://mldp-query.example:50052
        min-conn: 1
        max-conn: 2
reader: []
)";

    const auto               cfg = makeConfigFromYaml(yaml);
    MLDPPVXSControllerConfig controllerCfg(cfg);

    ASSERT_TRUE(controllerCfg.valid());
    ASSERT_EQ(1u, controllerCfg.writerEntries().size());
    const auto mldpCfg = MLDPWriterConfig::parse(controllerCfg.writerEntries()[0].second);
    const auto& pool = mldpCfg.poolConfig;
    EXPECT_EQ("https://mldp-ingestion.example:50051", pool.ingestionUrl());
    EXPECT_EQ("https://mldp-query.example:50052", pool.queryUrl());
}

TEST(MLDPPVXSControllerConfigTest, ThrowsWhenProviderNameMissing)
{
    const std::string yaml = R"(
writer:
  mldp:
    - name: mldp_main
      thread-pool: 1
      mldp-pool:
        ingestion-url: https://mldp.example:443
        min-conn: 1
        max-conn: 1
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(MLDPPVXSControllerConfig(cfg)), MLDPPVXSControllerConfig::Error);
}

TEST(MLDPPVXSControllerConfigTest, ThrowsWhenMinConnMissing)
{
    const std::string yaml = R"(
writer:
  mldp:
    - name: mldp_main
      thread-pool: 1
      mldp-pool:
        provider-name: pvxs_provider
        ingestion-url: https://mldp.example:443
        max-conn: 2
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(MLDPPVXSControllerConfig(cfg)), MLDPPVXSControllerConfig::Error);
}

TEST(MLDPPVXSControllerConfigTest, ThrowsWhenMinConnExceedsMax)
{
    const std::string yaml = R"(
writer:
  mldp:
    - name: mldp_main
      thread-pool: 1
      mldp-pool:
        provider-name: pvxs_provider
        ingestion-url: https://mldp.example:443
        min-conn: 5
        max-conn: 2
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(MLDPPVXSControllerConfig(cfg)), MLDPPVXSControllerConfig::Error);
}

TEST(MLDPPVXSControllerConfigTest, ThrowsWithoutPoolConfig)
{
    const std::string yaml = R"(
writer:
  mldp:
    - name: mldp_main
      thread-pool: 1
reader: []
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(MLDPPVXSControllerConfig(cfg)), MLDPPVXSControllerConfig::Error);
}

TEST(MLDPPVXSControllerConfigTest, ThrowsWhenReaderIsNotSequence)
{
    const std::string yaml = R"(
writer:
  mldp:
    - name: mldp_main
      thread-pool: 1
      mldp-pool:
        provider-name: pvxs_provider
        ingestion-url: https://mldp.example
        min-conn: 1
        max-conn: 1
reader: invalid
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(MLDPPVXSControllerConfig(cfg)), MLDPPVXSControllerConfig::Error);
}

TEST(MLDPPVXSControllerConfigTest, ThrowsForUnsupportedReaderType)
{
    const std::string yaml = R"(
writer:
  mldp:
    - name: mldp_main
      thread-pool: 1
      mldp-pool:
        provider-name: pvxs_provider
        ingestion-url: https://mldp.example
        min-conn: 1
        max-conn: 1
reader:
  - foo:
      - bar
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(MLDPPVXSControllerConfig(cfg)), MLDPPVXSControllerConfig::Error);
}

TEST(MLDPPVXSControllerConfigTest, ThrowsWhenWriterBlockMissing)
{
    // No writer block at all → must throw (no backward-compat path anymore)
    const std::string yaml = R"(
reader: []
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(MLDPPVXSControllerConfig(cfg)), MLDPPVXSControllerConfig::Error);
}

TEST(MLDPPVXSControllerConfigTest, ThrowsWhenGrpcInstanceMissingName)
{
    const std::string yaml = R"(
writer:
  mldp:
    - thread-pool: 1
      mldp-pool:
        provider-name: pvxs_provider
        ingestion-url: https://mldp.example
        min-conn: 1
        max-conn: 1
reader: []
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(MLDPPVXSControllerConfig(cfg)), MLDPPVXSControllerConfig::Error);
}

TEST(MLDPPVXSControllerConfigTest, ThrowsWhenGrpcIsNotSequence)
{
    const std::string yaml = R"(
writer:
  mldp:
    thread-pool: 1
    mldp-pool:
      provider-name: pvxs_provider
      ingestion-url: https://mldp.example
      min-conn: 1
      max-conn: 1
reader: []
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(MLDPPVXSControllerConfig(cfg)), MLDPPVXSControllerConfig::Error);
}

} // namespace mldp_pvxs_driver::controller

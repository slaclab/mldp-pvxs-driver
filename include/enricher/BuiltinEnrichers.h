//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////
#pragma once

#include <enricher/EnricherFactory.h>

#include <random>
#include <unordered_map>

namespace mldp_pvxs_driver::enricher {

class StaticMetadataEnricher final : public IPayloadEnricher {
    REGISTER_ENRICHER("static-metadata", StaticMetadataEnricher)
public: explicit StaticMetadataEnricher(const config::Config& config) { configure(config); }
    void configure(const config::Config& config) override; bool enrich(util::bus::IDataBus::EventBatch& batch) noexcept override;
    std::string enricherType() const override { return "static-metadata"; }
private: std::unordered_map<std::string, std::string> metadata_;
};
class ColumnAttributesEnricher final : public IPayloadEnricher {
    REGISTER_ENRICHER("column-attributes", ColumnAttributesEnricher)
public: explicit ColumnAttributesEnricher(const config::Config& config) { configure(config); }
    void configure(const config::Config& config) override; bool enrich(util::bus::IDataBus::EventBatch& batch) noexcept override;
    std::string enricherType() const override { return "column-attributes"; }
private: std::string pattern_; std::unordered_map<std::string, std::string> attributes_;
};
class TimestampClampEnricher final : public IPayloadEnricher {
    REGISTER_ENRICHER("timestamp-clamp", TimestampClampEnricher)
public: explicit TimestampClampEnricher(const config::Config&) {}
    void configure(const config::Config&) override {} bool enrich(util::bus::IDataBus::EventBatch& batch) noexcept override;
    std::string enricherType() const override { return "timestamp-clamp"; }
};
class ShardSlotEnricher final : public IPayloadEnricher {
    REGISTER_ENRICHER("shard-slot", ShardSlotEnricher)
public: explicit ShardSlotEnricher(const config::Config& config) { configure(config); }
    void configure(const config::Config& config) override; bool enrich(util::bus::IDataBus::EventBatch& batch) noexcept override;
    std::string enricherType() const override { return "shard-slot"; }
private: std::size_t num_shards_{6}; std::size_t next_shard_{0}; std::mt19937 rng_{std::random_device{}()}; std::unordered_map<std::string, uint16_t> slots_;
};
} // namespace mldp_pvxs_driver::enricher

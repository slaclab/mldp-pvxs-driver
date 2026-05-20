# TODO-11: Create MLDPAnnotationQueryClient + wire into factory + add tests

## Goal
New concrete class `MLDPAnnotationQueryClient` inherits `IQueryable` (factory marker only),
wraps `MLDPGrpcAnnotationPool`, exposes all PV metadata + configuration RPC methods.
Register `"mldp-annotation"` type in the controller dispatch map.
Add integration test (requires live MLDP annotation service).

## Depends On
- TODO-07 (IQueryable is factory marker only)
- TODO-08 (controller dispatch map exists)
- TODO-10 (MLDPGrpcAnnotationPool exists)

## Before starting
Verify exact protobuf message and method names from generated headers:
```bash
find build/ -name "annotation_service.pb.h" -o -name "annotation_service.grpc.pb.h" | head -3
grep -n "getPvMetadata\|GetPvMetadata\|queryPvMetadata\|QueryPvMetadata\|getConfiguration\|getConfigurationActivation\|getActiveConfigurations" \
    build/include/annotation_service.grpc.pb.h 2>/dev/null | head -30
```

## Files to Create

### `include/query/impl/mldp/MLDPAnnotationQueryClient.h` (NEW)
```cpp
//////////////////////////////////////////////////////////////////////////////
// license header...
//////////////////////////////////////////////////////////////////////////////
#pragma once
#include <query/IQueryable.h>
#include <pool/MLDPGrpcAnnotationPool.h>
#include <pool/MLDPGrpcPoolConfig.h>
#include <config/Config.h>
#include <metrics/Metrics.h>
#include <util/log/ILogger.h>
// Include generated protobuf headers for request/response types
#include <annotation_service.pb.h>
#include <annotation_service.grpc.pb.h>
#include <common.pb.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mldp_pvxs_driver::query::impl::mldp {

class MLDPAnnotationQueryClient final : public query::IQueryable {
public:
    explicit MLDPAnnotationQueryClient(const util::pool::MLDPGrpcPoolConfig&,
                                       std::shared_ptr<metrics::Metrics> = nullptr);
    explicit MLDPAnnotationQueryClient(const config::Config&,
                                       std::shared_ptr<metrics::Metrics> = nullptr);

    // PV metadata
    std::optional<dp::service::common::PvMetadata>
    getPvMetadata(const std::string& pvNameOrAlias);

    std::pair<std::vector<dp::service::common::PvMetadata>, std::string>
    queryPvMetadata(const dp::service::annotation::QueryPvMetadataRequest&);

    // Configuration definition
    std::optional<dp::service::common::Configuration>
    getConfiguration(const std::string& configurationName);

    std::pair<std::vector<dp::service::common::Configuration>, std::string>
    queryConfigurations(const dp::service::annotation::QueryConfigurationsRequest&);

    // Configuration activation
    std::optional<dp::service::common::ConfigurationActivation>
    getConfigurationActivation(
        const dp::service::annotation::GetConfigurationActivationRequest&);

    std::pair<std::vector<dp::service::common::ConfigurationActivation>, std::string>
    queryConfigurationActivations(
        const dp::service::annotation::QueryConfigurationActivationsRequest&);

    std::vector<dp::service::common::ConfigurationActivation>
    getActiveConfigurations(const dp::service::common::Timestamp& at);

private:
    std::shared_ptr<util::log::ILogger>                                   logger_;
    util::pool::MLDPGrpcAnnotationPool::MLDPGrpcAnnotationPoolShrdPtr     pool_;
};

} // namespace
```

### `src/query/MLDPAnnotationQueryClient.cpp` (NEW)
Implementation pattern (replicate for each method):
```cpp
std::optional<dp::service::common::PvMetadata>
MLDPAnnotationQueryClient::getPvMetadata(const std::string& pvNameOrAlias)
{
    auto handle = pool_->acquire();
    grpc::ClientContext ctx;
    dp::service::annotation::GetPvMetadataRequest req;
    req.set_pvnameoralias(pvNameOrAlias);
    dp::service::annotation::GetPvMetadataResponse resp;
    const auto status = handle->stub->getPvMetadata(&ctx, req, &resp);
    if (!status.ok()) {
        errorf(*logger_, "getPvMetadata failed: {}", status.error_message());
        return std::nullopt;
    }
    if (resp.has_exceptional_result()) return std::nullopt;
    return resp.get_pv_metadata_result().pv_metadata();
}
```

Adjust method/field names to match actual generated code. Mirror error-handling
style from `MLDPQueryClient.cpp`.

## Files to Change

### `src/controller/MLDPPVXSController.cpp`
In `prepareQueryables()` dispatch map, add the annotation entry:
```cpp
#include <query/impl/mldp/MLDPAnnotationQueryClient.h>

// In kDispatch:
{"mldp-annotation",
 [](const config::Config& c, std::shared_ptr<metrics::Metrics> m) {
     QueryableFactory::instance().prepare<MLDPAnnotationQueryClient>(c, std::move(m));
 }},
```

### `CMakeLists.txt`
Add `src/query/MLDPAnnotationQueryClient.cpp` to `libmldp_pvxs_driver` sources.

## Test File to Create

### `test/query/mldp_annotation_query_client_integration_test.cpp` (NEW)
Integration test — requires live MLDP annotation service.
Mirror the pattern from `test/writer/mldp/mldp_writer_integration_test.cpp`.

Key test cases:
1. **getPvMetadata** — known PV returns populated `PvMetadata`
2. **queryPvMetadata** — returns non-empty list
3. **getActiveConfigurations(now)** — returns result without throwing
4. **Factory integration** — `QueryableFactory::instance().prepare<MLDPAnnotationQueryClient>(cfg)`,
   then `create<MLDPAnnotationQueryClient>()` returns non-null and `getPvMetadata` succeeds

Add to CMakeLists.txt as a separate test executable (integration tests often gated by
a CI environment variable or separate ctest label).

## Verification
```bash
cmake --build build 2>&1 | grep -E "error:" | head -20
# Unit / no-service tests pass
ctest --test-dir build -R "annotation" -V 2>&1 | tail -30
# With live service:
# MLDP_ANNOTATION_URL=localhost:50053 ctest --test-dir build -R "annotation_integration" -V
```

## Commit
```
feat(query): add MLDPAnnotationQueryClient with factory integration

MLDPAnnotationQueryClient wraps MLDPGrpcAnnotationPool and exposes all
DpAnnotationService RPC methods as plain public methods. Registered as
"mldp-annotation" type in the controller factory dispatch map. Includes
integration tests for getPvMetadata and configuration activation queries.
```

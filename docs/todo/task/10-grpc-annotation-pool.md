# TODO-10: Add annotation URL to MLDPGrpcPoolConfig + create MLDPGrpcAnnotationPool

## Goal
`MLDPGrpcPoolConfig` gains an `annotation_url_` field. New class `MLDPGrpcAnnotationPool`
mirrors `MLDPGrpcQueryPool` but uses `DpAnnotationService::Stub`.

## Depends On
Nothing from previous todos (pure additions), but logically precedes TODO-11.

## Before starting
Examine `MLDPGrpcQueryPool` to understand the pool pattern:
```bash
cat include/pool/MLDPGrpcQueryPool.h
cat src/pool/MLDPGrpcQueryPool.cpp
cat include/pool/MLDPGrpcPoolConfig.h
cat src/pool/MLDPGrpcPoolConfig.cpp
```
Also check which protobuf/gRPC header provides `DpAnnotationService`:
```bash
find build/ -name "annotation_service.grpc.pb.h" | head -3
grep -rn "DpAnnotationService\|annotation_service" build/include/ --include="*.h" | head -10
```

## Files to Change

### `include/pool/MLDPGrpcPoolConfig.h`
Add private field:
```cpp
std::string annotation_url_;
```
Add public accessor:
```cpp
const std::string& annotationUrl() const { return annotation_url_; }
```

### `src/pool/MLDPGrpcPoolConfig.cpp`
Parse alongside `query-url` (or `ingestion-url`):
```cpp
annotation_url_ = root.get("annotation-url", "");
```
If `annotation-url` is absent, `annotation_url_` is empty string (soft disable).

### `include/pool/MLDPGrpcAnnotationPool.h` (NEW)
Mirror `MLDPGrpcQueryPool.h` structure. Key differences:
- Stub type: `dp::service::annotation::DpAnnotationService::Stub` (verify exact namespace)
- Pool object (`AnnotationObject` or similar): holds `shared_ptr<grpc::Channel>` + `unique_ptr<Stub>`
- Channel built from `config.annotationUrl()`
- Class name: `MLDPGrpcAnnotationPool`
- Shared pointer alias: `MLDPGrpcAnnotationPoolShrdPtr`

```cpp
class MLDPGrpcAnnotationPool {
public:
    using MLDPGrpcAnnotationPoolShrdPtr = std::shared_ptr<MLDPGrpcAnnotationPool>;

    static MLDPGrpcAnnotationPoolShrdPtr create(
        const MLDPGrpcPoolConfig&,
        std::shared_ptr<metrics::Metrics> = nullptr);

    // Returns a pooled handle; exact type mirrors MLDPGrpcQueryPool::acquire()
    // The handle's stub field is unique_ptr<DpAnnotationService::Stub>
    util::pool::PooledHandle<AnnotationObject> acquire();

private:
    // ... mirror MLDPGrpcQueryPool internals
};
```

### `src/pool/MLDPGrpcAnnotationPool.cpp` (NEW)
Mirror `MLDPGrpcQueryPool.cpp`. Channel creation from `config.annotationUrl()`.
Stub creation: `DpAnnotationService::NewStub(channel)`.

### `CMakeLists.txt`
Add `src/pool/MLDPGrpcAnnotationPool.cpp` to `libmldp_pvxs_driver` sources.

## Verification
```bash
cmake --build build 2>&1 | grep -E "error:" | head -20
# Check pool tests if any exist
ctest --test-dir build -R "pool" -V 2>&1 | tail -20
```

## Commit
```
feat(pool): add annotation_url to MLDPGrpcPoolConfig and new MLDPGrpcAnnotationPool

MLDPGrpcPoolConfig gains annotation-url YAML key and annotationUrl() accessor.
MLDPGrpcAnnotationPool provides a gRPC connection pool for DpAnnotationService,
mirroring the existing MLDPGrpcQueryPool pattern.
```

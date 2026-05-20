# TODO-09: QueryableFactory unit tests

## Goal
Verify the factory's prepare/create/isPrepared/reset contract with a focused unit test.
No live gRPC connection needed — use `MLDPQueryClient` with a fake/unreachable URL
or write a minimal stub type.

## Depends On
- TODO-06 (`QueryableFactory` and `QueryableHolder` exist)
- TODO-07 (`MLDPQueryClient` has Config-ctor)

## File to Create

### `test/query/queryable_factory_test.cpp` (NEW)

Test cases to cover:

1. **isPrepared false before prepare**
   ```cpp
   QueryableFactory::instance().reset();
   EXPECT_FALSE(QueryableFactory::instance().isPrepared<MLDPQueryClient>());
   ```

2. **isPrepared true after prepare**
   ```cpp
   QueryableFactory::instance().prepare<MLDPQueryClient>(make_minimal_config());
   EXPECT_TRUE(QueryableFactory::instance().isPrepared<MLDPQueryClient>());
   ```

3. **create returns non-null unique_ptr<T>**
   ```cpp
   auto q = QueryableFactory::instance().create<MLDPQueryClient>();
   EXPECT_NE(q, nullptr);
   ```

4. **create on unprepared type throws std::runtime_error**
   ```cpp
   QueryableFactory::instance().reset();
   EXPECT_THROW(QueryableFactory::instance().create<MLDPQueryClient>(),
                std::runtime_error);
   ```

5. **reset clears registered types**
   ```cpp
   QueryableFactory::instance().prepare<MLDPQueryClient>(make_minimal_config());
   QueryableFactory::instance().reset();
   EXPECT_FALSE(QueryableFactory::instance().isPrepared<MLDPQueryClient>());
   ```

6. **QueryableHolder::as<T>() — correct type returns non-null**
   ```cpp
   auto q = QueryableFactory::instance().create<MLDPQueryClient>();
   QueryableHolder holder(std::move(q));
   EXPECT_TRUE(holder.valid());
   EXPECT_NE(holder.as<MLDPQueryClient>(), nullptr);
   ```

### Helper `make_minimal_config()`
```cpp
static config::Config make_minimal_config() {
    return makeConfigFromYaml(
        "ingestion-url: localhost:19999\n"
        "query-url: localhost:19998\n"
        "provider-name: test-provider\n"
        "min-connections: 1\n"
        "max-connections: 1\n");
}
```
Use an unreachable port — construction should succeed; only actual RPC calls would fail.

## CMakeLists.txt
Add the new test file to the appropriate test target. Look at how other test files in
`test/query/` (if any) or `test/writer/` are registered and mirror that pattern.

```cmake
# In test/CMakeLists.txt or test/query/CMakeLists.txt:
add_executable(queryable_factory_test query/queryable_factory_test.cpp)
target_link_libraries(queryable_factory_test PRIVATE
    mldp_pvxs_driver GTest::gtest_main)
gtest_discover_tests(queryable_factory_test)
```

## Verification
```bash
cmake --build build 2>&1 | grep -E "error:" | head -20
ctest --test-dir build -R "queryable_factory" -V 2>&1 | tail -30
```

## Commit
```
test(query): add QueryableFactory unit tests

Covers prepare/isPrepared/create/reset lifecycle and QueryableHolder::as<T>()
downcast. Uses an unreachable localhost URL so no live service is required.
```

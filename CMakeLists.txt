cmake_minimum_required(VERSION 3.15)
project(mldp_pvxs_driver VERSION 0.2.0 LANGUAGES CXX)

# C++ Standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(MLDP_PVXS_DRIVER_MAIN  "Build MLDP PVXS driver executable"  ON)
option(MLDP_PVXS_DRIVER_TESTS "Build MLDP PVXS driver tests"      OFF)
option(MLDP_PVXS_DRIVER_COVERAGE "Enable code coverage instrumentation" OFF)
option(MLDP_PVXS_DRIVER_LINK_EPICS_PVXS_STATIC "Link EPICS and PVXS libraries statically (prefer .a over .so)" ON)

# Default to Release build
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

set(PROMETHEUS_CPP_VERSION "v1.3.0")
set(SPDLOG_VERSION "v1.16.0")
set(ARGPARSE_VERSION "v3.2")
# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/mldp_pvxs_driver_version.h"
    @ONLY
)

message(STATUS "Building version: ${PROJECT_VERSION}")

# print the c++ standard being used
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")

# Acquire spdlog for structured logging and ensure it builds before the driver library.
message(STATUS "Acquiring spdlog version ${SPDLOG_VERSION}...")
FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        ${SPDLOG_VERSION}
)
FetchContent_MakeAvailable(spdlog)

# Acquire argparse for command-line argument parsing
message(STATUS "Acquire argparse version ${ARGPARSE_VERSION}...")
FetchContent_Declare(argparse
        GIT_REPOSITORY https://github.com/p-ranav/argparse.git
        GIT_TAG        ${ARGPARSE_VERSION}
)
FetchContent_MakeAvailable(argparse)

# Prometheus-cpp pulls its own GoogleTest dependency when ENABLE_TESTING is set.
# Temporarily override the cached ENABLE_TESTING option so the dependency does
# not try to define conflicting gmock targets when added as a subproject.
set(_PROMETHEUS_ENABLE_TESTING_DEFINED FALSE)
if(DEFINED CACHE{ENABLE_TESTING})
    set(_PROMETHEUS_ENABLE_TESTING_DEFINED TRUE)
    set(_PROMETHEUS_ENABLE_TESTING_VALUE ${ENABLE_TESTING})
endif()
set(ENABLE_TESTING OFF CACHE BOOL "Disable tests when pulling prometheus-cpp" FORCE)
message (STATUS "Acquiring prometheus-cpp version ${PROMETHEUS_CPP_VERSION}...")
FetchContent_Declare(prometheus-cpp
        GIT_REPOSITORY https://github.com/jupp0r/prometheus-cpp.git
        GIT_TAG        ${PROMETHEUS_CPP_VERSION}
)
FetchContent_MakeAvailable(prometheus-cpp)

if(_PROMETHEUS_ENABLE_TESTING_DEFINED)
    set(ENABLE_TESTING ${_PROMETHEUS_ENABLE_TESTING_VALUE} CACHE BOOL "Restore ENABLE_TESTING after prometheus-cpp" FORCE)
else()
    unset(ENABLE_TESTING CACHE)
endif()

# Find required packages
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

# epics-base (7.0+)
if(NOT EPICS_BASE)
    set(EPICS_BASE "$ENV{EPICS_BASE}")
endif()
if(NOT EPICS_HOST_ARCH)
    set(EPICS_HOST_ARCH "$ENV{EPICS_HOST_ARCH}")
endif()

# Prefer user hints but fall back to common dev-container install locations
set(EPICS_LIB_HINTS "")
if(EPICS_BASE)
    list(APPEND EPICS_LIB_HINTS
            "${EPICS_BASE}/lib"
            "${EPICS_BASE}/lib/${EPICS_HOST_ARCH}")
endif()

if(MLDP_PVXS_DRIVER_LINK_EPICS_PVXS_STATIC)
    set(_mldp_saved_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    unset(EPICS_COM_LIB CACHE)
endif()

find_library(EPICS_COM_LIB
        NAMES Com libCom
        PATHS ${EPICS_LIB_HINTS})

if(MLDP_PVXS_DRIVER_LINK_EPICS_PVXS_STATIC)
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${_mldp_saved_suffixes}")
endif()

if(NOT EPICS_COM_LIB)
    message(FATAL_ERROR "Could not locate libCom. Set EPICS_BASE/EPICS_HOST_ARCH or install EPICS Base in a searchable location.")
endif()

get_filename_component(EPICS_LIB_DIR "${EPICS_COM_LIB}" DIRECTORY)

set(EPICS_INCLUDE_HINTS "")
if(EPICS_BASE)
    list(APPEND EPICS_INCLUDE_HINTS "${EPICS_BASE}/include")
endif()
list(APPEND EPICS_INCLUDE_HINTS "${EPICS_LIB_DIR}/../include")

find_path(EPICS_INCLUDE_DIR epicsVersion.h PATHS ${EPICS_INCLUDE_HINTS})

if(NOT EPICS_INCLUDE_DIR)
    message(FATAL_ERROR "Could not locate EPICS headers. Set EPICS_BASE or add include path manually.")
endif()

set(EPICS_OS_DIR "os/Linux")
if(APPLE)
    set(EPICS_OS_DIR "os/Darwin")
endif()

if(MLDP_PVXS_DRIVER_LINK_EPICS_PVXS_STATIC)
    add_library(epics-com STATIC IMPORTED)
else()
    add_library(epics-com SHARED IMPORTED)
endif()
set_target_properties(epics-com PROPERTIES IMPORTED_LOCATION "${EPICS_COM_LIB}")
target_include_directories(epics-com INTERFACE
        "${EPICS_INCLUDE_DIR}"
        "${EPICS_INCLUDE_DIR}/${EPICS_OS_DIR}"
)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_include_directories(epics-com INTERFACE
            "${EPICS_INCLUDE_DIR}/compiler/clang"
    )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_include_directories(epics-com INTERFACE
            "${EPICS_INCLUDE_DIR}/compiler/gcc"
    )
else()
    message(FATAL_ERROR "Unable to determine compiler ID!")
endif()

# pvxs
if(NOT PVXS_BASE)
    set(PVXS_BASE "$ENV{PVXS_BASE}")
endif()

set(PVXS_FOUND FALSE)
if(PKG_CONFIG_FOUND AND NOT MLDP_PVXS_DRIVER_LINK_EPICS_PVXS_STATIC)
    pkg_check_modules(PVXS_PC QUIET pvxs)
    if(PVXS_PC_FOUND)
        add_library(pvxs INTERFACE IMPORTED)
        target_link_libraries(pvxs INTERFACE epics-com ${PVXS_PC_LINK_LIBRARIES})
        target_include_directories(pvxs INTERFACE ${PVXS_PC_INCLUDE_DIRS})
        set(PVXS_FOUND TRUE)
    endif()
endif()

if(NOT PVXS_FOUND)
    set(PVXS_LIB_HINTS "")
    if(PVXS_BASE)
        list(APPEND PVXS_LIB_HINTS
                "${PVXS_BASE}/lib"
                "${PVXS_BASE}/lib/${EPICS_HOST_ARCH}")
    endif()

    if(MLDP_PVXS_DRIVER_LINK_EPICS_PVXS_STATIC)
        unset(PVXS_LIB CACHE)
        set(_mldp_saved_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
        set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
        find_library(PVXS_LIB
                NAMES pvxs libpvxs
                PATHS ${PVXS_LIB_HINTS})
        set(CMAKE_FIND_LIBRARY_SUFFIXES "${_mldp_saved_suffixes}")
    else()
        find_library(PVXS_LIB
                NAMES pvxs libpvxs
                PATHS ${PVXS_LIB_HINTS})
    endif()

    if(NOT PVXS_LIB)
        message(FATAL_ERROR "Could not find pvxs library. Set PVXS_BASE or install pvxs in a searchable location.")
    endif()

    get_filename_component(PVXS_LIB_DIR "${PVXS_LIB}" DIRECTORY)

    set(PVXS_INCLUDE_HINTS "")
    if(PVXS_BASE)
        list(APPEND PVXS_INCLUDE_HINTS "${PVXS_BASE}/include")
    endif()
    list(APPEND PVXS_INCLUDE_HINTS
            "${EPICS_INCLUDE_DIR}"
            "${PVXS_LIB_DIR}/../include")

    find_path(PVXS_INCLUDE_DIR pvxs/version.h PATHS ${PVXS_INCLUDE_HINTS})

    if(NOT PVXS_INCLUDE_DIR)
        message(FATAL_ERROR "Could not find pvxs headers. Set PVXS_BASE or add the include path manually.")
    endif()

    if(MLDP_PVXS_DRIVER_LINK_EPICS_PVXS_STATIC)
        add_library(pvxs STATIC IMPORTED)
    else()
        add_library(pvxs SHARED IMPORTED)
    endif()
    set_target_properties(pvxs PROPERTIES IMPORTED_LOCATION "${PVXS_LIB}")
    target_link_libraries(pvxs INTERFACE epics-com)

        if(MLDP_PVXS_DRIVER_LINK_EPICS_PVXS_STATIC)
            find_library(LIBEVENT_CORE_LIB NAMES event_core)
            find_library(LIBEVENT_EXTRA_LIB NAMES event_extra)
            find_library(LIBEVENT_PTHREADS_LIB NAMES event_pthreads)

            if(NOT LIBEVENT_CORE_LIB OR NOT LIBEVENT_EXTRA_LIB OR NOT LIBEVENT_PTHREADS_LIB)
                message(FATAL_ERROR "Static PVXS linking requested, but required libevent libraries were not found (event_core/event_extra/event_pthreads).")
            endif()

            target_link_libraries(pvxs INTERFACE
                    "${LIBEVENT_CORE_LIB}"
                    "${LIBEVENT_EXTRA_LIB}"
                    "${LIBEVENT_PTHREADS_LIB}"
            )
        endif()
    target_include_directories(pvxs INTERFACE "${PVXS_INCLUDE_DIR}")
endif()

# Proto file configuration
set(PROTO_PATH $ENV{PROTO_PATH})

set(PROTO_FILES
        ${PROTO_PATH}/common.proto
        ${PROTO_PATH}/ingestion.proto
)

# Generate proto sources
set(PROTO_SRCS "")
set(PROTO_HDRS "")
set(GRPC_SRCS "")
set(GRPC_HDRS "")

foreach(proto_file ${PROTO_FILES})
    get_filename_component(proto_name ${proto_file} NAME_WE)
    list(APPEND PROTO_SRCS "${CMAKE_CURRENT_BINARY_DIR}/proto/${proto_name}.pb.cc")
    list(APPEND PROTO_HDRS "${CMAKE_CURRENT_BINARY_DIR}/proto/${proto_name}.pb.h")
    list(APPEND GRPC_SRCS "${CMAKE_CURRENT_BINARY_DIR}/proto/${proto_name}.grpc.pb.cc")
    list(APPEND GRPC_HDRS "${CMAKE_CURRENT_BINARY_DIR}/proto/${proto_name}.grpc.pb.h")
endforeach()

# Find grpc_cpp_plugin
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

# Create proto output directory
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/proto)

# Custom command for proto compilation
add_custom_command(
        OUTPUT ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/proto
        COMMAND ${Protobuf_PROTOC_EXECUTABLE}
        --proto_path=${PROTO_PATH}
        --cpp_out=${CMAKE_CURRENT_BINARY_DIR}/proto
        --grpc_out=${CMAKE_CURRENT_BINARY_DIR}/proto
        --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
        ${PROTO_FILES}
        DEPENDS ${PROTO_FILES}
)

# Proto library
add_library(lib${PROJECT_NAME}_proto STATIC
        ${PROTO_SRCS}
        ${GRPC_SRCS}
)

target_link_libraries(lib${PROJECT_NAME}_proto PUBLIC
        protobuf::libprotobuf
        gRPC::grpc++
)

target_include_directories(lib${PROJECT_NAME}_proto PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}/proto
)

# Base library
add_library(lib${PROJECT_NAME}
        "${CMAKE_CURRENT_SOURCE_DIR}/ext/BS_thread_pool/include/BS_thread_pool.hpp"
        # "${CMAKE_CURRENT_SOURCE_DIR}/include/bus/IEventBusPush.h"
        # "${CMAKE_CURRENT_SOURCE_DIR}/include/reader/Reader.h"
        # "${CMAKE_CURRENT_SOURCE_DIR}/include/reader/ReaderFactory.h"
        # "${CMAKE_CURRENT_SOURCE_DIR}/include/mldp_pvxs_driver.h"
        # "${CMAKE_CURRENT_SOURCE_DIR}/include/reader/impl/epics/EpicsReader.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/reader/ReaderFactory.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/util/log/CoutLogger.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/config/Config.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/reader/impl/epics/EpicsReader.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/reader/impl/epics/EpicsReaderConfig.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/reader/impl/epics/EpicsMLDPConversion.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/reader/impl/epics/BSASEpicsMLDPConversion.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/pool/MLDPGrpcPoolConfig.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/pool/MLDPGrpcPool.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/controller/MLDPPVXSControllerConfig.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/controller/MLDPPVXSController.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/metrics/Metrics.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/metrics/MetricsConfig.cpp"
)
target_link_libraries(lib${PROJECT_NAME} PUBLIC lib${PROJECT_NAME}_proto Threads::Threads pvxs prometheus-cpp::core prometheus-cpp::pull)
target_include_directories(lib${PROJECT_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ext/BS_thread_pool/include")
target_include_directories(lib${PROJECT_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ext/rapidyaml/include")
target_include_directories(lib${PROJECT_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# Ensure the build tree's generated headers (e.g. config.h) are visible to users of the library
target_include_directories(lib${PROJECT_NAME} PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
set_target_properties(lib${PROJECT_NAME} PROPERTIES PREFIX "")

if(MLDP_PVXS_DRIVER_COVERAGE)
    message(STATUS "Coverage enabled!")
    if(NOT (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
        message(WARNING "Coverage requested but compiler '${CMAKE_CXX_COMPILER_ID}' may not support gcov-style coverage")
    endif()

    # Apply coverage flags only to this project's targets (not dependencies).
    target_compile_options(lib${PROJECT_NAME} PRIVATE -O0 -g --coverage)
    target_compile_options(lib${PROJECT_NAME}_proto PRIVATE -O0 -g --coverage)
endif()

# Test base library
if(MLDP_PVXS_DRIVER_TESTS)
    # Needed for CTest dashboard modes (e.g. `ctest -T test` / `ctest -T coverage`).
    # This generates DartConfiguration.tcl in the build tree.
    if(MLDP_PVXS_DRIVER_COVERAGE)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            find_program(LLVM_COV_EXECUTABLE NAMES llvm-cov llvm-cov-18 llvm-cov-17 llvm-cov-16)
            if(LLVM_COV_EXECUTABLE)
                set(MLDP_PVXS_DRIVER_GCOV_TOOL "${CMAKE_BINARY_DIR}/mldp-llvm-gcov.sh")
                file(WRITE "${MLDP_PVXS_DRIVER_GCOV_TOOL}" "#!/usr/bin/env bash\n\n# Wrapper to make Clang's coverage data usable via gcov-compatible tooling.\n# Filters llvm-cov gcov output lines which CTest doesn't understand.\nset -euo pipefail\n\n\"${LLVM_COV_EXECUTABLE}\" gcov \"$@\" 2>&1 | { grep -v 'Lines executed:nan% of 0' || true; }\nexit \${PIPESTATUS[0]}\n")
                execute_process(COMMAND chmod +x "${MLDP_PVXS_DRIVER_GCOV_TOOL}")
                set(CTEST_COVERAGE_COMMAND "${MLDP_PVXS_DRIVER_GCOV_TOOL}" CACHE FILEPATH "Coverage tool" FORCE)
                # For `ctest -T coverage`, DartConfiguration.tcl is generated from @COVERAGE_COMMAND@.
                set(COVERAGE_COMMAND "${MLDP_PVXS_DRIVER_GCOV_TOOL}" CACHE FILEPATH "Coverage tool" FORCE)
                # CTest defaults to `-l` for gcov, but llvm-cov's `-l` output naming isn't compatible
                # with CTest's file collection (it can't find expected *.gcov files).
                set(COVERAGE_EXTRA_FLAGS "" CACHE STRING "Extra flags for coverage tool" FORCE)
            else()
                message(WARNING "Coverage enabled with Clang but llvm-cov was not found; CTest/LCOV coverage may fail")
            endif()
        else()
            find_program(GCOV_EXECUTABLE gcov)
            if(GCOV_EXECUTABLE)
                set(CTEST_COVERAGE_COMMAND "${GCOV_EXECUTABLE}" CACHE FILEPATH "Coverage tool" FORCE)
                set(COVERAGE_COMMAND "${GCOV_EXECUTABLE}" CACHE FILEPATH "Coverage tool" FORCE)
            endif()
        endif()
    endif()
    include(CTest)

    # Help `ctest -T coverage` resolve relative source paths produced by llvm-cov gcov
    # (e.g. "Source:proto/common.pb.cc"), by providing symlinks inside CoverageInfo.
    if(MLDP_PVXS_DRIVER_COVERAGE)
        set(MLDP_PVXS_DRIVER_COVERAGEINFO_DIR "${CMAKE_BINARY_DIR}/Testing/CoverageInfo")
        file(MAKE_DIRECTORY "${MLDP_PVXS_DRIVER_COVERAGEINFO_DIR}")
        if(NOT EXISTS "${MLDP_PVXS_DRIVER_COVERAGEINFO_DIR}/proto")
            file(CREATE_LINK "${CMAKE_BINARY_DIR}/proto" "${MLDP_PVXS_DRIVER_COVERAGEINFO_DIR}/proto" SYMBOLIC)
        endif()
        if(NOT EXISTS "${MLDP_PVXS_DRIVER_COVERAGEINFO_DIR}/_deps")
            file(CREATE_LINK "${CMAKE_BINARY_DIR}/_deps" "${MLDP_PVXS_DRIVER_COVERAGEINFO_DIR}/_deps" SYMBOLIC)
        endif()
    endif()

    FetchContent_Declare(googletest
            GIT_REPOSITORY "https://github.com/google/googletest.git"
            GIT_TAG        "main")
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    add_executable(${PROJECT_NAME}_test
            "${CMAKE_CURRENT_SOURCE_DIR}/test/mock/sioc.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/config/epics_reader_config_test.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/config/mldppvxs_controller_config_test.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/controller/mldppvxs_controller_test.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/ryml_single_header_translation_unit.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/pool/test_mldp_grpc_pool.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/reader/impl/epics/epics_reader_test.cpp"
    )
    target_link_libraries(${PROJECT_NAME}_test PRIVATE gtest_main lib${PROJECT_NAME})

    if(MLDP_PVXS_DRIVER_COVERAGE)
        target_compile_options(${PROJECT_NAME}_test PRIVATE -O0 -g --coverage)
        target_link_options(${PROJECT_NAME}_test PRIVATE --coverage)

        find_program(LCOV_EXECUTABLE lcov)
        find_program(GENHTML_EXECUTABLE genhtml)
        if(LCOV_EXECUTABLE AND GENHTML_EXECUTABLE)
            add_custom_target(coverage
                COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
                COMMAND ${LCOV_EXECUTABLE} --ignore-errors inconsistent --gcov-tool ${CTEST_COVERAGE_COMMAND} --capture --directory . --output-file coverage.info
                COMMAND ${LCOV_EXECUTABLE} --ignore-errors inconsistent --remove coverage.info '/usr/*' '${CMAKE_BINARY_DIR}/_deps/*' --output-file coverage.info
                COMMAND ${GENHTML_EXECUTABLE} --ignore-errors inconsistent -o coverage-html coverage.info
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                DEPENDS ${PROJECT_NAME}_test
                COMMENT "Run tests and generate coverage.info + coverage-html/")

            # Convenience target for already-built instrumented binaries.
            # This does NOT build any targets; it assumes the test executable exists.
            add_custom_target(coverage-report
                COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
                COMMAND ${LCOV_EXECUTABLE} --ignore-errors inconsistent --gcov-tool ${CTEST_COVERAGE_COMMAND} --capture --directory . --output-file coverage.info
                COMMAND ${LCOV_EXECUTABLE} --ignore-errors inconsistent --remove coverage.info '/usr/*' '${CMAKE_BINARY_DIR}/_deps/*' --output-file coverage.info
                COMMAND ${GENHTML_EXECUTABLE} --ignore-errors inconsistent -o coverage-html coverage.info
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                COMMENT "Generate coverage from existing build (no rebuild)")
        else()
            message(STATUS "Coverage enabled, but 'lcov'/'genhtml' not found; build will still emit .gcno/.gcda when tests run")
        endif()
    endif()

    include(GoogleTest)
    gtest_discover_tests(${PROJECT_NAME}_test)
endif()

# Driver executable
if(MLDP_PVXS_DRIVER_MAIN)
    add_executable(${PROJECT_NAME}
            "${CMAKE_CURRENT_SOURCE_DIR}/ext/rapidyaml/include/rapidyaml-0.10.0.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/mldp_pvxs_driver_main.cpp"
    )

    # The reader implementations self-register via static initialization (REGISTER_READER).
    # When linking a static library, the linker may drop object files that only contain such
    # side-effect registrations. Ensure we keep the whole archive for the CLI.
    if(APPLE)
        target_link_libraries(${PROJECT_NAME} PRIVATE
                -Wl,-force_load $<TARGET_FILE:lib${PROJECT_NAME}>
                argparse::argparse
        )
    else()
        target_link_libraries(${PROJECT_NAME} PRIVATE
                -Wl,--whole-archive lib${PROJECT_NAME} -Wl,--no-whole-archive
                argparse::argparse
                spdlog::spdlog
        )
    endif()
    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ext/rapidyaml/include")

    if(MLDP_PVXS_DRIVER_COVERAGE)
        target_compile_options(${PROJECT_NAME} PRIVATE -O0 -g --coverage)
        target_link_options(${PROJECT_NAME} PRIVATE --coverage)
    endif()
endif()

find_package(Doxygen REQUIRED)
set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/docs")
doxygen_add_docs(
    docs
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    HTML_OUTPUT html
    COMMENT "Generating documentation"
)

# Installation
install(TARGETS lib${PROJECT_NAME}_proto
        ARCHIVE DESTINATION lib
)

install(TARGETS lib${PROJECT_NAME}
        ARCHIVE DESTINATION lib
)

install(DIRECTORY include/
        DESTINATION include/pvxs_mldp_driver
        FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
)


# Install generated config header so downstream consumers can find it after `make install`.
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/config.h"
    DESTINATION include/pvxs_mldp_driver
    OPTIONAL)

if(MLDP_PVXS_DRIVER_MAIN)
    install(TARGETS
            ${PROJECT_NAME}
            RUNTIME DESTINATION bin
    )
endif()

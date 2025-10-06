cmake_minimum_required(VERSION 3.15)
project(bsas_mldp_driver LANGUAGES CXX)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(PVXS_MLDP_DRIVER_MAIN  "Build PVXS MLDP driver executable"  ON)
option(PVXS_MLDP_DRIVER_TESTS "Build PVXS MLDP driver tests"      OFF)

# Default to Release build
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Find required packages
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
find_package(Threads REQUIRED)

# epics-base (7.0+)
if(NOT EPICS_BASE)
    set(EPICS_BASE "$ENV{EPICS_BASE}")
    if(NOT EPICS_BASE)
        message(FATAL_ERROR "Could not read EPICS_BASE environment variable!")
    endif()
endif()
if(NOT EPICS_HOST_ARCH)
    set(EPICS_HOST_ARCH "$ENV{EPICS_HOST_ARCH}")
    if(NOT EPICS_HOST_ARCH)
        message(FATAL_ERROR "Could not read EPICS_HOST_ARCH environment variable!")
    endif()
endif()
add_library(epics-com SHARED IMPORTED)
set_target_properties(epics-com PROPERTIES IMPORTED_LOCATION "${EPICS_BASE}/lib/${EPICS_HOST_ARCH}/libCom.so")
target_include_directories(epics-com INTERFACE
        "${EPICS_BASE}/include"
        "${EPICS_BASE}/include/os/Linux"
)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_include_directories(epics-com INTERFACE
            "${EPICS_BASE}/include/compiler/clang"
    )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_include_directories(epics-com INTERFACE
            "${EPICS_BASE}/include/compiler/gcc"
    )
else()
    message(FATAL_ERROR "Unable to determine compiler ID!")
endif()

# pvxs
if(NOT PVXS_BASE)
    set(PVXS_BASE "$ENV{PVXS_BASE}")
    if(NOT PVXS_BASE)
        message(FATAL_ERROR "Could not find PVXS! Set the PVXS_BASE variable to continue.")
    endif()
endif()
add_library(pvxs SHARED IMPORTED)
set_target_properties(pvxs PROPERTIES IMPORTED_LOCATION "${PVXS_BASE}/lib/${EPICS_HOST_ARCH}/libpvxs.so")
target_link_libraries(pvxs INTERFACE epics-com)
target_include_directories(pvxs INTERFACE "${PVXS_BASE}/include")

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

# Custom command for proto compilation
add_custom_command(
        OUTPUT ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
        COMMAND ${Protobuf_PROTOC_EXECUTABLE}
        --proto_path=${PROTO_PATH}
        --cpp_out=${CMAKE_CURRENT_BINARY_DIR}/proto
        --grpc_out=${CMAKE_CURRENT_BINARY_DIR}/proto
        --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
        ${PROTO_FILES}
        DEPENDS ${PROTO_FILES}
)

# Proto library
add_library(pvxs_mldp_driver_proto STATIC
        ${PROTO_SRCS}
        ${GRPC_SRCS}
)

target_link_libraries(pvxs_mldp_driver_proto PUBLIC
        protobuf::libprotobuf
        gRPC::grpc++
)

target_include_directories(pvxs_mldp_driver_proto PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}/proto
)

# Base library
add_library(libpvxs_mldp_driver
        "${CMAKE_CURRENT_SOURCE_DIR}/include/pvxs_mldp_driver.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/pvxs_mldp_driver.cpp"
)
target_link_libraries(libpvxs_mldp_driver PUBLIC pvxs_mldp_driver_proto Threads::Threads pvxs)
target_include_directories(libpvxs_mldp_driver PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(libpvxs_mldp_driver PROPERTIES PREFIX "")

# Test base library
if(PVXS_MLDP_DRIVER_TESTS)
    include(FetchContent)
    FetchContent_Declare(googletest
            GIT_REPOSITORY "https://github.com/google/googletest.git"
            GIT_TAG        "main")
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    add_executable(pvxs_mldp_driver_test
            "${CMAKE_CURRENT_SOURCE_DIR}/test/mock/dp.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/mock/dp.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/mock/sioc.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/mock/sioc.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/driver.cpp"
    )
    target_link_libraries(pvxs_mldp_driver_test PRIVATE gtest_main libpvxs_mldp_driver)
    include(GoogleTest)
    gtest_discover_tests(pvxs_mldp_driver_test)
endif()

# Driver executable
if(PVXS_MLDP_DRIVER_MAIN)
    add_executable(pvxs_mldp_driver
            "${CMAKE_CURRENT_SOURCE_DIR}/ext/rapidyaml/include/rapidyaml-0.10.0.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/pvxs_mldp_driver_main.cpp"
    )
    target_link_libraries(pvxs_mldp_driver PRIVATE libpvxs_mldp_driver)
    target_include_directories(pvxs_mldp_driver PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ext/rapidyaml/include")
endif()

# Installation
install(TARGETS pvxs_mldp_driver_proto
        ARCHIVE DESTINATION lib
)

install(TARGETS libpvxs_mldp_driver
        ARCHIVE DESTINATION lib
)

install(DIRECTORY include/
        DESTINATION include/pvxs_mldp_driver
        FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
)

if(PVXS_MLDP_DRIVER_MAIN)
    install(TARGETS
            pvxs_mldp_driver
            RUNTIME DESTINATION bin
    )
endif()

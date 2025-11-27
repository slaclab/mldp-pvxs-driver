cmake_minimum_required(VERSION 3.15)
project(mldp_pvxs_driver VERSION 0.2.0 LANGUAGES CXX)

# C++ Standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(MLDP_PVXS_DRIVER_MAIN  "Build MLDP PVXS driver executable"  ON)
option(MLDP_PVXS_DRIVER_TESTS "Build MLDP PVXS driver tests"      OFF)

# Default to Release build
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

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
FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        v1.16.0
)
FetchContent_MakeAvailable(spdlog)

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

find_library(EPICS_COM_LIB
        NAMES Com libCom
        PATHS ${EPICS_LIB_HINTS})

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

add_library(epics-com SHARED IMPORTED)
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
if(PKG_CONFIG_FOUND)
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

    find_library(PVXS_LIB
            NAMES pvxs libpvxs
            PATHS ${PVXS_LIB_HINTS})

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

    add_library(pvxs SHARED IMPORTED)
    set_target_properties(pvxs PROPERTIES IMPORTED_LOCATION "${PVXS_LIB}")
    target_link_libraries(pvxs INTERFACE epics-com)
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
        "${CMAKE_CURRENT_SOURCE_DIR}/src/reader/impl/epics/EpicsReader.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/config/Config.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/mldp_pvxs_driver.cpp"
)
target_link_libraries(lib${PROJECT_NAME} PUBLIC lib${PROJECT_NAME}_proto Threads::Threads pvxs spdlog::spdlog)
target_include_directories(lib${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ext/BS_thread_pool/include")
target_include_directories(lib${PROJECT_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ext/rapidyaml/include")
target_include_directories(lib${PROJECT_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# Ensure the build tree's generated headers (e.g. config.h) are visible to users of the library
target_include_directories(lib${PROJECT_NAME} PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
set_target_properties(lib${PROJECT_NAME} PROPERTIES PREFIX "")

# Test base library
if(MLDP_PVXS_DRIVER_TESTS)
    FetchContent_Declare(googletest
            GIT_REPOSITORY "https://github.com/google/googletest.git"
            GIT_TAG        "main")
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    add_executable(${PROJECT_NAME}_test
            "${CMAKE_CURRENT_SOURCE_DIR}/test/mock/sioc.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/mock/sioc.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/driver.cpp"
    )
    target_link_libraries(${PROJECT_NAME}_test PRIVATE gtest_main lib${PROJECT_NAME})
    include(GoogleTest)
    gtest_discover_tests(${PROJECT_NAME}_test)
endif()

# Driver executable
if(MLDP_PVXS_DRIVER_MAIN)
    add_executable(${PROJECT_NAME}
            "${CMAKE_CURRENT_SOURCE_DIR}/ext/rapidyaml/include/rapidyaml-0.10.0.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/mldp_pvxs_driver_main.cpp"
    )
    target_link_libraries(${PROJECT_NAME} PRIVATE lib${PROJECT_NAME})
    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ext/rapidyaml/include")
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

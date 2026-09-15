cmake_minimum_required(VERSION 3.25)

project(FabricCapabilityRegistry
        VERSION 1.0.0
        DESCRIPTION "Authoritative capability knowledge runtime for the Distributed Fabric Infrastructure stack"
        HOMEPAGE_URL "https://github.com/pngen/Fabric-Capability-Registry"
        LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(FCR_BUILD_SHARED "Build Fabric Capability Registry as a shared library" OFF)
option(FCR_BUILD_TESTS "Build the Fabric Capability Registry test suite" ON)
option(FCR_BUILD_TOOLS "Build the fcrctl inspection CLI and the distributed tools" ON)
option(FCR_BUILD_EXAMPLES "Build the public API examples" ON)
option(FCR_BUILD_BENCHMARKS "Build the guarded benchmarks" OFF)
option(FCR_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(FCR_ENABLE_ANALYZE "Enable the platform static analyzer (/analyze on MSVC)" OFF)
option(FCR_ENABLE_ASAN "Enable AddressSanitizer (fails clearly when the toolchain lacks the runtime)" OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(FCR_LIBRARY_TYPE STATIC)
if(FCR_BUILD_SHARED)
  set(FCR_LIBRARY_TYPE SHARED)
endif()

# ---------------------------------------------------------------------------
# First-party warning policy. No warning is globally suppressed.
# ---------------------------------------------------------------------------

add_library(fcr_project_options INTERFACE)
target_compile_features(fcr_project_options INTERFACE cxx_std_20)
if(MSVC)
  target_compile_options(fcr_project_options INTERFACE
    /W4
    /permissive-
    /utf-8
    /Zc:__cplusplus
    /Zc:preprocessor
    /EHsc)
  if(FCR_WARNINGS_AS_ERRORS)
    target_compile_options(fcr_project_options INTERFACE /WX)
  endif()
  if(FCR_ENABLE_ANALYZE)
    target_compile_options(fcr_project_options INTERFACE /analyze)
  endif()
else()
  target_compile_options(fcr_project_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
  if(FCR_WARNINGS_AS_ERRORS)
    target_compile_options(fcr_project_options INTERFACE -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# AddressSanitizer. The configuration fails clearly when the toolchain cannot
# actually instrument, so that no build ever claims sanitizer coverage it does
# not have.
# ---------------------------------------------------------------------------

if(FCR_ENABLE_ASAN)
  if(MSVC)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "/fsanitize=address")
    check_cxx_source_compiles("#include <cstdio>\nint main(){std::printf(\"asan probe\\n\");return 0;}" FCR_MSVC_ASAN_AVAILABLE)
    unset(CMAKE_REQUIRED_FLAGS)
    if(NOT FCR_MSVC_ASAN_AVAILABLE)
      message(FATAL_ERROR
        "FCR_ENABLE_ASAN=ON but the MSVC toolchain cannot build with /fsanitize=address. "
        "Install the 'AddressSanitizer' component of the Visual Studio C++ workload, or "
        "configure with FCR_ENABLE_ASAN=OFF. AddressSanitizer is reported UNSUPPORTED in that case.")
    endif()
    add_compile_options(/fsanitize=address)
    add_link_options(/fsanitize=address)
  else()
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
  endif()
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

set(FCR_PUBLIC_HEADERS
  include/fabric/capability/authority.hpp
  include/fabric/capability/digest.hpp
  include/fabric/capability/discovery.hpp
  include/fabric/capability/distributed.hpp
  include/fabric/capability/encoding.hpp
  include/fabric/capability/error.hpp
  include/fabric/capability/evidence.hpp
  include/fabric/capability/fabric_capability.hpp
  include/fabric/capability/ids.hpp
  include/fabric/capability/limits.hpp
  include/fabric/capability/persistence.hpp
  include/fabric/capability/publication.hpp
  include/fabric/capability/query.hpp
  include/fabric/capability/record.hpp
  include/fabric/capability/registry.hpp
  include/fabric/capability/requirement.hpp
  include/fabric/capability/schema.hpp
  include/fabric/capability/state.hpp
  include/fabric/capability/synthetic.hpp
  include/fabric/capability/value.hpp
  include/fabric/capability/version.hpp)

set(FCR_SOURCES
  src/digest.cpp
  src/discovery.cpp
  src/distributed.cpp
  src/encoding.cpp
  src/error.cpp
  src/evidence.cpp
  src/ids.cpp
  src/persistence.cpp
  src/query.cpp
  src/registry.cpp
  src/requirement.cpp
  src/schema.cpp
  src/state.cpp
  src/synthetic.cpp
  src/value.cpp
  src/wire.cpp)

add_library(fabric_capability ${FCR_LIBRARY_TYPE} ${FCR_SOURCES} ${FCR_PUBLIC_HEADERS})
add_library(SummonSoftwareLabs::FabricCapabilityRegistry ALIAS fabric_capability)

set_target_properties(fabric_capability PROPERTIES
  OUTPUT_NAME FabricCapabilityRegistry
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

target_include_directories(fabric_capability PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# Private internal headers (store layout, internal codec boundaries) live
# outside the public include tree and are never installed.
target_include_directories(fabric_capability PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The warning policy target is a build tree concern only: it carries no
# compiled artifact and must not appear in the installed export set.
target_link_libraries(fabric_capability PRIVATE $<BUILD_INTERFACE:fcr_project_options>)
target_compile_definitions(fabric_capability PRIVATE FCR_BUILDING_LIBRARY=1)

if(WIN32)
  target_link_libraries(fabric_capability PRIVATE ws2_32 iphlpapi setupapi)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(fabric_capability PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

if(FCR_BUILD_TOOLS)
  add_executable(fcrctl tools/fcrctl/main.cpp)
  target_link_libraries(fcrctl PRIVATE SummonSoftwareLabs::FabricCapabilityRegistry fcr_project_options)

  add_executable(fcr_coordinator tools/fcr_coordinator/main.cpp)
  target_link_libraries(fcr_coordinator PRIVATE SummonSoftwareLabs::FabricCapabilityRegistry fcr_project_options)

  add_executable(fcr_publisher tools/fcr_publisher/main.cpp)
  target_link_libraries(fcr_publisher PRIVATE SummonSoftwareLabs::FabricCapabilityRegistry fcr_project_options)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------

if(FCR_BUILD_EXAMPLES)
  file(GLOB FCR_EXAMPLE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp)
  foreach(example_source IN LISTS FCR_EXAMPLE_SOURCES)
    get_filename_component(example_name ${example_source} NAME_WE)
    add_executable(${example_name} ${example_source})
    target_link_libraries(${example_name} PRIVATE SummonSoftwareLabs::FabricCapabilityRegistry fcr_project_options)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Tests. No test carries a timeout: a hanging test is a defect that must be
# diagnosed, never masked by a watchdog.
# ---------------------------------------------------------------------------

if(FCR_BUILD_TESTS)
  enable_testing()

  set(FCR_TEST_SOURCES
    tests/support/test_framework.cpp
    tests/support/process_helper.cpp
    tests/test_adversarial.cpp
    tests/test_authority.cpp
    tests/test_cli.cpp
    tests/test_concurrency.cpp
    tests/test_coordinator_restart.cpp
    tests/test_discovery.cpp
    tests/test_ids.cpp
    tests/test_persistence.cpp
    tests/test_property.cpp
    tests/test_publication.cpp
    tests/test_query.cpp
    tests/test_requirement.cpp
    tests/test_schema.cpp
    tests/test_synthetic.cpp
    tests/test_value.cpp
    tests/test_wire.cpp
    tests/test_distributed.cpp)

  add_executable(fcr_tests ${FCR_TEST_SOURCES})
  target_link_libraries(fcr_tests PRIVATE SummonSoftwareLabs::FabricCapabilityRegistry fcr_project_options)
  target_include_directories(fcr_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)

  if(TARGET fcr_coordinator)
    target_compile_definitions(fcr_tests PRIVATE
      FCR_COORDINATOR_EXE_PATH="$<TARGET_FILE:fcr_coordinator>"
      FCR_PUBLISHER_EXE_PATH="$<TARGET_FILE:fcr_publisher>"
      FCR_FCRCTL_EXE_PATH="$<TARGET_FILE:fcrctl>")
  endif()

  set(FCR_TEST_SUITES
    ids value schema publication authority query requirement persistence
    concurrency property adversarial discovery synthetic wire
    distributed coordinator_restart cli)

  foreach(suite IN LISTS FCR_TEST_SUITES)
    add_test(NAME fcr.${suite} COMMAND fcr_tests --filter=${suite})
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

if(FCR_BUILD_BENCHMARKS)
  add_executable(fcr_benchmarks benchmarks/bench_registry.cpp)
  target_link_libraries(fcr_benchmarks PRIVATE SummonSoftwareLabs::FabricCapabilityRegistry fcr_project_options)
endif()

# ---------------------------------------------------------------------------
# Install and package export
# ---------------------------------------------------------------------------

install(TARGETS fabric_capability
        EXPORT FabricCapabilityRegistryTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(FCR_BUILD_TOOLS)
  install(TARGETS fcrctl fcr_coordinator fcr_publisher
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(FCR_CMAKE_CONFIG_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FabricCapabilityRegistry)

install(EXPORT FabricCapabilityRegistryTargets
        FILE FabricCapabilityRegistryTargets.cmake
        NAMESPACE SummonSoftwareLabs::
        DESTINATION ${FCR_CMAKE_CONFIG_DESTINATION})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FabricCapabilityRegistryConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/FabricCapabilityRegistryConfig.cmake
  INSTALL_DESTINATION ${FCR_CMAKE_CONFIG_DESTINATION})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/FabricCapabilityRegistryConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/FabricCapabilityRegistryConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/FabricCapabilityRegistryConfigVersion.cmake
        DESTINATION ${FCR_CMAKE_CONFIG_DESTINATION})

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/README.md
              ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------

message(STATUS "Fabric Capability Registry ${PROJECT_VERSION}")
message(STATUS "  library type .......... ${FCR_LIBRARY_TYPE}")
message(STATUS "  tests ................. ${FCR_BUILD_TESTS}")
message(STATUS "  tools ................. ${FCR_BUILD_TOOLS}")
message(STATUS "  examples .............. ${FCR_BUILD_EXAMPLES}")
message(STATUS "  benchmarks ............ ${FCR_BUILD_BENCHMARKS}")
message(STATUS "  warnings as errors .... ${FCR_WARNINGS_AS_ERRORS}")
message(STATUS "  static analyzer ....... ${FCR_ENABLE_ANALYZE}")
message(STATUS "  address sanitizer ..... ${FCR_ENABLE_ASAN}")

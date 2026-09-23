cmake_minimum_required(VERSION 3.25)

project(LossObservatory
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral packet-loss observation, attribution, and localization runtime"
  HOMEPAGE_URL "https://github.com/summonlabs/Loss-Observatory"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(LO_BUILD_TOOLS "Build the locctl inspection tooling" ON)
option(LO_BUILD_TESTS "Build the Loss Observatory test suites" ON)
option(LO_BUILD_BENCHMARKS "Build the Loss Observatory benchmarks" ON)
option(LO_BUILD_EXAMPLES "Build the Loss Observatory examples" ON)
option(LO_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(LO_ENABLE_ASAN "Enable AddressSanitizer when the toolchain supports it" OFF)
option(LO_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer when the toolchain supports it" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Sanitizers
# ---------------------------------------------------------------------------
set(LO_SANITIZER_FLAGS "")
if(LO_ENABLE_ASAN)
  if(MSVC)
    list(APPEND LO_SANITIZER_FLAGS /fsanitize=address)
    set(LO_ASAN_ACTIVE ON)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    list(APPEND LO_SANITIZER_FLAGS -fsanitize=address -fno-omit-frame-pointer)
    set(LO_ASAN_ACTIVE ON)
  else()
    message(WARNING "LO_ENABLE_ASAN requested but compiler '${CMAKE_CXX_COMPILER_ID}' support is unknown; ASan disabled.")
    set(LO_ASAN_ACTIVE OFF)
  endif()
else()
  set(LO_ASAN_ACTIVE OFF)
endif()

if(LO_ENABLE_UBSAN)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    list(APPEND LO_SANITIZER_FLAGS -fsanitize=undefined -fno-sanitize-recover=undefined)
    set(LO_UBSAN_ACTIVE ON)
  else()
    message(WARNING "LO_ENABLE_UBSAN requested but compiler '${CMAKE_CXX_COMPILER_ID}' has no runtime UBSan support; UBSan disabled.")
    set(LO_UBSAN_ACTIVE OFF)
  endif()
else()
  set(LO_UBSAN_ACTIVE OFF)
endif()

if(MSVC)
  set(LO_ASAN_SUPPORTED TRUE)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(LO_ASAN_SUPPORTED TRUE)
else()
  set(LO_ASAN_SUPPORTED FALSE)
endif()

# ---------------------------------------------------------------------------
# Shared first-party warning policy
# ---------------------------------------------------------------------------
add_library(loss_observatory_warnings INTERFACE)
add_library(LossObservatory::Warnings ALIAS loss_observatory_warnings)

if(MSVC)
  # /FS is applied globally: MSBuild compiles one project with several CL.EXE
  # processes, and they must serialise their writes to the shared PDB.
  add_compile_options(/FS)
  target_compile_options(loss_observatory_warnings INTERFACE
    /W4
    /permissive-
    /Zc:__cplusplus
    /Zc:preprocessor
    /Zc:externConstexpr
    /Zc:inline
    /utf-8
    /EHsc
    /bigobj
    /FS # serialise PDB writes so parallel builds cannot clash
    /wd4820 # padding added by the compiler: reported per-type, not actionable here
  )
  if(LO_WARNINGS_AS_ERRORS)
    target_compile_options(loss_observatory_warnings INTERFACE /WX)
  endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(loss_observatory_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wcast-qual -Wdouble-promotion -Wformat=2 -Wundef -Wnon-virtual-dtor
    -Wold-style-cast -Woverloaded-virtual -Wnull-dereference -Wuseless-cast
  )
  if(LO_WARNINGS_AS_ERRORS)
    target_compile_options(loss_observatory_warnings INTERFACE -Werror)
  endif()
endif()

# Sanitizer flags are applied repository-wide rather than per target: mixing
# instrumented and uninstrumented objects in one link is rejected by the
# toolchain, and a partially instrumented build would weaken the evidence the
# sanitizer run is supposed to provide.
if(LO_SANITIZER_FLAGS)
  add_compile_options(${LO_SANITIZER_FLAGS})
  add_link_options(${LO_SANITIZER_FLAGS})
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(LO_PUBLIC_HEADERS
  include/loss_observatory/version.hpp
  include/loss_observatory/core/status.hpp
  include/loss_observatory/core/checked.hpp
  include/loss_observatory/core/hash.hpp
  include/loss_observatory/core/time.hpp
  include/loss_observatory/core/bytes.hpp
  include/loss_observatory/core/bounded.hpp
  include/loss_observatory/model/ids.hpp
  include/loss_observatory/model/granularity.hpp
  include/loss_observatory/model/topology.hpp
  include/loss_observatory/model/source.hpp
  include/loss_observatory/model/method.hpp
  include/loss_observatory/model/generation.hpp
  include/loss_observatory/evidence/evidence.hpp
  include/loss_observatory/evidence/counter.hpp
  include/loss_observatory/evidence/derive.hpp
  include/loss_observatory/evidence/store.hpp
  include/loss_observatory/freshness.hpp
  include/loss_observatory/classify.hpp
  include/loss_observatory/conflict.hpp
  include/loss_observatory/localize.hpp
  include/loss_observatory/episode.hpp
  include/loss_observatory/aggregate.hpp
  include/loss_observatory/persist.hpp
  include/loss_observatory/engine.hpp
  include/loss_observatory/report.hpp
  include/loss_observatory/script.hpp
  include/loss_observatory/testkit/testkit.hpp
)

set(LO_SOURCES
  src/version.cpp
  src/core/status.cpp
  src/core/time.cpp
  src/core/bytes.cpp
  src/core/bounded.cpp
  src/model/ids.cpp
  src/model/granularity.cpp
  src/model/topology.cpp
  src/model/source.cpp
  src/model/method.cpp
  src/model/generation.cpp
  src/evidence/counter.cpp
  src/evidence/evidence.cpp
  src/evidence/derive.cpp
  src/evidence/store.cpp
  src/freshness.cpp
  src/classify.cpp
  src/conflict.cpp
  src/localize.cpp
  src/episode.cpp
  src/aggregate.cpp
  src/persist.cpp
  src/engine.cpp
  src/report.cpp
  src/script.cpp
  src/testkit/testkit.cpp
)

add_library(loss_observatory ${LO_SOURCES})
add_library(LossObservatory::loss_observatory ALIAS loss_observatory)

target_include_directories(loss_observatory
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# The warning policy is a build-time-only dependency: it must not leak into
# the exported package, where no such target exists.
target_link_libraries(loss_observatory
  PUBLIC Threads::Threads
  PRIVATE $<BUILD_INTERFACE:LossObservatory::Warnings>
)

target_compile_definitions(loss_observatory
  PUBLIC
    LO_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    LO_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    LO_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    $<$<BOOL:${LO_ASAN_ACTIVE}>:LO_WITH_ASAN=1>
    $<$<BOOL:${LO_UBSAN_ACTIVE}>:LO_WITH_UBSAN=1>
    $<$<BOOL:${LO_ASAN_SUPPORTED}>:LO_ASAN_SUPPORTED=1>
    $<$<BOOL:${LO_UBSAN_ACTIVE}>:LO_UBSAN_SUPPORTED=1>
)

set_target_properties(loss_observatory PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS loss_observatory
  EXPORT LossObservatoryTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT LossObservatoryTargets
  FILE LossObservatoryTargets.cmake
  NAMESPACE LossObservatory::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LossObservatory
)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/LossObservatoryConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/LossObservatoryConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LossObservatory
)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/LossObservatoryConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/LossObservatoryConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/LossObservatoryConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LossObservatory
)

# ---------------------------------------------------------------------------
# Tools, examples, benchmarks, tests
# ---------------------------------------------------------------------------
if(LO_BUILD_TOOLS)
  add_subdirectory(tools/locctl)
endif()

if(LO_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(LO_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()

if(LO_BUILD_TESTS AND BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "Loss Observatory ${PROJECT_VERSION}")
message(STATUS "  build type ............ ${CMAKE_BUILD_TYPE}${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  compiler .............. ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  warnings as errors .... ${LO_WARNINGS_AS_ERRORS}")
message(STATUS "  AddressSanitizer ...... ${LO_ASAN_ACTIVE} (supported: ${LO_ASAN_SUPPORTED})")
message(STATUS "  UndefinedBehaviorSan .. ${LO_UBSAN_ACTIVE}")
message(STATUS "  tools ................. ${LO_BUILD_TOOLS}")
message(STATUS "  tests ................. ${LO_BUILD_TESTS}")
message(STATUS "  benchmarks ............ ${LO_BUILD_BENCHMARKS}")
message(STATUS "  examples .............. ${LO_BUILD_EXAMPLES}")

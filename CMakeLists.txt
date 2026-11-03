cmake_minimum_required(VERSION 3.20)
project(lightmusic VERSION 0.1.0 LANGUAGES C CXX)

# C++23: std::print/std::println and std::format (CLAUDE.md "Output"). gcc >= 14 / clang >= 19
# with libstdc++ >= 14 provide <print>; Fedora 43 ships gcc 15 / clang 21.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Install layout (bin/ under CMAKE_INSTALL_PREFIX): `cmake --install build`.
include(GNUInstallDirs)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(LM_ASAN "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(LM_TSAN "Build with ThreadSanitizer" OFF)
option(LM_BUILD_TESTS "Build the tests" ON)
option(LM_BUILD_TUI "Build the lightmusic TUI (needs FTXUI >= 6)" ON)
option(LM_BUILD_NET "Build libnet: server client, streaming and offline cache (needs libcurl, SQLite)" ON)
option(LM_BUILD_SERVER "Build the lightmusicd server (needs TagLib, SQLite)" ON)
option(LM_BUILD_MPRIS "Build MPRIS (media keys / desktop integration) for the TUI (needs libsystemd)" ON)
option(LM_BUILD_BENCH "Build the benchmarks on the project's libraries (bench/lm)" OFF)
option(LM_FETCH_FTXUI "Fetch FTXUI v6.1.9 with FetchContent if no system package" OFF)
option(LM_HARDENING "Release hardening: _FORTIFY_SOURCE=3, stack protector, stack-clash protection" ON)
option(LM_ASSERTS "Force LM_ASSERT checks on in optimised builds (they are always on without NDEBUG)" OFF)
option(LM_GC_SECTIONS "Link the shipped executables with --gc-sections in optimised builds (drops unused code)" ON)

if(LM_ASAN AND LM_TSAN)
  message(FATAL_ERROR "LM_ASAN and LM_TSAN are mutually exclusive")
endif()
include(cmake/LmQuality.cmake)

# Release link flags for the shipped executables (lightmusic, lightmusicd), not for
# libraries' consumers in general: libconfig compiles with per-function sections, and
# --gc-sections drops the functions we never call.
function(lm_release_link_options target)
  if(LM_GC_SECTIONS)
    target_link_options(${target} PRIVATE $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-Wl,--gc-sections>)
  endif()
endfunction()

# Project assertions (include/lm/assert.hpp); every target gets them through lm_warnings_app/_test
# or lm::player.
add_library(lm_assert INTERFACE)
target_include_directories(lm_assert INTERFACE ${PROJECT_SOURCE_DIR}/include)
if(LM_ASSERTS)
  target_compile_definitions(lm_assert INTERFACE LM_ENABLE_ASSERTS)
endif()
target_link_libraries(lm_warnings_app INTERFACE lm_assert)
target_link_libraries(lm_warnings_test INTERFACE lm_assert)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUSFILE REQUIRED IMPORTED_TARGET opusfile)
pkg_check_modules(VORBISFILE REQUIRED IMPORTED_TARGET vorbisfile)

add_subdirectory(libplayer)
add_subdirectory(libart)
add_subdirectory(libconfig)
if(LM_BUILD_NET)
  add_subdirectory(libnet)
endif()
if(LM_BUILD_MPRIS)
  pkg_check_modules(LIBSYSTEMD QUIET IMPORTED_TARGET libsystemd)
  if(LIBSYSTEMD_FOUND)
    add_subdirectory(frontends/mpris)
  else()
    message(STATUS "MPRIS disabled: libsystemd not found (dnf install systemd-devel)")
  endif()
endif()
add_subdirectory(tools)
if(LM_BUILD_BENCH)
  add_subdirectory(bench/lm)
endif()
if(LM_BUILD_TUI)
  add_subdirectory(frontends/tui)
endif()
if(LM_BUILD_SERVER)
  add_subdirectory(server)
endif()

if(LM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

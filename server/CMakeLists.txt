# lightmusicd: library index (TagLib + SQLite + inotify) and HTTP API (cpp-httplib).
pkg_check_modules(TAGLIB REQUIRED IMPORTED_TARGET taglib)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)

# cpp-httplib: the system header if it is at least the vendored version (Fedora ships
# 0.48.0 as /usr/include/httplib.h, without a pkg-config file), else third_party/.
set(LM_HTTPLIB_PINNED 0.48.0)
option(LM_SYSTEM_HTTPLIB "Prefer a system cpp-httplib header" ON)
set(LM_HTTPLIB_DIR ${PROJECT_SOURCE_DIR}/third_party/cpp-httplib)
if(LM_SYSTEM_HTTPLIB)
  find_path(LM_HTTPLIB_SYSTEM_DIR httplib.h NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH)
  if(LM_HTTPLIB_SYSTEM_DIR)
    file(STRINGS ${LM_HTTPLIB_SYSTEM_DIR}/httplib.h _v REGEX "#define CPPHTTPLIB_VERSION \"")
    string(REGEX REPLACE ".*\"([0-9.]+)\".*" "\\1" _v "${_v}")
    if(_v VERSION_GREATER_EQUAL LM_HTTPLIB_PINNED)
      set(LM_HTTPLIB_DIR ${LM_HTTPLIB_SYSTEM_DIR})
      message(STATUS "cpp-httplib: system ${_v} (${LM_HTTPLIB_SYSTEM_DIR})")
    else()
      message(STATUS "cpp-httplib: system ${_v} is older than ${LM_HTTPLIB_PINNED}; using vendored")
    endif()
  endif()
endif()
if(LM_HTTPLIB_DIR STREQUAL ${PROJECT_SOURCE_DIR}/third_party/cpp-httplib)
  message(STATUS "cpp-httplib: vendored ${LM_HTTPLIB_PINNED}")
endif()
add_library(lm_httplib INTERFACE)
target_include_directories(lm_httplib SYSTEM INTERFACE ${LM_HTTPLIB_DIR})
target_link_libraries(lm_httplib INTERFACE Threads::Threads)

add_library(lm_server_core STATIC
  src/auth.cpp src/range.cpp src/config.cpp src/db.cpp src/tags.cpp
  src/indexer.cpp src/watcher.cpp src/safe_open.cpp)
add_library(lm::server_core ALIAS lm_server_core)
target_include_directories(lm_server_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lm_server_core
  PUBLIC PkgConfig::SQLITE3 Threads::Threads
  PRIVATE lm_warnings PkgConfig::TAGLIB)

# api.cpp is the only TU that includes httplib.h (slow to compile).
add_executable(lightmusicd src/main.cpp src/api.cpp)
target_link_libraries(lightmusicd PRIVATE lm_server_core lm_httplib lm_warnings PkgConfig::TAGLIB)
install(TARGETS lightmusicd RUNTIME DESTINATION bin)

# GCC 13 with -fsanitize=address reports a false -Wmaybe-uninitialized inside
# <bits/std_function.h> for the std::regex code httplib.h instantiates in our TU.
if(LM_ASAN AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set_source_files_properties(src/api.cpp PROPERTIES COMPILE_OPTIONS -Wno-maybe-uninitialized)
endif()

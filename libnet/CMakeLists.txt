# libnet: client side of lightmusicd — HTTP (libcurl multi, one network thread), library
# mirror and offline cache (SQLite + sparse files), CachedStreamSource for libplayer.
# UI-free; the player core does not depend on it (frontends wire it in through
# lm::PlayerConfig::openSource).
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl>=7.68)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
option(LM_WITH_SDBUS "Detect metered networks via NetworkManager over sd-bus (libsystemd)" ON)
if(LM_WITH_SDBUS)
  pkg_check_modules(SDBUS QUIET IMPORTED_TARGET libsystemd)
endif()

add_library(net STATIC
  src/range_set.cpp
  src/json.cpp
  src/http.cpp
  src/client_db.cpp
  src/metered.cpp
  src/client.cpp
)
add_library(lm::net ALIAS net)
target_include_directories(net PUBLIC include PRIVATE src)
target_link_libraries(net
  PUBLIC lm::player Threads::Threads
  PRIVATE lm_warnings PkgConfig::CURL PkgConfig::SQLITE3)
if(SDBUS_FOUND)
  target_compile_definitions(net PRIVATE LM_HAVE_SDBUS=1)
  target_link_libraries(net PRIVATE PkgConfig::SDBUS)
  message(STATUS "libnet: metered detection via sd-bus (libsystemd ${SDBUS_VERSION})")
else()
  message(STATUS "libnet: no libsystemd; networks are treated as unmetered")
endif()

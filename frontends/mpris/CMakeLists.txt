# libmpris: MPRIS 2 over sd-bus (libsystemd). Optional: LM_BUILD_MPRIS, auto-off when
# libsystemd is missing (see the top-level CMakeLists.txt).
add_library(mpris STATIC src/mpris.cpp)
add_library(lm::mpris ALIAS mpris)
target_include_directories(mpris PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mpris PUBLIC lm::player PkgConfig::LIBSYSTEMD PRIVATE lm_warnings)

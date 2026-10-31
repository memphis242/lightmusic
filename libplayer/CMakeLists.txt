# Vendored single-header implementations, compiled once, without our warning flags.
add_library(lm_third_party STATIC
  src/miniaudio_impl.c
  src/dr_impl.c
)
target_include_directories(lm_third_party PUBLIC ${PROJECT_SOURCE_DIR}/third_party)
target_compile_options(lm_third_party PRIVATE -w)
target_link_libraries(lm_third_party PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)

add_library(player STATIC
  src/source.cpp
  src/decoder.cpp
  src/dec_mp3.cpp
  src/dec_flac.cpp
  src/dec_opus.cpp
  src/dec_vorbis.cpp
  src/tags.cpp
  src/output.cpp
  src/player.cpp
)
add_library(lm::player ALIAS player)
target_include_directories(player
  PUBLIC include
  PRIVATE src)
target_link_libraries(player
  PUBLIC Threads::Threads
  PRIVATE lm_warnings lm_third_party PkgConfig::OPUSFILE PkgConfig::VORBISFILE)

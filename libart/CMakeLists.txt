# libart: cover-art thumbnails. Decodes JPEG/PNG with the vendored stb_image, makes the
# 64x64 RGB album thumbnails, keeps the on-disk thumbnail cache and turns a thumbnail
# into half-block terminal cells. UI-free; used by the TUI and lightmusicd.

# stb_image (third_party/stb_image.h), compiled once, without our warning flags, like the
# other vendored code. JPEG and PNG only, no stdio (src/lm_stb_image.h has the options).
add_library(lm_stb STATIC src/stb_impl.c)
target_include_directories(lm_stb SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party)
target_compile_options(lm_stb PRIVATE -w)
# stb_image truncates integers on purpose (bit readers, colour conversion): keep clang's
# implicit-integer-truncation check to our own code, as for lm_third_party.
if(LM_ASAN AND CMAKE_C_COMPILER_ID MATCHES "Clang")
  target_compile_options(lm_stb PRIVATE -fno-sanitize=implicit-integer-truncation)
endif()
target_link_libraries(lm_stb PUBLIC m)

add_library(art STATIC
  src/thumb.cpp
  src/store.cpp
)
add_library(lm::art ALIAS art)
target_include_directories(art PUBLIC include PRIVATE src)
target_link_libraries(art
  PUBLIC lm_assert
  PRIVATE lm_warnings_app lm_stb lm::player)

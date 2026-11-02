set(LM_FIXTURE_DIR ${CMAKE_CURRENT_BINARY_DIR}/fixtures)

function(lm_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE lm::player lm_warnings_test)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 120
    ENVIRONMENT "LM_FIXTURE_DIR=${LM_FIXTURE_DIR}")
endfunction()

lm_test(test_ring)
lm_test(test_print)
# Assertion facility death tests: header-only (no lm::player, so forcing assertions on
# here cannot give inline functions two definitions), on in every build type.
add_executable(test_assert test_assert.cpp)
target_link_libraries(test_assert PRIVATE lm_warnings_test)
target_include_directories(test_assert PRIVATE ${PROJECT_SOURCE_DIR}/libplayer/include)
target_compile_definitions(test_assert PRIVATE LM_ENABLE_ASSERTS)
add_test(NAME test_assert COMMAND test_assert)
set_tests_properties(test_assert PROPERTIES TIMEOUT 60)
lm_test(test_decoders)
lm_test(test_player)
lm_test(test_tags)
lm_test(test_tags_files)
lm_test(test_config)
lm_test(test_art)
target_link_libraries(test_art PRIVATE lm::art)
target_link_libraries(test_config PRIVATE lm::config)
# Unit tests of internal tag parsers need the private headers.
target_include_directories(test_decoders PRIVATE ${PROJECT_SOURCE_DIR}/libplayer/src)
target_include_directories(test_tags PRIVATE ${PROJECT_SOURCE_DIR}/libplayer/src)
# The gapless test resamples its reference with miniaudio's converter, like the player.
target_include_directories(test_player SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/libplayer/src ${PROJECT_SOURCE_DIR}/third_party)
target_link_libraries(test_player PRIVATE lm_third_party)

if(TARGET lightmusic)
  lm_test(test_tui_logic)
  target_link_libraries(test_tui_logic PRIVATE lm::tui_logic)
  lm_test(test_tui_art)
  target_link_libraries(test_tui_art PRIVATE lm::tui_art)
  add_test(NAME test_tui_smoke COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/test_tui_smoke.sh $<TARGET_FILE:lightmusic>)
  set_tests_properties(test_tui_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 240)
  # Browser: folders always; the server library when the TUI has libnet and the server exists.
  if(TARGET lm::net AND TARGET lightmusicd)
    add_test(NAME test_tui_browse COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/test_tui_browse.sh
                                          $<TARGET_FILE:lightmusic> $<TARGET_FILE:lightmusicd>)
  else()
    add_test(NAME test_tui_browse COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/test_tui_browse.sh $<TARGET_FILE:lightmusic>)
  endif()
  set_tests_properties(test_tui_browse PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()

if(TARGET lightmusicd)
  function(lm_server_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE lm::server_core lm_warnings_test)
    add_test(NAME ${name} COMMAND ${name} ${ARGN})
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
  endfunction()
  lm_server_test(test_server_range)
  lm_server_test(test_server_auth)
  lm_server_test(test_server_index)
  lm_server_test(test_server_config)
  lm_server_test(test_server_e2e $<TARGET_FILE:lightmusicd>)
  target_link_libraries(test_server_e2e PRIVATE lm_httplib)
endif()
if(TARGET test_server_e2e AND LM_ASAN AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # Same GCC false positive as server/src/api.cpp (std::regex via httplib.h).
  set_source_files_properties(test_server_e2e.cpp PROPERTIES COMPILE_OPTIONS -Wno-maybe-uninitialized)
endif()

if(TARGET lm::net)
  lm_test(test_range_set)
  target_link_libraries(test_range_set PRIVATE lm::net)
  lm_test(test_cache_names)
  target_link_libraries(test_cache_names PRIVATE lm::net)
  target_include_directories(test_cache_names PRIVATE ${PROJECT_SOURCE_DIR}/libnet/src)
  if(TARGET lm_httplib)
    # Cache tests use an in-process cpp-httplib server (tests/fake_server.hpp).
    lm_test(test_cache)
    target_link_libraries(test_cache PRIVATE lm::net lm_httplib)
    target_include_directories(test_cache PRIVATE ${PROJECT_SOURCE_DIR}/libnet/src)
    set_tests_properties(test_cache PROPERTIES TIMEOUT 300)
  endif()
  if(TARGET lightmusicd)
    # Real lightmusicd + Player on the Null backend, then offline playback.
    lm_test(test_stream_e2e)
    target_link_libraries(test_stream_e2e PRIVATE lm::net)
    set_tests_properties(test_stream_e2e PROPERTIES TIMEOUT 300)
    set_property(TEST test_stream_e2e APPEND PROPERTY ENVIRONMENT "LM_SERVER_BIN=$<TARGET_FILE:lightmusicd>")
  endif()
  if(LM_ASAN AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND TARGET test_cache)
    # Same GCC false positive as server/src/api.cpp (std::regex via httplib.h).
    set_source_files_properties(test_cache.cpp PROPERTIES COMPILE_OPTIONS -Wno-maybe-uninitialized)
  endif()
  if(LM_ASAN AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND TARGET test_stream_e2e)
    # GCC 13 + ASan: same -Wmaybe-uninitialized false positive in the std library headers.
    set_source_files_properties(test_stream_e2e.cpp PROPERTIES COMPILE_OPTIONS -Wno-maybe-uninitialized)
  endif()
endif()

if(TARGET lm::mpris)
  add_executable(test_mpris test_mpris.cpp)
  target_link_libraries(test_mpris PRIVATE lm::mpris lm_warnings_test)
  add_test(NAME test_mpris COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/test_mpris.sh $<TARGET_FILE:test_mpris>)
  set_tests_properties(test_mpris PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120
                       ENVIRONMENT "LM_FIXTURE_DIR=${LM_FIXTURE_DIR}")
  if(TARGET lightmusic)
    add_test(NAME test_tui_mpris COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/test_tui_mpris.sh $<TARGET_FILE:lightmusic>)
    set_tests_properties(test_tui_mpris PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)
  endif()
endif()

# Compile-fail tests: constraints (concepts, deleted copies) must reject misuse at compile
# time. Each case is an EXCLUDE_FROM_ALL object library that the test builds; it passes
# when the build fails with the expected diagnostic (a build that succeeds prints no
# match). RESOURCE_LOCK: they drive the build tool on this build tree, one at a time.
function(lm_compile_fail name src case regex)
  add_library(${name} OBJECT EXCLUDE_FROM_ALL compile_fail/${src})
  target_compile_definitions(${name} PRIVATE LM_CF_CASE=${case})
  target_link_libraries(${name} PRIVATE lm_warnings_test ${ARGN})
  add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ${name})
  set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}" RESOURCE_LOCK lm_build_tree TIMEOUT 120
                                          LABELS compile_fail)
endfunction()
lm_compile_fail(cf_int_field_bool config_fields.cpp 1 "IntFieldValue" lm::config)
lm_compile_fail(cf_int_field_double config_fields.cpp 2 "IntFieldValue" lm::config)
lm_compile_fail(cf_enum_field_int config_fields.cpp 3 "EnumFieldValue" lm::config)
lm_compile_fail(cf_bytesource_copy interfaces.cpp 1 "deleted" lm::player)
lm_compile_fail(cf_decoder_copy interfaces.cpp 2 "deleted" lm::player)
# gcc names the type 'lm::Frames', clang 'Frames'.
lm_compile_fail(cf_frames_from_int frames.cpp 1 "to '(lm::)?Frames'" lm::player)
lm_compile_fail(cf_frames_to_int frames.cpp 2 "'(lm::)?Frames' to" lm::player)
# gcc names the type 'lm::TrackId', clang 'lm::TrackId' or 'TrackId'.
lm_compile_fail(cf_trackid_from_int track_id.cpp 1 "to '(const )?(lm::)?TrackId'" lm::player)
lm_compile_fail(cf_trackid_to_int track_id.cpp 2 "'(const )?(lm::)?TrackId' to" lm::player)
lm_compile_fail(cf_trackid_arith track_id.cpp 3 "operator\\+|invalid operands" lm::player)
if(TARGET lightmusicd)
  lm_compile_fail(cf_json_kv_size_t interfaces.cpp 3 "JsonScalar" lm::player lm::server_core)
  lm_compile_fail(cf_paging_swapped paging.cpp 1 "Page(Offset|Limit)" lm::server_core)
  lm_compile_fail(cf_paging_int paging.cpp 2 "Page(Offset|Limit)" lm::server_core)
endif()

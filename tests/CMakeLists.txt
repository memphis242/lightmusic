set(LM_FIXTURE_DIR ${CMAKE_CURRENT_BINARY_DIR}/fixtures)

function(lm_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE lm::player lm_warnings)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 120
    ENVIRONMENT "LM_FIXTURE_DIR=${LM_FIXTURE_DIR}")
endfunction()

lm_test(test_ring)
lm_test(test_decoders)
lm_test(test_player)
lm_test(test_tags)
lm_test(test_tags_files)
# Unit tests of internal tag parsers need the private headers.
target_include_directories(test_decoders PRIVATE ${PROJECT_SOURCE_DIR}/libplayer/src)
target_include_directories(test_tags PRIVATE ${PROJECT_SOURCE_DIR}/libplayer/src)
# The gapless test resamples its reference with miniaudio's converter, like the player.
target_include_directories(test_player SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/libplayer/src ${PROJECT_SOURCE_DIR}/third_party)
target_link_libraries(test_player PRIVATE lm_third_party)

if(TARGET lightmusic)
  lm_test(test_tui_logic)
  target_link_libraries(test_tui_logic PRIVATE lm::tui_logic)
  add_test(NAME test_tui_smoke COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/test_tui_smoke.sh $<TARGET_FILE:lightmusic>)
  set_tests_properties(test_tui_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()

if(TARGET lightmusicd)
  function(lm_server_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE lm::server_core lm_warnings)
    add_test(NAME ${name} COMMAND ${name} ${ARGN})
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
  endfunction()
  lm_server_test(test_server_range)
  lm_server_test(test_server_auth)
  lm_server_test(test_server_index)
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

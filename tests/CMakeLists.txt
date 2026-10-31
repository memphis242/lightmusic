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

add_executable(lightmusic-play lightmusic_play.cpp)
target_link_libraries(lightmusic-play PRIVATE lm::player lm_warnings)

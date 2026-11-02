# Benchmarks on the project's libraries (LM_BUILD_BENCH, off by default).
add_executable(lightmusic-bench-decode decode_bench.cpp)
target_link_libraries(lightmusic-bench-decode PRIVATE lm::player lm_warnings_app)
add_executable(lightmusic-bench-art art_bench.cpp)
target_link_libraries(lightmusic-bench-art PRIVATE lm::art lm_warnings_app)

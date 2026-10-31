# libconfig: YAML configuration (layers, typed schema, permission checks, inotify
# watcher). UI-free; used by the TUI and lightmusicd. Parser: system yaml-cpp (shared),
# seen only by src/yaml.cpp.
find_package(yaml-cpp 0.6 REQUIRED)

add_library(lm_config STATIC src/config.cpp src/yaml.cpp src/client.cpp)
add_library(lm::config ALIAS lm_config)
target_include_directories(lm_config PUBLIC include)
target_link_libraries(lm_config PRIVATE lm_warnings yaml-cpp::yaml-cpp)
target_compile_options(lm_config PRIVATE -ffunction-sections -fdata-sections)

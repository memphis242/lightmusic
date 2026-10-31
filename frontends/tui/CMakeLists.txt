# lightmusic TUI. Uses the system FTXUI (Fedora: ftxui-devel 6.x) when found; otherwise,
# with LM_FETCH_FTXUI=ON, fetches FTXUI v6.1.9 (needs network) and links it statically.
find_package(ftxui 6 CONFIG QUIET)
if(ftxui_FOUND)
  message(STATUS "lightmusic TUI: system FTXUI ${ftxui_VERSION}")
elseif(LM_FETCH_FTXUI)
  include(FetchContent)
  set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(FTXUI_QUIET ON CACHE BOOL "" FORCE)
  FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
    GIT_TAG v6.1.9
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(ftxui)
  message(STATUS "lightmusic TUI: fetched FTXUI v6.1.9 (static)")
else()
  message(FATAL_ERROR "FTXUI >= 6 not found: install ftxui-devel, pass -DLM_FETCH_FTXUI=ON, "
                      "or disable the TUI with -DLM_BUILD_TUI=OFF")
endif()

# Non-UI logic, unit-tested without FTXUI.
add_library(tui_logic STATIC tui_logic.cpp)
add_library(lm::tui_logic ALIAS tui_logic)
target_include_directories(tui_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tui_logic PRIVATE lm_warnings)

add_executable(lightmusic main.cpp)
lm_release_link_options(lightmusic)
target_link_libraries(lightmusic PRIVATE lm::tui_logic lm::player lm::config ftxui::screen ftxui::dom
                                         lm_warnings)
if(TARGET lm::net)
  target_compile_definitions(lightmusic PRIVATE LM_TUI_NET=1)
  target_link_libraries(lightmusic PRIVATE lm::net)
endif()

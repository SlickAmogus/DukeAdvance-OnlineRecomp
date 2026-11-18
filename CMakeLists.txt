cmake_minimum_required(VERSION 3.20)
project(DukeAdvanceRecomp C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(GBARECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../gbarecomp"
    CACHE PATH "Path to the gbarecomp framework checkout")

# Shared in-game settings menu (mstan/recomp-ui, MIT). Must be set before the
# framework is added: it switches the runtime's menu adapter on.
set(GBARECOMP_RUNTIME_UI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../recomp-ui"
    CACHE PATH "recomp-ui checkout")
set(RECOMP_UI_ROOT "${GBARECOMP_RUNTIME_UI_ROOT}" CACHE PATH "")

# Framework: runtime, hardware model, debug server. Tools/tests stay unbuilt.
add_subdirectory("${GBARECOMP_ROOT}" gbarecomp EXCLUDE_FROM_ALL)
include("${CMAKE_CURRENT_BINARY_DIR}/gbarecomp/runtime.cmake")

gbarecomp_add_runtime_target(DukeAdvanceRecomp
    GAME_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated"
    DEBUG_PORT 19850
    WINDOW_TITLE "Duke Nukem Advance"
    DEFAULT_GAME_CONFIG_PATH "game.toml"
)
target_sources(DukeAdvanceRecomp PRIVATE src/main.cpp src/debug_watch.cpp src/pc_controls.cpp)
target_include_directories(DukeAdvanceRecomp PRIVATE
    "${RECOMP_UI_ROOT}/src" ${SDL2_INCLUDE_DIR})
# Executable icon (assets/icon.ico, made from assets/icon.png). SDL also uses
# the exe's first icon resource for the window and taskbar.
if(WIN32)
    enable_language(RC)
    target_sources(DukeAdvanceRecomp PRIVATE assets/icon.rc)
endif()
# The runtime archives reference each other; GNU ld needs them grouped.
target_link_gbarecomp_runtime_stack(DukeAdvanceRecomp)

# Dear ImGui core + SDL2 backend + the menu model. recomp-ui is C++17 while
# the generated cartridge code is C++20, so it gets its own static library.
# (recomp_ui.cmake is included again here: the framework included it in its
# own directory scope, which hides recomp-ui's asset tables from this one.)
include("${RECOMP_UI_ROOT}/recomp_ui.cmake")
add_library(game_ui STATIC src/game_ui.cpp)
recomp_target_launcher_ui(game_ui CONSOLE gba)
target_include_directories(game_ui PRIVATE ${SDL2_INCLUDE_DIR})
target_link_libraries(game_ui PUBLIC ${SDL2_LIBRARY})
if(WIN32)
    target_link_libraries(game_ui PUBLIC opengl32)
endif()
target_link_libraries(DukeAdvanceRecomp PRIVATE game_ui)
recomp_stage_launcher_assets(DukeAdvanceRecomp CONSOLE gba)

# runtime.cpp reads the title at its own compile time.
target_compile_definitions(gbarecomp_runtime PRIVATE
    GBARECOMP_WINDOW_TITLE="Duke Nukem Advance")

gbarecomp_stage_mingw_runtime(DukeAdvanceRecomp)

# Runtime config sits next to the exe.
add_custom_command(TARGET DukeAdvanceRecomp POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/game.toml"
        "$<TARGET_FILE_DIR:DukeAdvanceRecomp>/game.toml"
    VERBATIM)

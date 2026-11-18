#pragma once
#include <string>
#include <vector>

#include "runtime.h"

// Installs mouse aim, the WASD/mouse-button layout and their settings-menu
// rows into the run options. Call once before gbarecomp::run_game().
void duke_pc_controls_install(gbarecomp::RunOptions* opts, const char* argv0);

// Debug TCP only: feed synthetic horizontal mouse counts (headless testing).
void duke_pc_controls_debug_mouse(int dx);

// Remembered display choices (fullscreen mode, scaling filter) as runtime CLI
// flags. Call after duke_pc_controls_install(); skip for scripted runs.
void duke_pc_controls_startup_args(std::vector<std::string>* extra);

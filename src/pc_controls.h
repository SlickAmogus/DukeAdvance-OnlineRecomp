#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "runtime.h"

// Installs mouse aim, the WASD/mouse-button layout and their settings-menu
// rows into the run options. Call once before gbarecomp::run_game().
void duke_pc_controls_install(gbarecomp::RunOptions* opts, const char* argv0);

// Guest address of the player's entity once it has been identified for the
// current stretch of gameplay, otherwise 0.
std::uint32_t duke_pc_controls_player_entity();

// Mouse movement not yet handed to the game (it is applied once per game
// tick): turn in angle units (0x10000 = 360 degrees, + = right) and look in
// horizon pixels (+ = up). The renderer adds it so the view follows the mouse
// every display frame.
void duke_pc_controls_pending_aim(double* yaw_units, double* pitch_pixels);

// Debug TCP only: feed synthetic mouse counts (headless testing).
void duke_pc_controls_debug_mouse(int dx, int dy);

// Remembered display choices (fullscreen mode, scaling filter) as runtime CLI
// flags. Call after duke_pc_controls_install(); skip for scripted runs.
void duke_pc_controls_startup_args(std::vector<std::string>* extra);

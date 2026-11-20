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

// Debug TCP only: a synthetic pointer for the game's menus (headless testing).
// x, y in GBA screen pixels; clicks and wheel notches are added to the queue.
void duke_pc_controls_debug_pointer(int x, int y, int left_clicks, int right_clicks, int wheel);

// ── for the PC menus drawn over the game (pc_menu.cpp) ──
// The game's menu running right now (ROM address of its record) and its
// selected item; false when no menu is live.
bool duke_pc_controls_menu(std::uint32_t* menu, int* selected);
// Where the mouse is on the GBA screen (the game's own picture); false when
// it is not over the game window.
bool duke_pc_controls_pointer(const gbarecomp::TouchFrameInfo* info, float* x, float* y);
// Clicks and wheel notches not yet handled. take_ consumes them.
void duke_pc_controls_peek_mouse(int* left, int* right, int* wheel);
void duke_pc_controls_take_mouse(int* left, int* right, int* wheel);
// Called first in every input frame. Returning true means the PC menus own
// this frame's input; *keys is then all the game gets.
void duke_pc_controls_set_overlay_input(bool (*fn)(const gbarecomp::TouchFrameInfo*, std::uint16_t*));
// The PC settings (the same rows as Esc > Graphics / Controls).
struct RecompRuntimeUiItem;
const RecompRuntimeUiItem* duke_pc_controls_items(int* count);
int duke_pc_controls_get(const char* key, int* value);
int duke_pc_controls_set(const char* key, int value);
// True while the game is in live play (3D view, player ticking).
bool duke_pc_controls_in_gameplay();

// Remembered display choices (fullscreen mode, scaling filter) as runtime CLI
// flags. Call after duke_pc_controls_install(); skip for scripted runs.
void duke_pc_controls_startup_args(std::vector<std::string>* extra);

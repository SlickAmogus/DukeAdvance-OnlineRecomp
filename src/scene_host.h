#pragma once
#include "runtime.h"

// Player-facing native renderer settings (owned by scene_host.cpp, saved by
// pc_controls.cpp).
struct DukeSceneSettings {
    int native = 1;          // 0 = the game's own 240x160 picture
    int resolution = 0;      // 0 = window resolution, then 75 / 50 / 33 / 25 %
    int fov_percent = 100;   // 100 = the game's field of view
};
DukeSceneSettings* duke_scene_settings();

// Installs the present-time compositor. Call before gbarecomp::run_game().
void duke_scene_install(gbarecomp::RunOptions* opts);

// Call once per emulated frame (keeps the scene hook armed).
void duke_scene_frame_start();

// Debug TCP only: write the current composite as a PPM. False when the game
// is not in its 3D view.
bool duke_scene_debug_shot(const char* path, int width, int height);

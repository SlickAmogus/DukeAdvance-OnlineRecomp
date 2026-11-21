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

// Link matches: the native renderer from a read-only hook (the consoles must
// stay identical on both PCs, so nothing changes what the game runs).
namespace gbarecomp { struct GbaNetplayLaunch; }
void duke_scene_install_match(gbarecomp::GbaNetplayLaunch* launch);

// Call once per emulated frame (keeps the scene hook armed).
void duke_scene_frame_start();

// Debug TCP only: the camera the most recent composite was drawn from.
struct DukeSceneView { float x = 0, y = 0, z = 0; unsigned angle = 0; };
DukeSceneView duke_scene_debug_view();

// Debug TCP only: write the current composite as a PPM. False when the game
// is not in its 3D view.
bool duke_scene_debug_shot(const char* path, int width, int height);

// True while one of the game's own menus is on screen: sprites along the top
// then stay centred instead of following the window edges like the HUD.
void duke_scene_set_menu_active(bool active);

// Number of frames presented by the native renderer so far. While it keeps
// rising the picture fills the window: 160 lines tall, 240 columns centred.
unsigned duke_scene_native_presents();

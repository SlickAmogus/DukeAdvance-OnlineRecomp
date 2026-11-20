// PC menus drawn over the game in its own style.
//
//   PC SETTINGS   a row added under GAMMA in the game's OPTIONS list: mouse
//                 aim, sensitivity, WASD, native renderer, resolution, field
//                 of view... (the same settings as Esc > Graphics / Controls)
//   ONLINE        MULTIPLAYER on the main menu (the game keeps it switched off
//                 without a link cable): host a room, find rooms, join by code,
//                 your name, chat, and start a link match through the room
//                 server (online_match.h). Netplay builds only.
//
// Both are drawn through the runtime's host overlay, so the game's picture,
// memory and timing are untouched; while one is open the game gets no input.
#pragma once

#include <cstddef>
#include <string>

namespace gbarecomp { struct RunOptions; }

// Call after duke_scene_install() and duke_pc_controls_install().
void duke_menu_install(gbarecomp::RunOptions* opts, int argc, char** argv, const std::string& build_tag);

// Debug server: {"cmd":"duke_menu", ...} (headless tests). Non-zero if handled.
int duke_menu_tcp_command(const char* request, void (*write)(void*, const char*, std::size_t), void* ctx);

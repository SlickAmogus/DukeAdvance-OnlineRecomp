// Duke Nukem Advance (AD9P, Europe) - static recompilation host.
//
// The generated cartridge code lives in ../generated; everything else
// (window, audio, input, saves, PPU) comes from the shared gbarecomp runtime.
#include "runtime.h"
#if defined(DUKE_NETPLAY)
#include "gba_netplay_build_identity.h"
#include "multiplayer_launch.h"
#include "online/online_match.h"
#endif
#include "debug_watch.h"
#include "pc_controls.h"
#include "pc_menu.h"
#include "scene_host.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

static int tcp_command(const char* request, void (*write)(void*, const char*, std::size_t), void* ctx) {
    return duke_menu_tcp_command(request, write, ctx) || duke_debug_tcp_command(request, write, ctx);
}

int main(int argc, char** argv) {
    // Launcher-seam flags are sent by framework tooling (replay_diagnostic.py)
    // to every title; this host has no launcher, so drop them.
    std::vector<char*> args;
    bool scripted = false;      // headless / tooling runs keep explicit flags only
    bool display_flag = false;  // the player chose a mode on the command line
    for (int i = 0; i < argc; ++i) {
        if (i > 0 && std::strcmp(argv[i], "--no-launcher") == 0) continue;
        if (!std::strcmp(argv[i], "--no-window") || !std::strcmp(argv[i], "--frames") ||
            !std::strcmp(argv[i], "--tcp") || !std::strcmp(argv[i], "--replay-diagnostic"))
            scripted = true;
        if (!std::strncmp(argv[i], "--fullscreen", 12) ||
            !std::strcmp(argv[i], "--linear-filter") || !std::strcmp(argv[i], "--sharp-filter"))
            display_flag = true;
        args.push_back(argv[i]);
    }

    gbarecomp::RunOptions opts;
    std::string build_tag = "dukeadv";
    bool netplay = false;
#if defined(DUKE_NETPLAY)
    // The cartridge's own link-cable multiplayer, over the network: every
    // peer runs both GBAs and only controller input travels (framework
    // MULTIPLAYER.md). Content hash = SHA-256 of the AD9P cartridge.
    opts.netplay = gbarecomp::make_gba_netplay_launch(
        "dukeadvance-eur", GBARECOMP_NETPLAY_BUILD_ID,
        "a4216a05cd8526d0c4fe7989e0d08d3e22202353c62780b22753ec916082af71");
    opts.netplay->supported_media = 1u << static_cast<unsigned>(gbarecomp::GbaLinkMedium::Cable);
    build_tag = std::string("dukeadv-") + std::string(GBARECOMP_NETPLAY_BUILD_ID).substr(0, 16);
    {
        // An online room (--online-host / --online-join / --online-resume)
        // becomes netplay options once both players are in it; the netplay
        // options (--netplay-*) are then taken out of the argument list.
        static std::vector<std::string> arg_strings;
        arg_strings.assign(args.begin(), args.end());
        std::string online_error;
        if (!dukeonline::prepare_online_match(arg_strings, build_tag, &online_error)) {
            std::fprintf(stderr, "online: %s\n", online_error.c_str());
            return 1;
        }
        try {
            gbarecomp::parse_gba_netplay_arguments(arg_strings, *opts.netplay);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "netplay: %s\n", e.what());
            return 1;
        }
        args.clear();
        for (std::string& a : arg_strings) args.push_back(a.data());
    }
    netplay = opts.netplay->enabled;
#endif
    opts.builtin_game_name = "Duke Nukem Advance";
    opts.builtin_rom_sha1  = "f01522ee46f5aa821adbba0a97b1a745ff9059f5";
    opts.freely_resizable_window = true;
    opts.show_fps_by_default = true;
    opts.tcp_command = tcp_command;
    // A link match runs both consoles on every PC and must stay identical on
    // each: nothing that changes guest execution (native renderer hooks, mouse
    // aim) is installed, only the pad reaches the game.
    if (!netplay) {
        duke_scene_install(&opts);
        duke_pc_controls_install(&opts, argv[0]);
        duke_menu_install(&opts, argc, argv, build_tag);
    }

    std::vector<std::string> remembered;
    if (!scripted && !display_flag) duke_pc_controls_startup_args(&remembered);
    for (std::string& s : remembered) args.push_back(s.data());

    return gbarecomp::run_game(static_cast<int>(args.size()), args.data(), opts);
}

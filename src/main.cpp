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

#include <cstdint>
#include <cstdio>
#include <cstring>
#if defined(_WIN32)
#include <windows.h>
#endif
#include <exception>
#include <string>
#include <vector>

#if defined(_WIN32)
// A crash leaves crash.log (exception, address inside the exe) and crash.dmp
// beside the game, so a report from another PC can be traced. (The runtime's
// own fatal errors already append to crash.log.) Same as GTA Advance's.
static LONG WINAPI write_crash_report(EXCEPTION_POINTERS* info) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    char exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return EXCEPTION_CONTINUE_SEARCH;
    std::string dir(exe);
    dir.erase(dir.find_last_of("\\/") + 1);
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
    const auto* rec = info ? info->ExceptionRecord : nullptr;
    const auto at = rec ? reinterpret_cast<std::uintptr_t>(rec->ExceptionAddress) : 0;
    if (std::FILE* f = std::fopen((dir + "crash.log").c_str(), "a")) {
        SYSTEMTIME t; GetLocalTime(&t);
        std::fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u crash: exception 0x%08lX at %p (exe+0x%llX)",
                     t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                     rec ? static_cast<unsigned long>(rec->ExceptionCode) : 0ul, reinterpret_cast<void*>(at),
                     static_cast<unsigned long long>(at - base));
        if (rec && rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
            std::fprintf(f, ", %s 0x%llX", rec->ExceptionInformation[0] ? "writing" : "reading",
                         static_cast<unsigned long long>(rec->ExceptionInformation[1]));
        std::fprintf(f, "\n\n");
        std::fclose(f);
    }
    using DumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
    if (HMODULE dbghelp = LoadLibraryA("dbghelp.dll")) {
        if (auto dump = reinterpret_cast<DumpFn>(reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")))) {
            HANDLE file = CreateFileA((dir + "crash.dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                struct { DWORD thread; EXCEPTION_POINTERS* pointers; BOOL client; } exc{GetCurrentThreadId(), info, FALSE};
                dump(GetCurrentProcess(), GetCurrentProcessId(), file, 0 /* MiniDumpNormal */,
                     info ? &exc : nullptr, nullptr, nullptr);
                CloseHandle(file);
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static int tcp_command(const char* request, void (*write)(void*, const char*, std::size_t), void* ctx) {
    return duke_menu_tcp_command(request, write, ctx) || duke_debug_tcp_command(request, write, ctx);
}

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetUnhandledExceptionFilter(write_crash_report);
#endif
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
    // The game links in 115200-baud multiplayer mode, where one transfer takes
    // 5755 cycles: each console may run 2048 cycles ahead of the other instead
    // of 63 (the framework's bound for the fastest normal-mode transfer). The
    // master starts a transfer about 6500 cycles after the slave has written
    // its reply, so the slave's data is always in place. Cuts the scheduler's
    // switches between the two consoles from about 25000 a frame to hundreds.
    opts.netplay->link_lookahead = 2048;
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
    // FRAME RATE: 60 changes how the consoles run, so peers that chose
    // differently must not pair up.
    if (netplay && duke_match_fps60()) opts.netplay->program_id += "-fps60";
#endif
    opts.builtin_game_name = "Duke Nukem Advance";
    opts.builtin_rom_sha1  = "f01522ee46f5aa821adbba0a97b1a745ff9059f5";
    opts.freely_resizable_window = true;
    opts.show_fps_by_default = true;
    opts.tcp_command = tcp_command;
    // A link match runs both consoles on every PC and must stay identical on
    // each: nothing that changes guest execution (the single-player renderer
    // hooks, mouse aim) is installed, only the pad reaches the game. The
    // native renderer still draws the match, from a hook that only reads.
    if (!netplay) {
        duke_scene_install(&opts);
        duke_pc_controls_install(&opts, argv[0]);
        duke_menu_install(&opts, argc, argv, build_tag);
    }
#if defined(DUKE_NETPLAY)
    else {
        duke_pc_controls_load_settings(argv[0]);
        duke_scene_install_match(opts.netplay.get());
    }
#endif

    std::vector<std::string> remembered;
    if (!scripted && !display_flag) duke_pc_controls_startup_args(&remembered);
    for (std::string& s : remembered) args.push_back(s.data());

    return gbarecomp::run_game(static_cast<int>(args.size()), args.data(), opts);
}

// PC controls for Duke Nukem Advance: mouse aim + WASD/mouse-button layout.
//
// Mouse aim adds the mouse movement to the player's facing angle and look
// pitch once per game tick, at the entry of the game's own player update
// (a [[mod_function_hook]] in recomp.toml that only observes). Guest facts
// (AD9P):
//
//   IWRAM 0x03001344   the game's pointer to the player entity (EWRAM)
//   entity + 0x74      u16 facing angle, 0x10000 = 360 degrees, + = turn right
//   IWRAM 0x03002F10   s32 look pitch: the game adds it to the horizon line
//                      (walls, floors, sprites and aiming all follow it).
//                      + looks up. The game's own look keys drive it and its
//                      player update pulls it back to 0 every tick.
//   0x0801BDCC         player update, run once per game tick
//
// The pitch auto-recentre is switched off while mouse aim is enabled, through
// two reviewed immediate overrides declared in recomp.toml
// ([[thumb_alu_immediate_override]]): the two "cmp pitch, #0" tests that guard
// the recentre at 0x0801C3E4 / 0x0801C3FA are given operands that make both
// branches skip it.
//
// Two things that do NOT work, both tried: IWRAM 0x0300002C is merely the
// entity the game is working on (often a bullet), and writing the angle at
// display-frame boundaries lands mid-tick, where the game may overwrite it.
#include "pc_controls.h"
#include "scene_host.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "gba_bus.h"
#include "mod_function_hooks.h"
#include "recomp_runtime_ui.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "touch_input.h"

namespace {

constexpr char kKeyMouseAim[]    = "duke.mouse_aim";
constexpr char kKeySensitivity[] = "duke.mouse_sensitivity";
constexpr char kKeyInvertY[]     = "duke.mouse_invert_y";
constexpr char kKeyWasd[]        = "duke.wasd";
constexpr char kKeyFilter[]      = "duke.filter";
constexpr char kKeyNative[]      = "duke.native_renderer";
constexpr char kKeyResolution[]  = "duke.resolution";
constexpr char kKeyFov[]         = "duke.fov";

struct Settings {
    int mouse_aim = 1;
    int sensitivity = 10;   // 1..40
    int invert_y = 0;
    int wasd = 1;
    int fullscreen = 1;     // 0 windowed, 1 borderless, 2 exclusive (remembered)
    int filter = 0;         // 0 sharp pixels, 1 smooth, 2 sharp-bilinear
} g_settings;

std::string g_settings_path;

// The runtime's own hotkeys (config.ini [KeyMap]) default to Tab = turbo and
// F = performance overlay; a stray Tab would run the game at turbo speed.
// Give them other defaults unless the player has already chosen bindings.
void ensure_hotkeys(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / "config.ini";
    std::string text;
    if (std::FILE* f = std::fopen(path.string().c_str(), "rb")) {
        char buf[4096];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
        std::fclose(f);
    }
    if (text.find("[KeyMap]") != std::string::npos) return;
    if (!text.empty() && text.back() != '\n') text += '\n';
    text += "\n[KeyMap]\n"
            "# Hotkeys of the PC shell. Empty = unbound.\n"
            "Fullscreen=Alt+Return\n"
            "Pause=Shift+P\n"
            "Turbo=\n"
            "DisplayPerf=F3\n";
    if (std::FILE* f = std::fopen(path.string().c_str(), "wb")) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
}

void load_settings(const char* argv0) {
    // Beside the executable. (No SDL here: this runs before SDL_Init, and a
    // headless run never initialises SDL at all.)
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::absolute(
        std::filesystem::path(argv0 ? argv0 : "."), ec).parent_path();
    g_settings_path = (dir / "duke_pc.ini").string();
    ensure_hotkeys(dir);
    std::FILE* f = std::fopen(g_settings_path.c_str(), "r");
    if (!f) return;
    char key[64];
    int value = 0;
    while (std::fscanf(f, " %63[^= \n] = %d", key, &value) == 2) {
        if (!std::strcmp(key, "mouse_aim")) g_settings.mouse_aim = value != 0;
        else if (!std::strcmp(key, "mouse_sensitivity"))
            g_settings.sensitivity = value < 1 ? 1 : value > 40 ? 40 : value;
        else if (!std::strcmp(key, "wasd")) g_settings.wasd = value != 0;
        else if (!std::strcmp(key, "mouse_invert_y")) g_settings.invert_y = value != 0;
        else if (!std::strcmp(key, "fullscreen"))
            g_settings.fullscreen = value < 0 ? 0 : value > 2 ? 2 : value;
        else if (!std::strcmp(key, "filter"))
            g_settings.filter = value < 0 ? 0 : value > 2 ? 2 : value;
        else if (!std::strcmp(key, "native_renderer")) duke_scene_settings()->native = value != 0;
        else if (!std::strcmp(key, "resolution"))
            duke_scene_settings()->resolution = value < 0 ? 0 : value > 4 ? 4 : value;
        else if (!std::strcmp(key, "fov"))
            duke_scene_settings()->fov_percent = value < 70 ? 70 : value > 140 ? 140 : value;
    }
    std::fclose(f);
}

void save_settings() {
    std::FILE* f = std::fopen(g_settings_path.c_str(), "w");
    if (!f) return;
    const DukeSceneSettings& scene = *duke_scene_settings();
    std::fprintf(f, "mouse_aim = %d\nmouse_sensitivity = %d\nwasd = %d\n"
                    "mouse_invert_y = %d\nfullscreen = %d\nfilter = %d\n"
                    "native_renderer = %d\nresolution = %d\nfov = %d\n",
                 g_settings.mouse_aim, g_settings.sensitivity, g_settings.wasd,
                 g_settings.invert_y,
                 g_settings.fullscreen, g_settings.filter,
                 scene.native, scene.resolution, scene.fov_percent);
    std::fclose(f);
}

// ── host side: mouse capture and motion ─────────────────────────────────────
std::atomic<int> g_mouse_dx{0};
std::atomic<int> g_mouse_dy{0};
std::atomic<std::uint32_t> g_last_policy_ms{0};   // input hook heartbeat
std::atomic<bool> g_in_gameplay{false};
std::atomic<bool> g_captured{false};
bool g_test_capture = false;        // debug TCP: pretend the mouse is captured
// SDL's event watch and the game loop both call in, and changing the mouse
// mode makes SDL deliver events from inside the call, so the lock must allow
// re-entry and the work itself must not nest.
std::recursive_mutex g_capture_mutex;
bool g_capture_busy = false;        // guarded by g_capture_mutex
std::uint32_t g_unwanted_since_ms = 0;

// Play is "live" while the per-frame input hook keeps firing; the runtime
// stops calling it whenever its settings menu is open.
std::uint32_t now_ms() {
    using namespace std::chrono;
    return static_cast<std::uint32_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool play_is_live() {
    return now_ms() - g_last_policy_ms.load(std::memory_order_relaxed) < 200u;
}

// Short history of why the mouse was taken or let go, kept beside the exe
// (duke_mouse.log, restarted each launch) so a report of "the cursor came
// back" can be traced to a cause.
void capture_log(const char* what, bool focus, bool live, bool gameplay) {
    static std::FILE* f = nullptr;
    static int lines = 0;
    if (!f && !g_settings_path.empty()) {
        std::string path = g_settings_path;
        const std::size_t slash = path.find_last_of("/\\");
        path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "duke_mouse.log";
        f = std::fopen(path.c_str(), "w");
    }
    if (!f || ++lines > 2000) return;
    std::fprintf(f, "%10u ms  %-22s focus=%d live=%d gameplay=%d aim=%d\n", now_ms(), what,
                 focus ? 1 : 0, live ? 1 : 0, gameplay ? 1 : 0, g_settings.mouse_aim);
    std::fflush(f);
}

// Captures the mouse while it should aim and frees it otherwise. The cursor
// is taken at once but only let go after the reason has held for a moment, so
// a single odd frame cannot drop the mouse in the middle of a fight; and the
// decision is checked against what SDL actually has, not a remembered flag,
// so a capture the system took away is taken back.
void update_capture() {
    if (g_test_capture) { g_captured.store(true, std::memory_order_relaxed); return; }
    std::lock_guard<std::recursive_mutex> lock(g_capture_mutex);
    if (g_capture_busy) return;
    struct Busy { Busy() { g_capture_busy = true; } ~Busy() { g_capture_busy = false; } } busy;
    SDL_Window* focus = SDL_GetKeyboardFocus();
    const bool live = play_is_live();
    const bool gameplay = g_in_gameplay.load(std::memory_order_relaxed);
    const bool want = g_settings.mouse_aim && focus && live && gameplay;
    const bool have = SDL_GetRelativeMouseMode() == SDL_TRUE;
    const std::uint32_t now = now_ms();
    if (want) {
        g_unwanted_since_ms = 0;
        if (!have && SDL_SetRelativeMouseMode(SDL_TRUE) == 0) {
            g_mouse_dx.store(0, std::memory_order_relaxed);
            g_mouse_dy.store(0, std::memory_order_relaxed);
            capture_log("captured", focus != nullptr, live, gameplay);
        }
        g_captured.store(SDL_GetRelativeMouseMode() == SDL_TRUE, std::memory_order_relaxed);
        return;
    }
    if (!have) { g_captured.store(false, std::memory_order_relaxed); return; }
    // Losing focus frees the cursor immediately. Anything else has to last a
    // little: a single slow frame looks exactly like the menu opening (the
    // per-frame input hook goes quiet), and must not drop the mouse.
    if (!g_unwanted_since_ms) g_unwanted_since_ms = now ? now : 1u;
    if (!focus || !g_settings.mouse_aim || now - g_unwanted_since_ms > 200u) {
        SDL_SetRelativeMouseMode(SDL_FALSE);
        g_captured.store(false, std::memory_order_relaxed);
        capture_log("released", focus != nullptr, live, gameplay);
    }
}

extern std::atomic<int> g_left_clicks, g_right_clicks, g_wheel;

int SDLCALL event_watch(void*, SDL_Event* e) {
    if (e->type == SDL_MOUSEMOTION && g_captured) {
        g_mouse_dx.fetch_add(e->motion.xrel, std::memory_order_relaxed);
        g_mouse_dy.fetch_add(e->motion.yrel, std::memory_order_relaxed);
    }
    // Clicks and wheel notches for the game's menus. Only while the game is
    // taking input (not behind the Esc menu) and the mouse is not aiming.
    if (!g_captured && play_is_live()) {
        if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT)
            g_left_clicks.fetch_add(1, std::memory_order_relaxed);
        else if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_RIGHT)
            g_right_clicks.fetch_add(1, std::memory_order_relaxed);
        else if (e->type == SDL_MOUSEWHEEL && e->wheel.y != 0)
            g_wheel.fetch_add(e->wheel.y > 0 ? 1 : -1, std::memory_order_relaxed);
    }
    // Only for events that come from the window's own thread; controller and
    // audio events are delivered from SDL's worker threads.
    const bool window_thread_event =
        e->type == SDL_MOUSEMOTION || e->type == SDL_MOUSEBUTTONDOWN ||
        e->type == SDL_MOUSEBUTTONUP || e->type == SDL_KEYDOWN || e->type == SDL_KEYUP ||
        e->type == SDL_WINDOWEVENT;
    if (!window_thread_event) return 0;
    if (e->type == SDL_WINDOWEVENT && g_captured &&
        (e->window.event == SDL_WINDOWEVENT_LEAVE ||
         e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED)) {
        // The pointer got out, or focus came back: Windows may have dropped
        // the confinement that relative mode relies on. Take it afresh.
        std::lock_guard<std::recursive_mutex> lock(g_capture_mutex);
        if (g_capture_busy) return 0;
        g_capture_busy = true;
        SDL_SetRelativeMouseMode(SDL_FALSE);
        g_capture_busy = false;
        capture_log(e->window.event == SDL_WINDOWEVENT_LEAVE ? "re-arm (pointer left)"
                                                             : "re-arm (focus gained)",
                    true, play_is_live(), g_in_gameplay.load(std::memory_order_relaxed));
    }
    update_capture();   // releases the cursor once the menu opens / focus is lost
    return 0;
}

// The runtime's own Display > Fullscreen row applies the mode but does not
// remember it (its launcher normally would). Watch the window and save the
// mode whenever it changes so the next launch starts the same way.
void remember_display_mode() {
    static unsigned tick = 0;
    if (++tick % 30u) return;
    SDL_Window* w = SDL_GetKeyboardFocus();
    if (!w) return;
    const Uint32 flags = SDL_GetWindowFlags(w);
    const int mode =
        (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP ? 1
        : (flags & SDL_WINDOW_FULLSCREEN) ? 2 : 0;
    if (mode == g_settings.fullscreen) return;
    g_settings.fullscreen = mode;
    save_settings();
}

// True once the runtime has opened its window. Headless runs never do, and no
// SDL call is made in that case.
bool host_ready() {
    static bool watching = false;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return false;
    if (!watching) {
        SDL_AddEventWatch(event_watch, nullptr);
        watching = true;
    }
    return true;
}

// ── guest side ──────────────────────────────────────────────────────────────
constexpr std::uint32_t kPlayerPtr = 0x1344;    // IWRAM: the game's pointer to the player
constexpr std::uint32_t kAngleOffset = 0x74;
constexpr std::uint32_t kPitch = 0x2F10;        // IWRAM: look pitch
constexpr int kPitchLimit = 60;                 // horizon pixels either way
constexpr std::uint32_t kPlayerUpdateFn = 0x0801BDCCu;
constexpr std::uint32_t kRecentreCmpUp = 0x0801C3E4u;    // cmp pitch,#0 ; ble skip
constexpr std::uint32_t kRecentreCmpDown = 0x0801C3FAu;  // cmp pitch,#0 ; bge skip
constexpr char kMouseHookId[] = "duke.mouse_aim";
// Sensitivity 10 = 40 angle units per mouse count (about 1600 counts per turn),
// and the same angular speed up and down: the view is about 4 pixels per degree.
constexpr double kYawPerCount = 4.0, kPitchPerCount = 0.088;

// Keeps the game's pitch auto-recentre from running while mouse aim is on.
int recentre_override(std::uint32_t pc, std::uint32_t, std::uint32_t* out) {
    if (!g_settings.mouse_aim) return 0;
    if (pc == kRecentreCmpUp) { *out = 0x7FFFFFFFu; return 1; }    // pitch <= max: skip
    if (pc == kRecentreCmpDown) { *out = 0x80000000u; return 1; }  // pitch >= min: skip
    return 0;
}

std::uint32_t rd32(const std::uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint32_t g_entity = 0;            // the player's entity (guest address), 0 = unknown
bool g_trusted = false;                // the game is ticking the player right now
std::uint64_t g_frame = 0;             // display frames seen
std::uint64_t g_last_update_frame = 0; // display frame of the latest player update
double g_fraction = 0.0;
double g_pitch_fraction = 0.0;

bool plausible_entity(std::uint32_t e) {
    return e >= 0x02000000u && e <= 0x0203FF00u && !(e & 3u);
}

bool aiming() {
    return g_settings.mouse_aim && g_captured.load(std::memory_order_relaxed);
}

// Runs at the entry of the game's player update, once per game tick. This is
// the one moment at which changing the player's facing is safe: the game is
// about to read it, and has finished with last tick's value. The function
// itself always runs (the hook only observes).
int player_update_hook(std::uint32_t, int, ArmCpuState*) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return 0;
    const std::uint32_t entity = rd32(bus->iwram_ptr() + kPlayerPtr);
    if (!plausible_entity(entity)) return 0;
    g_entity = entity;
    g_last_update_frame = g_frame;
    if (!aiming()) return 0;
    const int dx = g_mouse_dx.exchange(0, std::memory_order_relaxed);
    const int dy = g_mouse_dy.exchange(0, std::memory_order_relaxed);
    if (dy != 0) {
        const double px = (g_settings.invert_y ? dy : -dy) * g_settings.sensitivity * kPitchPerCount +
                          g_pitch_fraction;
        const long whole = std::lround(px);
        g_pitch_fraction = px - whole;
        std::uint8_t* p = bus->iwram_ptr() + kPitch;
        long pitch = static_cast<std::int32_t>(rd32(p)) + whole;
        pitch = pitch < -kPitchLimit ? -kPitchLimit : pitch > kPitchLimit ? kPitchLimit : pitch;
        const std::uint32_t v = static_cast<std::uint32_t>(static_cast<std::int32_t>(pitch));
        p[0] = static_cast<std::uint8_t>(v);
        p[1] = static_cast<std::uint8_t>(v >> 8);
        p[2] = static_cast<std::uint8_t>(v >> 16);
        p[3] = static_cast<std::uint8_t>(v >> 24);
    }
    if (dx != 0) {
        const double units = dx * g_settings.sensitivity * kYawPerCount + g_fraction;
        const long whole = std::lround(units);
        g_fraction = units - whole;
        std::uint8_t* angle_p = bus->ewram_ptr() + (entity - 0x02000000u) + kAngleOffset;
        const std::uint16_t angle = static_cast<std::uint16_t>(
            (angle_p[0] | (angle_p[1] << 8)) + whole);
        angle_p[0] = static_cast<std::uint8_t>(angle);
        angle_p[1] = static_cast<std::uint8_t>(angle >> 8);
    }
    return 0;
}

int menu_update_hook(std::uint32_t, int, ArmCpuState* cpu);
bool menu_is_live();

const int g_mouse_hook_registered =
    gba_mod_register_function_entry_plugin(kMouseHookId, kPlayerUpdateFn, 1, player_update_hook) &&
    gba_mod_register_function_entry_plugin(kMouseHookId, 0x08013924u, 1, menu_update_hook) &&
    gba_mod_register_function_entry_plugin(kMouseHookId, 0x08016794u, 1, menu_update_hook);

// Start of every display frame. Decides whether the game is in live play: it
// is while it is in its 3D mode and has run the player update within the last
// third of a second (which stops when the game is paused or in a menu).
void frame_start(const gbarecomp::ExtendedViewFrameInfo* info) {
    ++g_frame;
    const bool mode4 = info->io && info->io_size >= 2 && (info->io[0] & 7u) == 4u;
    g_trusted = mode4 && g_entity != 0 && g_frame - g_last_update_frame <= 20;
    if (!mode4) g_entity = 0;
    g_in_gameplay.store(g_trusted, std::memory_order_relaxed);
    // run_game() clears these hooks on entry, so they are (re)installed from here.
    g_runtime_thumb_alu_imm_override = recentre_override;
    if (g_mouse_hook_registered) gba_mod_set_function_hook_enabled(kMouseHookId, 1);
    duke_scene_frame_start();
    duke_scene_set_menu_active(!g_trusted && menu_is_live());
    if (!g_trusted || !aiming()) {
        // Nothing is going to consume mouse movement: do not let it pile up
        // and swing the view when aim resumes.
        g_mouse_dx.store(0, std::memory_order_relaxed);
        g_mouse_dy.store(0, std::memory_order_relaxed);
        g_fraction = 0.0;
        g_pitch_fraction = 0.0;
    }
}

// ── the game's own menus: mouse pointing ────────────────────────────────────
// Every menu in the game (main, difficulty, save slots, pause...) is run by
// one handler, 0x08013924, called once per frame with the menu in r0. The
// options list has its own, 0x08016794, with the same argument and state.
// Guest facts (AD9P):
//
//   menu + 0x18            item count
//   menu + 0x1C + lang*4   item table for that language, 0x18 bytes per item:
//                          +4 action (0 = a value changed with Left/Right),
//                          +0xC flags (bit 0 = can be selected). A field
//                          holding 9999 means "same as the first language".
//   IWRAM 0x030011D0       selected item (-1 = none)
//   IWRAM 0x030011F0       one sprite object pointer per item; object +0xE /
//                          +0x12 = position, +0x44 / +0x46 = the byte range
//                          of its pieces in OAM as drawn last frame,
//                          +0xC bit 4 = hidden (an entry that is switched off)
//   EWRAM 0x02002CA4       the sprite list being built: a write pointer, then
//                          OAM entries (copied to the hardware next VBlank)
//   IWRAM 0x030011A0       1 = Start (not A) activates the item (title screen)
//   IWRAM 0x03000EA6       language index
//
// The mouse never writes any of this. Pointing at an item taps Up/Down until
// the game's own selection gets there, a click then taps A, so sounds,
// animations and disabled items behave exactly as with the pad.
constexpr std::uint32_t kMenuSelected = 0x11D0, kMenuObjects = 0x11F0;
constexpr std::uint32_t kMenuStartConfirms = 0x11A0, kLanguage = 0x0EA6;
constexpr std::uint32_t kSameAsFirstLanguage = 9999;
constexpr std::uint32_t kOamShadow = 0x02002CA4u;   // EWRAM: write pointer, then 128 entries
constexpr int kMaxMenuItems = 16;
enum { kBitA = 0, kBitB = 1, kBitStart = 3, kBitRight = 4, kBitLeft = 5, kBitUp = 6, kBitDown = 7 };

std::uint32_t g_menu = 0;               // menu the handler last ran for
std::uint64_t g_menu_frame = 0;         // display frame it last ran in
std::atomic<int> g_left_clicks{0}, g_right_clicks{0}, g_wheel{0};
std::atomic<int> g_test_pointer_x{-1}, g_test_pointer_y{-1};
std::atomic<bool> g_test_pointer{false};

int menu_update_hook(std::uint32_t, int, ArmCpuState* cpu) {
    g_menu = cpu->R[0];
    g_menu_frame = g_frame;
    return 0;
}

bool menu_is_live() { return g_menu != 0 && g_frame - g_menu_frame <= 3; }

struct MenuItem {
    bool selectable = false, has_action = false, has_rect = false;
    int x = 0, y = 0;                   // sprite position
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // what it covers on the GBA screen
};

std::uint16_t rd16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

// Reads the live menu's items and where they are on screen. Returns the item
// count (0 if the menu cannot be read).
int read_menu(gba::GbaBus* bus, MenuItem* items, int* selected) {
    constexpr std::uint32_t kRomSize = 0x00800000u;
    const std::uint8_t* rom = bus->rom_ptr();
    const std::uint8_t* iwram = bus->iwram_ptr();
    auto in_rom = [&](std::uint32_t a, std::uint32_t len) {
        return a >= 0x08000000u && a - 0x08000000u + len <= kRomSize;
    };
    if (!in_rom(g_menu, 0x34)) return 0;
    const std::uint8_t* menu = rom + (g_menu - 0x08000000u);
    const std::uint32_t count = rd32(menu + 0x18);
    if (count == 0 || count > kMaxMenuItems) return 0;
    const std::uint32_t first = rd32(menu + 0x1C);
    std::uint32_t local = rd32(menu + 0x1C + (iwram[kLanguage] & 3u) * 4u);
    if (!in_rom(first, count * 0x18)) return 0;
    if (!in_rom(local, count * 0x18)) local = first;
    *selected = static_cast<std::int32_t>(rd32(iwram + kMenuSelected));

    // The ranges index the list the game built this frame, which reaches the
    // screen one frame later: read that list, not the hardware OAM, or the
    // boxes belong to last frame's draw order.
    const std::uint8_t* oam = bus->ewram_ptr() + (kOamShadow - 0x02000000u) + 4;
    const std::uint32_t oam_end = rd32(oam - 4);
    if (oam_end < kOamShadow + 4 || oam_end > kOamShadow + 4 + 128 * 8) oam = bus->oam_ptr();
    static const std::uint8_t kSize[3][4][2] = {
        {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
        {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
        {{8, 16}, {8, 32}, {16, 32}, {32, 64}}};
    for (std::uint32_t i = 0; i < count; ++i) {
        MenuItem& it = items[i];
        it = MenuItem{};
        auto field = [&](std::uint32_t off) {
            const std::uint32_t v = rd32(rom + (local - 0x08000000u) + i * 0x18 + off);
            return v == kSameAsFirstLanguage ? rd32(rom + (first - 0x08000000u) + i * 0x18 + off) : v;
        };
        it.selectable = field(0xC) & 1u;
        it.has_action = field(4) != 0;
        const std::uint32_t obj = rd32(iwram + kMenuObjects + i * 4);
        if (obj < 0x02000000u || obj > 0x0203FF00u) continue;
        const std::uint8_t* o = bus->ewram_ptr() + (obj - 0x02000000u);
        it.x = static_cast<std::int16_t>(rd16(o + 0xE));
        it.y = static_cast<std::int16_t>(rd16(o + 0x12));
        // Bit 4 of the object's flags: not drawn. The game uses it for entries
        // it will not let you select (MULTIPLAYER without a link cable); its
        // OAM range is then left over from whatever used the object before.
        if (o[0xC] & 0x10) { it.selectable = false; continue; }
        const unsigned a = rd16(o + 0x44) / 8u, b = rd16(o + 0x46) / 8u;
        if (a >= b || b > 128) continue;
        bool any = false;
        for (unsigned k = a; k < b; ++k) {
            const std::uint16_t a0 = rd16(oam + k * 8), a1 = rd16(oam + k * 8 + 2);
            if ((a0 & 0x300) == 0x200 || (a0 >> 14) == 3) continue;
            int sy = a0 & 0xFF, sx = a1 & 0x1FF;
            if (sy >= 160) sy -= 256;
            if (sx >= 240) sx -= 512;
            const int w = kSize[a0 >> 14][a1 >> 14][0], h = kSize[a0 >> 14][a1 >> 14][1];
            if (!any) { it.x0 = sx; it.y0 = sy; it.x1 = sx + w; it.y1 = sy + h; any = true; }
            else {
                it.x0 = std::min(it.x0, sx); it.y0 = std::min(it.y0, sy);
                it.x1 = std::max(it.x1, sx + w); it.y1 = std::max(it.y1, sy + h);
            }
        }
        // An item that was not drawn this frame keeps a stale range that now
        // belongs to other sprites; those pieces are nowhere near it.
        it.has_rect = any && it.x >= it.x0 - 8 && it.x <= it.x1 + 8 &&
                      it.y >= it.y0 - 8 && it.y <= it.y1 + 8;
        // Text rows are only 8 pixels tall: a little slack around every item.
        it.x0 -= 2; it.y0 -= 2; it.x1 += 2; it.y1 += 2;
    }
    return static_cast<int>(count);
}

// One key tap at a time: held for a few frames, then released for a few, so
// the game sees a clean press each time.
int g_tap_bit = -1, g_tap_timer = 0;
constexpr int kTapHold = 3, kTapGap = 3;
bool tap_idle() { return g_tap_timer == 0; }
void tap(int bit) { g_tap_bit = bit; g_tap_timer = kTapHold + kTapGap; }

struct Pointer { bool present = false; float x = 0, y = 0; };

// Where the mouse is on the GBA screen.
Pointer read_pointer(const gbarecomp::TouchFrameInfo* info, bool native_view) {
    Pointer p;
    if (g_test_pointer.load(std::memory_order_relaxed)) {
        p.present = true;
        p.x = static_cast<float>(g_test_pointer_x.load(std::memory_order_relaxed));
        p.y = static_cast<float>(g_test_pointer_y.load(std::memory_order_relaxed));
        return p;
    }
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return p;
    SDL_Window* w = SDL_GetMouseFocus();
    if (!w || w != SDL_GetKeyboardFocus()) return p;
    int mx = 0, my = 0, ww = 0, wh = 0;
    SDL_GetMouseState(&mx, &my);
    SDL_GetWindowSize(w, &ww, &wh);
    if (ww <= 0 || wh <= 0 || info->drawable_width <= 0 || info->drawable_height <= 0) return p;
    const float dx = (mx + 0.5f) * info->drawable_width / ww;
    const float dy = (my + 0.5f) * info->drawable_height / wh;
    if (native_view) {
        // The native renderer fills the window: 160 lines tall, centred.
        const float scale = info->drawable_height / 160.0f;
        p.x = (dx - (info->drawable_width - 240.0f * scale) * 0.5f) / scale;
        p.y = dy / scale;
    } else {
        float lx = 0, ly = 0;
        gbarecomp::presentation_point_to_logical(info->game_rect, static_cast<int>(info->view_width),
                                                 static_cast<int>(info->view_height), dx, dy, &lx, &ly);
        p.x = lx - static_cast<float>(info->extra_left);
        p.y = ly - static_cast<float>(info->extra_top);
    }
    p.present = true;
    return p;
}

int item_under(const MenuItem* items, int count, const Pointer& p) {
    int best = -1;
    float best_d = 0;
    for (int i = 0; i < count; ++i) {
        const MenuItem& it = items[i];
        if (!it.selectable || !it.has_rect) continue;
        if (p.x < it.x0 || p.x >= it.x1 || p.y < it.y0 || p.y >= it.y1) continue;
        const float cx = (it.x0 + it.x1) * 0.5f - p.x, cy = (it.y0 + it.y1) * 0.5f - p.y;
        const float d = cx * cx + cy * cy;
        if (best < 0 || d < best_d) { best = i; best_d = d; }
    }
    return best;
}

// Mouse handling whenever the game is not in live play: menus, briefings,
// title and score screens. Adds its key taps to `keys`.
void menu_mouse(const gbarecomp::TouchFrameInfo* info, std::uint16_t* keys, bool resumed) {
    static unsigned last_presents = 0;
    static int native_frames_left = 0;
    static std::uint32_t last_menu = 0;
    static float last_x = -1000, last_y = -1000;
    static int want = -1;          // item the pointer asked for
    static int click = -1;         // item waiting to be activated
    static int budget = 0;         // taps left to get there
    static int moving_from = -2;   // selection when the last Up/Down was sent
    static int moving_wait = 0;    // frames left for the game to act on it
    const unsigned presents = duke_scene_native_presents();
    if (presents != last_presents) { last_presents = presents; native_frames_left = 4; }
    else if (native_frames_left > 0) --native_frames_left;

    int left = g_left_clicks.exchange(0, std::memory_order_relaxed);
    int right = g_right_clicks.exchange(0, std::memory_order_relaxed);
    int wheel = g_wheel.exchange(0, std::memory_order_relaxed);
    const bool playing = g_in_gameplay.load(std::memory_order_relaxed) ||
                         g_captured.load(std::memory_order_relaxed);
    if (resumed || playing) { left = right = wheel = 0; want = click = -1; }

    if (g_tap_timer > 0) {
        if (g_tap_timer > kTapGap) *keys &= static_cast<std::uint16_t>(~(1u << g_tap_bit));
        --g_tap_timer;
    }
    if (playing) return;

    gba::GbaBus* bus = gbarecomp::active_bus();
    MenuItem items[kMaxMenuItems];
    int selected = -1;
    const int count = bus && menu_is_live() ? read_menu(bus, items, &selected) : 0;
    if (g_menu != last_menu || !count) {
        want = click = -1;
        moving_wait = 0;
        last_menu = count ? g_menu : 0;
    }

    if (!count) {
        // No menu: any click continues (briefings, title, tallies), right
        // click backs out, the wheel steps sideways (language flags).
        if (!tap_idle()) return;
        if (left > 0) tap(kBitA);
        else if (right > 0) tap(kBitB);
        else if (wheel != 0) tap(wheel > 0 ? kBitLeft : kBitRight);
        return;
    }

    const Pointer p = read_pointer(info, native_frames_left > 0);
    bool any_rect = false;
    for (int i = 0; i < count; ++i) any_rect = any_rect || (items[i].selectable && items[i].has_rect);
    const int confirm = bus->iwram_ptr()[kMenuStartConfirms] == 1 ? kBitStart : kBitA;
    const int under = p.present ? item_under(items, count, p) : -1;
    // A resting pointer leaves the selection to the keyboard and pad.
    if (p.present && (std::fabs(p.x - last_x) >= 0.5f || std::fabs(p.y - last_y) >= 0.5f)) {
        last_x = p.x; last_y = p.y;
        if (under >= 0 && under != want) { want = under; budget = kMaxMenuItems * 2; }
    }
    if (left > 0) {
        if (!any_rect) { if (tap_idle()) tap(confirm); }
        else if (under >= 0) { want = click = under; budget = kMaxMenuItems * 2; }
    }
    if (!tap_idle()) return;
    // The game can take several frames to act on a press (longer while a menu
    // is sliding in): send the next one only once this one has landed.
    if (moving_wait > 0) {
        if (selected != moving_from) moving_wait = 0;
        else { --moving_wait; return; }
    }
    if (right > 0) { tap(kBitB); want = click = -1; return; }
    if (wheel != 0) {
        // On a value (sound volume, gamma...) the wheel changes it; anywhere
        // else it moves the selection.
        const bool value = selected >= 0 && selected < count && items[selected].selectable &&
                           !items[selected].has_action;
        tap(value ? (wheel > 0 ? kBitRight : kBitLeft) : (wheel > 0 ? kBitUp : kBitDown));
        want = click = -1;
        return;
    }
    if (want >= 0 && want != selected) {
        if (budget-- <= 0) { want = click = -1; return; }
        tap(want > selected ? kBitDown : kBitUp);
        moving_from = selected;
        moving_wait = 30;
        return;
    }
    if (click >= 0 && click == selected) {
        tap(items[click].has_action ? confirm : kBitRight);
        click = -1;
    }
    want = -1;
}

// GBA KEYINPUT bits (active low): 0=A 1=B 2=Select 3=Start 4=Right 5=Left
// 6=Up 7=Down 8=R 9=L. In-game: B fires, A jumps, L/R strafe, Select
// switches weapon (checked against the running game: holding B spends ammo,
// tapping A lifts the view).
std::uint16_t input_frame(const gbarecomp::TouchFrameInfo* info) {
    const std::uint32_t now = now_ms();
    // Coming back from the Esc menu: clicks made in it are not for the game.
    // (Scripted runs pause between commands; their clicks always count.)
    const bool resumed = now - g_last_policy_ms.load(std::memory_order_relaxed) > 200u &&
                         !g_test_pointer.load(std::memory_order_relaxed);
    g_last_policy_ms.store(now, std::memory_order_relaxed);
    std::uint16_t keys = 0x03FF;
    menu_mouse(info, &keys, resumed);
    if (!host_ready()) return keys;
    update_capture();
    remember_display_mode();
    auto press = [&](int bit) { keys &= static_cast<std::uint16_t>(~(1u << bit)); };
    if (g_settings.wasd && SDL_GetKeyboardFocus()) {
        const Uint8* k = SDL_GetKeyboardState(nullptr);
        if (k[SDL_SCANCODE_W]) press(6);
        if (k[SDL_SCANCODE_S]) press(7);
        if (k[SDL_SCANCODE_A]) press(9);
        if (k[SDL_SCANCODE_D]) press(8);
        if (k[SDL_SCANCODE_SPACE]) press(0);    // jump
        if (k[SDL_SCANCODE_LCTRL]) press(1);    // fire
        if (k[SDL_SCANCODE_Q] || k[SDL_SCANCODE_E]) press(2);   // weapon select
    }
    if (g_captured) {
        const Uint32 buttons = SDL_GetMouseState(nullptr, nullptr);
        if (buttons & SDL_BUTTON_LMASK) press(1);   // fire
        if (buttons & SDL_BUTTON_RMASK) press(0);   // jump
        if (buttons & SDL_BUTTON_MMASK) press(2);
    }
    return keys;
}

// ── settings-menu rows ──────────────────────────────────────────────────────
const char* const kFilterChoices[] = {"Sharp pixels", "Smooth", "Sharp-bilinear"};

const char* const kResolutionChoices[] = {"Window (sharpest)", "75%", "50%", "33%", "25% (chunky)"};

const RecompRuntimeUiItem kItems[] = {
    {kKeyNative, "Graphics", "Native renderer",
     "Draw the 3D view on the PC at the window's own resolution and shape "
     "(16:9, 21:9, anything) instead of stretching the 240x160 original. "
     "Turn off for the untouched GBA picture.",
     RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr},
    {kKeyResolution, "Graphics", "Render resolution",
     "Resolution of the native 3D view relative to the window. Lower values "
     "are faster and give a chunkier, more retro look.",
     RECOMP_RUNTIME_UI_CHOICE, 0, 4, 1, kResolutionChoices, 5, nullptr},
    {kKeyFov, "Graphics", "Field of view (%)",
     "100 keeps the game's vertical field of view; wider windows already see "
     "more to the sides. Higher values zoom out.",
     RECOMP_RUNTIME_UI_INT, 70, 140, 5, nullptr, 0, nullptr},
    {kKeyMouseAim, "Controls", "Mouse aim",
     "Turn and look up/down with the mouse during gameplay. Left click fires, "
     "right click jumps. Open this menu (Esc) to free the cursor. In the "
     "game's own menus the mouse always works: point and click, right click "
     "goes back, the wheel changes values.",
     RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr},
    {kKeySensitivity, "Controls", "Mouse sensitivity", "Higher turns faster.",
     RECOMP_RUNTIME_UI_INT, 1, 40, 1, nullptr, 0, nullptr},
    {kKeyInvertY, "Controls", "Invert mouse Y", "Push the mouse forward to look down.",
     RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr},
    {kKeyWasd, "Controls", "WASD layout",
     "W/S move, A/D strafe, Space jumps, Left Ctrl fires, Q/E switch weapon. "
     "The original bindings (arrows, Z fires, X jumps, C, V) keep working.",
     RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr},
    {kKeyFilter, "Graphics", "Scaling filter (next launch)",
     "How the 240x160 image is scaled up. Sharp pixels is the raw look, Smooth "
     "blurs, Sharp-bilinear keeps pixels crisp without shimmer at odd sizes. "
     "Takes effect the next time the game starts.",
     RECOMP_RUNTIME_UI_CHOICE, 0, 2, 1, kFilterChoices, 3, nullptr},
};

int ui_get(const char* key, int* out) {
    if (!std::strcmp(key, kKeyMouseAim)) *out = g_settings.mouse_aim;
    else if (!std::strcmp(key, kKeySensitivity)) *out = g_settings.sensitivity;
    else if (!std::strcmp(key, kKeyWasd)) *out = g_settings.wasd;
    else if (!std::strcmp(key, kKeyInvertY)) *out = g_settings.invert_y;
    else if (!std::strcmp(key, kKeyFilter)) *out = g_settings.filter;
    else if (!std::strcmp(key, kKeyNative)) *out = duke_scene_settings()->native;
    else if (!std::strcmp(key, kKeyResolution)) *out = duke_scene_settings()->resolution;
    else if (!std::strcmp(key, kKeyFov)) *out = duke_scene_settings()->fov_percent;
    else return 0;
    return 1;
}

int ui_set(const char* key, int value) {
    if (!std::strcmp(key, kKeyMouseAim)) g_settings.mouse_aim = value != 0;
    else if (!std::strcmp(key, kKeySensitivity))
        g_settings.sensitivity = value < 1 ? 1 : value > 40 ? 40 : value;
    else if (!std::strcmp(key, kKeyWasd)) g_settings.wasd = value != 0;
    else if (!std::strcmp(key, kKeyInvertY)) g_settings.invert_y = value != 0;
    else if (!std::strcmp(key, kKeyFilter))
        g_settings.filter = value < 0 ? 0 : value > 2 ? 2 : value;
    else if (!std::strcmp(key, kKeyNative)) duke_scene_settings()->native = value != 0;
    else if (!std::strcmp(key, kKeyResolution))
        duke_scene_settings()->resolution = value < 0 ? 0 : value > 4 ? 4 : value;
    else if (!std::strcmp(key, kKeyFov))
        duke_scene_settings()->fov_percent = value < 70 ? 70 : value > 140 ? 140 : value;
    else return 0;
    save_settings();
    return 1;
}

}  // namespace

std::uint32_t duke_pc_controls_player_entity() { return g_trusted ? g_entity : 0u; }

void duke_pc_controls_pending_aim(double* yaw_units, double* pitch_pixels) {
    *yaw_units = 0.0;
    *pitch_pixels = 0.0;
    if (!g_trusted || !aiming()) return;
    const int dx = g_mouse_dx.load(std::memory_order_relaxed);
    const int dy = g_mouse_dy.load(std::memory_order_relaxed);
    *yaw_units = dx * g_settings.sensitivity * kYawPerCount + g_fraction;
    *pitch_pixels = (g_settings.invert_y ? dy : -dy) * g_settings.sensitivity * kPitchPerCount +
                    g_pitch_fraction;
}

void duke_pc_controls_debug_mouse(int dx, int dy) {
    g_test_capture = true;
    g_captured.store(true, std::memory_order_relaxed);
    g_mouse_dx.fetch_add(dx, std::memory_order_relaxed);
    g_mouse_dy.fetch_add(dy, std::memory_order_relaxed);
}

void duke_pc_controls_debug_pointer(int x, int y, int left_clicks, int right_clicks, int wheel) {
    g_test_pointer_x.store(x, std::memory_order_relaxed);
    g_test_pointer_y.store(y, std::memory_order_relaxed);
    g_test_pointer.store(true, std::memory_order_relaxed);
    g_left_clicks.fetch_add(left_clicks, std::memory_order_relaxed);
    g_right_clicks.fetch_add(right_clicks, std::memory_order_relaxed);
    g_wheel.fetch_add(wheel, std::memory_order_relaxed);
}

void duke_pc_controls_startup_args(std::vector<std::string>* extra) {
    if (g_settings.fullscreen)
        extra->push_back("--fullscreen=" + std::to_string(g_settings.fullscreen));
    if (g_settings.filter == 1) { extra->push_back("--linear-filter"); extra->push_back("1"); }
    if (g_settings.filter == 2) { extra->push_back("--sharp-filter"); extra->push_back("1"); }
}

void duke_pc_controls_install(gbarecomp::RunOptions* opts, const char* argv0) {
    load_settings(argv0);
    opts->extended_view_frame = frame_start;
    opts->input_frame = input_frame;
    opts->touch_pad_default = 0;          // no on-screen pad on desktop
    opts->ui_extra_items = kItems;
    opts->ui_extra_item_count = sizeof(kItems) / sizeof(kItems[0]);
    opts->ui_get = ui_get;
    opts->ui_set = ui_set;
}

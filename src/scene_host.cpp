// Native renderer host glue: decides when the native scene replaces the
// game's own 3D view, and composites the game's weapon/overlay bitmap and HUD
// sprites on top of it at the window's resolution.
//
// How the layers are separated
// ----------------------------
// The game draws its 3D view into a mode-4 bitmap page, then draws the weapon
// and any text over it, and shows the HUD as OBJ sprites. A function-entry
// hook ([[mod_function_hook]] in recomp.toml) sits on the last step of the
// game's scene pass (its sprite drawer, 0x08022EF8). While the native
// renderer is on, the hook replaces that step by clearing the page to palette
// index 0. Everything the game draws afterwards (weapon, messages) therefore
// lands on a clean page, and at present time any non-zero pixel is overlay.
// Index 0 is the transparent index for the game's sprite drawers, so it never
// appears inside an overlay.
//
// The game still runs its own BSP walk and wall pass, because gameplay hangs
// off it (enemies wake when they are drawn, the automap records seen walls).
#include "scene_host.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "gba_bus.h"
#include "gba_io.h"
#include "mod_function_hooks.h"
#include "pc_controls.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "scene/scene.h"
#include "scene_hle.h"
#if defined(DUKE_NETPLAY)
#include "multiplayer_launch.h"
#endif

namespace {

constexpr char kHookId[] = "duke.scene_end";
constexpr std::uint32_t kSceneEndFn = 0x08022EF8u;   // sprite drawer, end of scene pass
constexpr std::uint32_t kPlaneFn = 0x0801F960u;      // draws one floor/ceiling plane
constexpr std::uint32_t kSegmentFn = 0x0801FABCu;    // called per wall segment drawn
constexpr std::uint32_t kWallColumnFn = 0x03003624u;      // draws one wall column (ARM)
constexpr std::uint32_t kWallColumnAgainFn = 0x0300342Cu; // the same, same column again
constexpr std::uint32_t kScenePassFn = 0x080201CCu;   // scene pass entry
constexpr std::uint32_t kWallFn = 0x080210A0u;        // per-wall routine (native: scene_hle.cpp)
constexpr std::uint32_t kBoxTestFn = 0x03004640u;     // BSP child box test (IWRAM ARM)
constexpr std::uint32_t kPlanesFn = 0x0801F92Cu;      // plane stage, after the BSP walk
constexpr std::uint32_t kFlipWaitFn = 0x08021146u;    // loop polling the page-flip flag
constexpr std::uint32_t kFlipPendingFlag = 0x03001194u; // non-zero until the VBlank flip
constexpr std::uint32_t kDrawPage = 0x0F04;          // IWRAM: pointer to the page being drawn
constexpr std::uint32_t kMirrorPass = 0x28DC;        // IWRAM: non-zero during a mirror pass
constexpr std::uint32_t kViewX = 0x2E64, kViewY = 0x2870, kViewZ = 0x2828, kViewAngle = 0x2820;
constexpr std::uint32_t kEntityAngle = 0x74;
constexpr std::size_t kPageBytes = 240 * 160;

DukeSceneSettings g_settings;

std::uint32_t rd32(const std::uint8_t* p) {
    std::uint32_t v; std::memcpy(&v, p, 4); return v;
}
std::uint16_t rd16(const std::uint8_t* p) {
    std::uint16_t v; std::memcpy(&v, p, 2); return v;
}

// Which of the two bitmap pages hold "overlay only" content, and the camera
// the game used for its most recent scene pass.
struct State {
    std::uint32_t cleared[2] = {0, 0};     // bumped when the hook clears a page
    std::uint32_t presented[2] = {0, 0};   // value of `cleared` when last flipped to
    bool overlay_valid[2] = {false, false};
    int front = -1;
    bool have_view = false;
    float x = 0, y = 0, z = 0;
    std::uint16_t angle = 0, entity_angle = 0;
    bool have_entity_angle = false;
} g_state;

int page_index(std::uint32_t addr) {
    return addr == 0x06000000u ? 0 : addr == 0x0600A000u ? 1 : -1;
}

bool in_bitmap_mode(gba::GbaBus* bus) {
    return (bus->io().read16(0x000) & 7u) == 4u;
}

// The player's facing angle, or false while the player entity is not known
// (IWRAM 0x2C is only "the entity being worked on"; see pc_controls.cpp).
bool entity_angle(gba::GbaBus* bus, std::uint16_t* out) {
    const std::uint32_t e = duke_pc_controls_player_entity();
    if (e < 0x02000000u || e > 0x0203FF00u) return false;
    *out = rd16(bus->ewram_ptr() + (e - 0x02000000u) + kEntityAngle);
    return true;
}

// Makes a hooked function return to its caller without running its body.
// A direct generated call has pushed its return address on the runtime's
// call-return stack; it is popped here exactly as the function's own return
// would have done (otherwise every skipped call leaks an entry).
int skip_function(ArmCpuState* cpu) {
    const std::uint32_t ret = cpu->R[14] & ~1u;
    runtime_call_should_return(ret);
    cpu->R[15] = ret;
    return 1;
}

// The scene pass still runs for its side effects, but nothing it draws is
// shown, so it is charged an eighth of its emulated CPU time. On the console
// that pass is what holds the game to about 30 frames a second in open rooms;
// discounted, the game completes a frame every display refresh.
constexpr unsigned kScenePassDiscountShift = 3;

// Link matches: every console on every PC runs these hooks the same way (they
// decide from guest memory only), so the consoles stay identical. Only what is
// host-side (the camera, which page holds overlay) follows this PC's console.
bool g_match = false;
gba::GbaBus* g_local_bus = nullptr;

int segment_hook(std::uint32_t, int, ArmCpuState*) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    // Link matches too: every console of every PC does the same (decided from
    // guest memory), and the framework keeps the discount per console and in
    // the saved state. With the native wall routine a match tick of both
    // consoles costs about 12 ms on the test PC at 60 game frames a second.
    if (!g_settings.native || !bus || !in_bitmap_mode(bus)) return 0;
    g_runtime_cycle_discount_shift = kScenePassDiscountShift;
    return 0;   // observed only: the game's function runs as usual
}

// ---- diagnostic write probe (DUKE_SCENE_PROBE=<file>) --------------------------
// Records every guest store between the scene pass's entry and its sprite
// stage: which instruction wrote where, in which CPU mode. Used to find the
// pass's effects that outlive it (what a native replacement must reproduce).
struct ProbeStat { std::uint32_t lo = 0xFFFFFFFFu, hi = 0; std::uint64_t n = 0, changed = 0; std::uint32_t modes = 0; };
struct SceneProbe : gba::BusWriteObserver {
    std::map<std::uint64_t, ProbeStat> by_pc_page;   // (pc << 32) | (addr >> 8)
    bool on_bus_write(gba::BusWriteRegion, std::uint32_t, std::uint32_t addr, std::uint8_t width,
                      std::uint32_t old_value, std::uint32_t new_value) override {
        ProbeStat& st = by_pc_page[(std::uint64_t(g_cpu.R[15]) << 32) | (addr >> 8)];
        st.lo = std::min(st.lo, addr); st.hi = std::max(st.hi, addr + width - 1);
        ++st.n; if (old_value != new_value) ++st.changed;
        st.modes |= 1u << (g_cpu.cpsr & 15u);
        return false;
    }
};
SceneProbe* g_probe = nullptr;
const char* g_probe_path = std::getenv("DUKE_SCENE_PROBE");
unsigned g_probe_passes = 0;
gba::GbaBus* g_probe_bus = nullptr;

void probe_dump() {
    std::FILE* f = std::fopen(g_probe_path, "w");
    if (!f) return;
    std::fprintf(f, "passes %u\n", g_probe_passes);
    for (const auto& [key, st] : g_probe->by_pc_page)
        std::fprintf(f, "pc %08X lo %08X hi %08X n %llu changed %llu modes %X\n", unsigned(key >> 32), st.lo, st.hi,
                     static_cast<unsigned long long>(st.n), static_cast<unsigned long long>(st.changed), st.modes);
    std::fclose(f);
}

int scene_start_hook(std::uint32_t, int, ArmCpuState*) {
    if (!g_probe_path) return 0;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || !in_bitmap_mode(bus)) return 0;
    if (!g_probe) g_probe = new SceneProbe;
    bus->set_write_observer(g_probe);
    g_probe_bus = bus;
    return 0;
}

void probe_end() {
    if (!g_probe_bus) return;
    g_probe_bus->set_write_observer(nullptr);
    g_probe_bus = nullptr;
    if (++g_probe_passes % 100 == 0) probe_dump();
}

int scene_end_hook(std::uint32_t, int, ArmCpuState* cpu) {
    probe_end();
    g_runtime_cycle_discount_shift = 0;
    if (!g_settings.native) return 0;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || !in_bitmap_mode(bus)) return 0;
    const std::uint8_t* iw = bus->iwram_ptr();
    const int page = page_index(rd32(iw + kDrawPage));
    if (page < 0) return 0;
    std::memset(bus->vram_ptr() + (page ? 0xA000 : 0), 0, kPageBytes);
    if (g_match && bus != g_local_bus) return skip_function(cpu);
    ++g_state.cleared[page];
    if (rd32(iw + kMirrorPass) == 0) {
        g_state.x = static_cast<std::int32_t>(rd32(iw + kViewX)) / 256.0f;
        g_state.y = static_cast<std::int32_t>(rd32(iw + kViewY)) / 256.0f;
        g_state.z = static_cast<std::int32_t>(rd32(iw + kViewZ)) / 256.0f;
        g_state.angle = rd16(iw + kViewAngle);
        g_state.have_entity_angle = entity_angle(bus, &g_state.entity_angle);
        g_state.have_view = true;
    }
    return skip_function(cpu);
}

// The game's floor/ceiling drawer only produces pixels, which the hook above
// throws away anyway.
int plane_hook(std::uint32_t, int, ArmCpuState* cpu) {
    if (!g_settings.native) return 0;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || !in_bitmap_mode(bus)) return 0;
    return skip_function(cpu);
}

// The game's wall column drawers (IWRAM ARM) likewise only write pixels; the
// column loops that call them keep every clip-array, plane and sprite-clip
// update. Reached with `mov lr, pc; bx rN` from the loops.
int wall_column_hook(std::uint32_t, int, ArmCpuState* cpu) {
    if (!g_settings.native) return 0;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || !in_bitmap_mode(bus)) return 0;
    return skip_function(cpu);
}

// The game waits for its previous page flip (done by its VBlank handler, which
// clears the byte r1 points at) by polling that byte in a three-instruction
// loop. Emulated, the spin costs as much host time as real work, and with the
// scene pass discounted or skipped most of a frame is spent there. Entering
// HALT instead does what the loop does: nothing until an interrupt, whose
// handler clears the byte. Decided from guest state only, so every console of
// a link match does the same. Not entered with an interrupt already pending
// (HALT would end at once) or with interrupts off (only the loop itself is
// faithful then).
int flip_wait_hook(std::uint32_t, int, ArmCpuState* cpu) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || cpu->R[1] != kFlipPendingFlag) return 0;
    if (bus->iwram_ptr()[kFlipPendingFlag & 0x7FFFu] == 0) return 0;
    const gba::GbaIo& io = bus->io();
    if (!io.ime() || !(io.ie() & 1u) || (io.ie() & io.if_reg()) || (cpu->cpsr & 0x80u)) return 0;
    bus->write8(0x04000301u, 0);   // HALTCNT: halt until IE & IF
    return 0;                      // the loop runs on once the CPU wakes
}

// ---- native wall routine (scene_hle.cpp) ------------------------------------------
// DUKE_HLE=0 leaves every wall to the game's code; DUKE_HLE=shadow runs the
// native routine on a copy of RAM beside the game's own and compares what
// both left behind (at the next wall, box test or the plane stage), printing
// each difference; anything else uses the native routine for every wall it
// supports.
enum class HleMode { Off, On, Shadow };
HleMode hle_mode() {
    static const HleMode mode = [] {
        const char* v = std::getenv("DUKE_HLE");
        if (v && std::strcmp(v, "0") == 0) return HleMode::Off;
        if (v && std::strcmp(v, "shadow") == 0) return HleMode::Shadow;
        return HleMode::On;
    }();
    return mode;
}

unsigned long long g_check_box = 0, g_check_planes = 0;
// One per console: a link match runs two, interleaved.
struct ShadowCheck {
    bool pending = false;
    std::uint32_t seg = 0, sp = 0, lr = 0;
    int result = 0;
    std::vector<std::uint8_t> iw, ew;
    std::uint64_t walls = 0, compared = 0, mismatched = 0;
};
std::map<const gba::GbaBus*, ShadowCheck> g_shadows;

void shadow_region(gba::GbaBus* bus, ShadowCheck& g_shadow, const char* name, std::uint32_t addr, std::uint32_t size) {
    const bool ewram = (addr >> 24) == 2;
    const std::uint32_t off = addr & (ewram ? 0x3FFFFu : 0x7FFFu);
    const std::uint8_t* game = (ewram ? bus->ewram_ptr() : bus->iwram_ptr()) + off;
    const std::uint8_t* mine = (ewram ? g_shadow.ew.data() : g_shadow.iw.data()) + off;
    if (std::memcmp(game, mine, size) == 0) return;
    ++g_shadow.mismatched;
    if (g_shadow.mismatched > 200) return;
    for (std::uint32_t i = 0; i < size; ++i)
        if (game[i] != mine[i]) {
            const std::uint8_t* v = g_shadow.iw.data() + 0x2490;
            std::fprintf(stderr, "[hle] seg %08X ret %d: %s differs at %08X: game %02X native %02X"
                         " | segflags %04X tex %02X/%02X sky %d part %08X up %d low %d 24 %d mid %d\n",
                         g_shadow.seg, g_shadow.result, name, addr + i, game[i], mine[i],
                         rd16(g_shadow.ew.data() + (g_shadow.seg & 0x3FFFFu) + 0xE), v[4], v[5], int(rd32(v + 0x118)),
                         rd32(v + 0xDC), int(rd32(v + 0x1C)), int(rd32(v + 0x20)), int(rd32(v + 0x24)),
                         int(rd32(v + 0x74)));
            break;
        }
}

void shadow_compare(gba::GbaBus* bus) {
    ShadowCheck& g_shadow = g_shadows[bus];
    if (!g_shadow.pending) return;
    g_shadow.pending = false;
    ++g_shadow.compared;
    const std::uint8_t* iw = bus->iwram_ptr();
    shadow_region(bus, g_shadow, "clip", 0x03002730u, 0xF0);
    shadow_region(bus, g_shadow, "closed", 0x03002690u, 0x78);
    shadow_region(bus, g_shadow, "closed count", 0x0300266Cu, 4);
    shadow_region(bus, g_shadow, "list count", 0x03002F00u, 4);
    shadow_region(bus, g_shadow, "divide cache", 0x03003338u, 16);
    shadow_region(bus, g_shadow, "plane rows", 0x03002D60u, 0xF0);
    shadow_region(bus, g_shadow, "flag 2E80", 0x03002E80u, 4);
    shadow_region(bus, g_shadow, "flag 28F0", 0x030028F0u, 4);
    const std::uint32_t verts = rd32(iw + 0x2E7C), sides = rd32(iw + 0x2728), lines = rd32(iw + 0x2884);
    const std::uint8_t* seg = bus->ewram_ptr() + (g_shadow.seg & 0x3FFFFu);
    if ((g_shadow.seg >> 24) == 2) {
        shadow_region(bus, g_shadow, "vertex 1", verts + 0x14u * rd16(seg + 0xA), 0x14);
        shadow_region(bus, g_shadow, "vertex 2", verts + 0x14u * rd16(seg + 0xC), 0x14);
        const std::uint32_t side = sides + 0x14u * rd16(seg + 6);
        if ((side >> 24) == 2) {
            const std::int16_t line = static_cast<std::int16_t>(rd16(bus->ewram_ptr() + (side & 0x3FFFFu) + 0xC));
            shadow_region(bus, g_shadow, "line", lines + 0x14u * std::uint32_t(line), 0x14);
        }
    }
    if (g_shadow.compared % 2000 == 1)
        std::fprintf(stderr, "[hle] shadow: %llu walls compared, %llu differing regions (box checks %llu, plane checks %llu)\n",
                     static_cast<unsigned long long>(g_shadow.compared),
                     static_cast<unsigned long long>(g_shadow.mismatched), g_check_box, g_check_planes);
}

int hle_wall_hook(std::uint32_t, int, ArmCpuState* cpu) {
    const HleMode mode = hle_mode();
    if (mode == HleMode::Off || !g_settings.native) return 0;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || !in_bitmap_mode(bus)) return 0;
    if (mode == HleMode::Shadow) {
        // A console resumed between slices at this entry, before its first
        // instruction, reaches the hook again for the same call.
        const ShadowCheck& pending = g_shadows[bus];
        if (pending.pending && pending.seg == cpu->R[0] && pending.sp == cpu->R[13] && pending.lr == cpu->R[14])
            return 0;
        shadow_compare(bus);
    }
    std::uint8_t* iw = bus->iwram_ptr();
    std::uint8_t* ew = bus->ewram_ptr();
    const std::uint32_t seg = cpu->R[0];
    if (!dukehle::supported(iw, ew, seg)) {
        static unsigned long long declined = 0;
        if (mode == HleMode::Shadow && (++declined % 5000) == 1)
            std::fprintf(stderr, "[hle] %llu walls left to the game\n", declined);
        return 0;
    }
    if (mode == HleMode::On) {
        cpu->R[0] = static_cast<std::uint32_t>(
            dukehle::run(iw, ew, bus->rom_ptr(), bus->rom_size(), seg,
                         static_cast<std::int32_t>(cpu->R[1]), static_cast<std::int32_t>(cpu->R[2])));
        return skip_function(cpu);
    }
    ShadowCheck& g_shadow = g_shadows[bus];
    g_shadow.iw.assign(iw, iw + 0x8000);
    g_shadow.ew.assign(ew, ew + 0x40000);
    g_shadow.result = dukehle::run(g_shadow.iw.data(), g_shadow.ew.data(), bus->rom_ptr(), bus->rom_size(), seg,
                                   static_cast<std::int32_t>(cpu->R[1]), static_cast<std::int32_t>(cpu->R[2]));
    g_shadow.seg = seg;
    g_shadow.sp = cpu->R[13];
    g_shadow.lr = cpu->R[14];
    g_shadow.pending = true;
    ++g_shadow.walls;
    return 0;   // the game's own routine runs; compared at the next check point
}

int hle_check_hook(std::uint32_t addr, int, ArmCpuState*) {
    if (hle_mode() != HleMode::Shadow) return 0;
    ++(addr == kBoxTestFn ? g_check_box : g_check_planes);
    if (gba::GbaBus* bus = gbarecomp::active_bus()) shadow_compare(bus);
    return 0;
}

const int g_registered =
    gba_mod_register_function_entry_plugin(kHookId, kSceneEndFn, 1, scene_end_hook) &&
    gba_mod_register_function_entry_plugin(kHookId, kPlaneFn, 1, plane_hook) &&
    gba_mod_register_function_entry_plugin(kHookId, kWallColumnFn, 0, wall_column_hook) &&
    gba_mod_register_function_entry_plugin(kHookId, kWallColumnAgainFn, 0, wall_column_hook) &&
    gba_mod_register_function_entry_plugin(kHookId, kSegmentFn, 1, segment_hook) &&
    gba_mod_register_function_entry_plugin(kHookId, kFlipWaitFn, 1, flip_wait_hook) &&
    gba_mod_register_function_entry_plugin(kHookId, kScenePassFn, 1, scene_start_hook) &&
    gba_mod_register_function_entry_plugin(kHookId, kWallFn, 1, hle_wall_hook) &&
    gba_mod_register_function_entry_plugin(kHookId, kBoxTestFn, 0, hle_check_hook) &&
    gba_mod_register_function_entry_plugin(kHookId, kPlanesFn, 1, hle_check_hook);

// ---- compositing ---------------------------------------------------------------
std::vector<std::uint32_t> g_frame;
DukeSceneView g_last_view;   // camera of the most recent composite (debugging)

struct Layout {
    int w = 0, h = 0;
    float scale = 1.0f;     // output pixels per GBA pixel
    int box_x = 0;          // left edge of the centred 240-wide area
};

std::uint32_t colour(std::uint16_t c) {
    const std::uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
}

void fill(const Layout& l, float x0, float y0, float x1, float y1, std::uint32_t c) {
    const int ax = std::max(0, static_cast<int>(x0 + 0.5f)), bx = std::min(l.w, static_cast<int>(x1 + 0.5f));
    const int ay = std::max(0, static_cast<int>(y0 + 0.5f)), by = std::min(l.h, static_cast<int>(y1 + 0.5f));
    if (bx <= ax) return;   // off-screen (a window narrower than 3:2 crops the sides)
    for (int y = ay; y < by; ++y) {
        std::uint32_t* row = g_frame.data() + static_cast<std::size_t>(y) * l.w;
        std::fill(row + ax, row + bx, c);
    }
}

// Weapon, messages and anything else the game drew into the bitmap after the
// scene pass. Kept centred: it was authored for the middle 240 columns.
void draw_bitmap_overlay(const Layout& l, const std::uint8_t* page, const std::uint8_t* pal) {
    std::uint32_t lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = colour(rd16(pal + i * 2));
    for (int y = 0; y < 160; ++y) {
        const std::uint8_t* row = page + y * 240;
        for (int x = 0; x < 240;) {
            if (!row[x]) { ++x; continue; }
            int e = x + 1;
            while (e < 240 && row[e] == row[x]) ++e;
            fill(l, l.box_x + x * l.scale, y * l.scale, l.box_x + e * l.scale, (y + 1) * l.scale, lut[row[x]]);
            x = e;
        }
    }
}

// Where a HUD pixel column lands on a window wider than 3:2.
//
// The status boxes live in a band along the top of the screen: the left box
// ends at column 68 and the right one starts at column 172. Within that band
// the left part keeps to the left edge of the window, the right part to the
// right edge, and whatever lies between (the weapon-select strip that joins
// the two boxes) is stretched to span the gap, so the band stays in one piece.
// Everything below the band - the gun, the crosshair - belongs to the 3D view
// and stays centred with it.
constexpr int kHudBandBottom = 46, kHudLeftEnd = 68, kHudRightStart = 172;
// While one of the game's menus is up (pause, map...) nothing on screen is a
// status box: its title sits in the same band and must stay centred.
std::atomic<bool> g_menu_active{false};
std::atomic<unsigned> g_native_presents{0};

float hud_x(const Layout& l, int gx, bool top_band) {
    if (!top_band) return l.box_x + gx * l.scale;
    if (gx <= kHudLeftEnd) return gx * l.scale;
    if (gx >= kHudRightStart) return l.w - (240 - gx) * l.scale;
    const float a = kHudLeftEnd * l.scale, b = l.w - (240 - kHudRightStart) * l.scale;
    return a + (gx - kHudLeftEnd) * (b - a) / (kHudRightStart - kHudLeftEnd);
}

void draw_objects(const Layout& l, gba::GbaBus* bus, std::uint16_t dispcnt) {
    if (!(dispcnt & 0x1000)) return;
    const std::uint8_t* oam = bus->oam_ptr();
    const std::uint8_t* vram = bus->vram_ptr() + 0x10000;
    const std::uint8_t* pal = bus->pal_ptr() + 0x200;
    const bool one_d = dispcnt & 0x40;
    static const std::uint8_t kSize[3][4][2] = {
        {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
        {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
        {{8, 16}, {8, 32}, {16, 32}, {32, 64}}};
    for (int pass = 3; pass >= 0; --pass) {
        for (int i = 127; i >= 0; --i) {
            const std::uint16_t a0 = rd16(oam + i * 8), a1 = rd16(oam + i * 8 + 2), a2 = rd16(oam + i * 8 + 4);
            if (((a2 >> 10) & 3) != pass) continue;
            const bool affine = a0 & 0x100;
            if (!affine && (a0 & 0x200)) continue;          // hidden
            if (((a0 >> 10) & 3) == 2) continue;            // window sprite
            const int shape = a0 >> 14;
            if (shape == 3) continue;
            const int w = kSize[shape][a1 >> 14][0], h = kSize[shape][a1 >> 14][1];
            const bool dbl = affine && (a0 & 0x200);
            const int bw = dbl ? w * 2 : w, bh = dbl ? h * 2 : h;
            int sy = a0 & 0xFF, sx = a1 & 0x1FF;
            if (sy >= 160) sy -= 256;
            if (sx >= 240) sx -= 512;
            const bool bpp8 = a0 & 0x2000;
            const int tile = a2 & 0x3FF;
            const int palbank = bpp8 ? 0 : (a2 >> 12) * 16;
            int pa = 0x100, pb = 0, pc = 0, pd = 0x100;
            if (affine) {
                const int grp = ((a1 >> 9) & 31) * 32;
                pa = static_cast<std::int16_t>(rd16(oam + grp + 6));
                pb = static_cast<std::int16_t>(rd16(oam + grp + 14));
                pc = static_cast<std::int16_t>(rd16(oam + grp + 22));
                pd = static_cast<std::int16_t>(rd16(oam + grp + 30));
            }
            const bool top_band = sy + bh <= kHudBandBottom && !g_menu_active.load(std::memory_order_relaxed);
            for (int y = 0; y < bh; ++y) {
                const int gy = sy + y;
                if (gy < 0 || gy >= 160) continue;
                for (int x = 0; x < bw; ++x) {
                    const int gx = sx + x;
                    if (gx < 0 || gx >= 240) continue;
                    int tx, ty;
                    if (affine) {
                        const int dx = x - bw / 2, dy = y - bh / 2;
                        tx = ((pa * dx + pb * dy) >> 8) + w / 2;
                        ty = ((pc * dx + pd * dy) >> 8) + h / 2;
                        if (tx < 0 || tx >= w || ty < 0 || ty >= h) continue;
                    } else {
                        tx = (a1 & 0x1000) ? w - 1 - x : x;
                        ty = (a1 & 0x2000) ? h - 1 - y : y;
                    }
                    const int row_tiles = one_d ? w / 8 : 32;
                    const int step = bpp8 ? 2 : 1;
                    const int t = (tile + (ty / 8) * (one_d ? row_tiles * step : 32) + (tx / 8) * step) & 0x3FF;
                    if (t < 512) continue;                  // bitmap modes: lower tiles unusable
                    const std::uint8_t* td = vram + t * 32;
                    int index;
                    if (bpp8) {
                        if (t * 32 + (ty & 7) * 8 + (tx & 7) >= 0x8000) continue;
                        index = td[(ty & 7) * 8 + (tx & 7)];
                    } else {
                        const std::uint8_t b = td[(ty & 7) * 4 + ((tx & 7) >> 1)];
                        index = (tx & 1) ? b >> 4 : b & 15;
                    }
                    if (!index) continue;
                    fill(l, hud_x(l, gx, top_band), gy * l.scale, hud_x(l, gx + 1, top_band),
                         (gy + 1) * l.scale, colour(rd16(pal + (palbank + index) * 2)));
                }
            }
        }
    }
}

// Brightness fades done with the blend registers (level start/end).
void apply_fade(const Layout& l, std::uint16_t bldcnt, std::uint16_t bldy) {
    const int mode = (bldcnt >> 6) & 3;
    if (mode < 2 || !(bldcnt & 0x04)) return;       // BG2 must be a first target
    const std::uint32_t k = std::min<std::uint32_t>(bldy & 31, 16);
    if (!k) return;
    for (std::uint32_t& px : g_frame) {
        std::uint32_t rb = px & 0xFF00FF, g = px & 0x00FF00;
        if (mode == 2) {
            rb += (((0xFF00FF - rb) * k) >> 4) & 0xFF00FF;
            g += (((0x00FF00 - g) * k) >> 4) & 0x00FF00;
        } else {
            rb -= ((rb * k) >> 4) & 0xFF00FF;
            g -= ((g * k) >> 4) & 0x00FF00;
        }
        px = 0xFF000000u | rb | g;
    }
    (void)l;
}

// Builds the composite into g_frame. False when the game is not showing a
// native-rendered 3D view right now.
bool compose(int out_w, int out_h, gba::GbaBus* bus = gbarecomp::active_bus(), bool match = false) {
    if (!g_settings.native || !bus || out_w < 64 || out_h < 48) return false;
    const std::uint16_t dispcnt = bus->io().read16(0x000);
    if ((dispcnt & 7u) != 4u || !(dispcnt & 0x0400)) { g_state.front = -1; return false; }

    const int front = (dispcnt >> 4) & 1;
    if (front != g_state.front) {
        // A page is overlay-only exactly when the hook cleared it since it
        // was last shown; anything else is a full-screen picture of its own.
        g_state.overlay_valid[front] = g_state.cleared[front] != g_state.presented[front];
        g_state.presented[front] = g_state.cleared[front];
        g_state.front = front;
    }
    if (!g_state.overlay_valid[front] || !g_state.have_view) return false;

    dukescene::Memory mem;
    mem.ewram = bus->ewram_ptr(); mem.iwram = bus->iwram_ptr();
    mem.rom = bus->rom_ptr(); mem.rom_size = 0x00800000;   // AD9P is an 8 MiB cartridge
    mem.palette = bus->pal_ptr();
    mem.oam = bus->oam_ptr();
    if (!dukescene::in_level(mem)) return false;

    dukescene::Options o;
    o.width = out_w; o.height = out_h;
    o.fov_scale = g_settings.fov_percent / 100.0f;
    o.use_view = true;
    o.view_x = g_state.x; o.view_y = g_state.y; o.view_z = g_state.z;
    // Follow the mouse between game ticks: the player's facing angle is
    // updated every display frame, the game's view angle only when it draws.
    o.view_angle = g_state.angle;
    std::uint16_t now = 0;
    if (!match && g_state.have_entity_angle && entity_angle(bus, &now)) {
        // Turning the game has applied since it drew, plus mouse movement it
        // has not been given yet (that happens once per game tick). Only ever
        // a fraction of a turn; anything larger is not mouse movement.
        double pending_yaw = 0.0, pending_pitch = 0.0;
        duke_pc_controls_pending_aim(&pending_yaw, &pending_pitch);
        const long lead = static_cast<std::int16_t>(now - g_state.entity_angle) +
                          std::lround(pending_yaw);
        if (lead > -0x1800 && lead < 0x1800)
            o.view_angle = static_cast<std::uint16_t>(g_state.angle + lead);
        // The game clamps the look pitch to +/-60; keep the preview inside it.
        const double pitch = static_cast<std::int32_t>(rd32(bus->iwram_ptr() + 0x2F10));
        o.pitch_offset = static_cast<float>(std::clamp(pitch + pending_pitch, -60.0, 60.0) - pitch);
    }
    g_last_view = {o.view_x, o.view_y, o.view_z, o.view_angle};
    g_frame.resize(static_cast<std::size_t>(out_w) * out_h);
    if (!dukescene::render(mem, o, g_frame.data())) return false;

    Layout l;
    l.w = out_w; l.h = out_h;
    l.scale = out_h / 160.0f;
    l.box_x = static_cast<int>((out_w - 240.0f * l.scale) * 0.5f);
    const std::uint8_t* page = bus->vram_ptr() + (front ? 0xA000 : 0);
    draw_bitmap_overlay(l, page, bus->pal_ptr());
    apply_fade(l, bus->io().read16(0x050), bus->io().read16(0x054));
    draw_objects(l, bus, dispcnt);
    return true;
}

int present_frame(gbarecomp::RunOptions::PresentFrame* frame) {
    static const int kPercent[] = {100, 75, 50, 33, 25};
    const int pct = kPercent[std::clamp(g_settings.resolution, 0, 4)];
    // Never narrower than 3:2: on a tall window the picture is letterboxed
    // by rendering extra floor and ceiling instead.
    const int w = std::max(240, frame->drawable_width * pct / 100);
    const int h = std::max(160, frame->drawable_height * pct / 100);
    if (!compose(w, h)) return 0;
    frame->argb = g_frame.data();
    frame->width = static_cast<unsigned>(w);
    frame->height = static_cast<unsigned>(h);
    g_native_presents.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

}  // namespace

DukeSceneSettings* duke_scene_settings() { return &g_settings; }

void duke_scene_install(gbarecomp::RunOptions* opts) {
    opts->present_frame = present_frame;
}

#if defined(DUKE_NETPLAY)
void duke_scene_install_match(gbarecomp::GbaNetplayLaunch* launch) {
    // The game's floor and sprite pixel work is skipped on every console of
    // every PC alike, so a match always uses the native renderer, whatever
    // duke_pc.ini says (the native wall routine and the 60 fps discount need it).
    g_match = true;
    g_settings.native = 1;
    if (g_registered) gba_mod_set_function_hook_enabled(kHookId, 1);
    launch->present_frame = [](gbarecomp::GbaNetplayPresentFrame& frame) {
        if (!frame.local_bus) return false;
        g_local_bus = frame.local_bus;
        static const int kPercent[] = {100, 75, 50, 33, 25};
        const int pct = kPercent[std::clamp(g_settings.resolution, 0, 4)];
        // At most 1080 lines (the window scales it up): every PC runs both
        // consoles, and a slow present holds up the match for both players.
        const int h = std::clamp(frame.drawable_height * pct / 100, 160, 1080);
        const int w = std::max(240, static_cast<int>(static_cast<long long>(frame.drawable_width) * h /
                                                     std::max(1, frame.drawable_height)));
        if (!compose(w, h, frame.local_bus, true)) return false;
        frame.argb = g_frame.data();
        frame.width = static_cast<unsigned>(w);
        frame.height = static_cast<unsigned>(h);
        g_native_presents.fetch_add(1, std::memory_order_relaxed);
        return true;
    };
}
#endif

void duke_scene_frame_start() {
    // The runtime disables every entry hook when it activates mod plugins, so
    // the hook is (re)armed from the per-frame callback.
    if (g_registered) gba_mod_set_function_hook_enabled(kHookId, 1);
    if (!g_settings.native) g_runtime_cycle_discount_shift = 0;
}

DukeSceneView duke_scene_debug_view() { return g_last_view; }

void duke_scene_set_menu_active(bool active) { g_menu_active.store(active, std::memory_order_relaxed); }

unsigned duke_scene_native_presents() { return g_native_presents.load(std::memory_order_relaxed); }

bool duke_scene_debug_shot(const char* path, int width, int height) {
    if (!compose(width, height)) return false;
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (std::uint32_t c : g_frame) {
        const unsigned char px[3] = {static_cast<unsigned char>(c >> 16),
                                     static_cast<unsigned char>(c >> 8),
                                     static_cast<unsigned char>(c)};
        std::fwrite(px, 1, 3, f);
    }
    std::fclose(f);
    return true;
}

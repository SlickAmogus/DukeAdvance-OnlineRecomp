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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "gba_bus.h"
#include "gba_io.h"
#include "mod_function_hooks.h"
#include "runtime_bus_bridge.h"
#include "scene/scene.h"

namespace {

constexpr char kHookId[] = "duke.scene_end";
constexpr std::uint32_t kSceneEndFn = 0x08022EF8u;   // sprite drawer, end of scene pass
constexpr std::uint32_t kDrawPage = 0x0F04;          // IWRAM: pointer to the page being drawn
constexpr std::uint32_t kMirrorPass = 0x28DC;        // IWRAM: non-zero during a mirror pass
constexpr std::uint32_t kViewX = 0x2E64, kViewY = 0x2870, kViewZ = 0x2828, kViewAngle = 0x2820;
constexpr std::uint32_t kPlayerPtr = 0x2C, kEntityAngle = 0x74;
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
} g_state;

int page_index(std::uint32_t addr) {
    return addr == 0x06000000u ? 0 : addr == 0x0600A000u ? 1 : -1;
}

bool in_bitmap_mode(gba::GbaBus* bus) {
    return (bus->io().read16(0x000) & 7u) == 4u;
}

std::uint16_t entity_angle(gba::GbaBus* bus) {
    const std::uint32_t e = rd32(bus->iwram_ptr() + kPlayerPtr);
    if (e < 0x02000000u || e > 0x0203FF00u) return 0;
    return rd16(bus->ewram_ptr() + (e - 0x02000000u) + kEntityAngle);
}

int scene_end_hook(std::uint32_t, int, ArmCpuState* cpu) {
    if (!g_settings.native) return 0;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || !in_bitmap_mode(bus)) return 0;
    const std::uint8_t* iw = bus->iwram_ptr();
    const int page = page_index(rd32(iw + kDrawPage));
    if (page < 0) return 0;
    std::memset(bus->vram_ptr() + (page ? 0xA000 : 0), 0, kPageBytes);
    ++g_state.cleared[page];
    if (rd32(iw + kMirrorPass) == 0) {
        g_state.x = static_cast<std::int32_t>(rd32(iw + kViewX)) / 256.0f;
        g_state.y = static_cast<std::int32_t>(rd32(iw + kViewY)) / 256.0f;
        g_state.z = static_cast<std::int32_t>(rd32(iw + kViewZ)) / 256.0f;
        g_state.angle = rd16(iw + kViewAngle);
        g_state.entity_angle = entity_angle(bus);
        g_state.have_view = true;
    }
    cpu->R[15] = cpu->R[14] & ~1u;   // return to the caller; the body is skipped
    return 1;
}

const int g_registered =
    gba_mod_register_function_entry_plugin(kHookId, kSceneEndFn, 1, scene_end_hook);

// ---- compositing ---------------------------------------------------------------
std::vector<std::uint32_t> g_frame;

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

// HUD sprites. Pieces on the left third stay with the left edge of the
// window, pieces on the right third with the right edge, the rest centred.
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
            const float centre = sx + bw * 0.5f;
            const float anchor = centre < 100.0f ? 0.0f
                               : centre > 140.0f ? l.w - 240.0f * l.scale
                               : static_cast<float>(l.box_x);
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
                    fill(l, anchor + gx * l.scale, gy * l.scale, anchor + (gx + 1) * l.scale,
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
bool compose(int out_w, int out_h) {
    gba::GbaBus* bus = gbarecomp::active_bus();
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
    o.view_angle = static_cast<std::uint16_t>(
        g_state.angle + static_cast<std::uint16_t>(entity_angle(bus) - g_state.entity_angle));
    g_frame.resize(static_cast<std::size_t>(out_w) * out_h);
    if (!dukescene::render(mem, o, g_frame.data())) return false;

    Layout l;
    l.w = out_w; l.h = out_h;
    l.scale = out_h / 160.0f;
    l.box_x = static_cast<int>((out_w - 240.0f * l.scale) * 0.5f);
    draw_bitmap_overlay(l, bus->vram_ptr() + (front ? 0xA000 : 0), bus->pal_ptr());
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
    return 1;
}

}  // namespace

DukeSceneSettings* duke_scene_settings() { return &g_settings; }

void duke_scene_install(gbarecomp::RunOptions* opts) {
    opts->present_frame = present_frame;
}

void duke_scene_frame_start() {
    // The runtime disables every entry hook when it activates mod plugins, so
    // the hook is (re)armed from the per-frame callback.
    if (g_registered) gba_mod_set_function_hook_enabled(kHookId, 1);
}

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

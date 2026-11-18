// Native scene renderer. See scene.h and docs/ENGINE_NOTES.md.
//
// The structure follows a Doom-style software renderer (the game's own engine
// is one): walk the BSP front to back, draw each subsector's wall segments
// column by column while narrowing a per-column open window, and fill the
// floor and ceiling above and below every wall column. Sprites and see-through
// middle textures are drawn afterwards against a depth buffer.
//
// Every screen column is independent, so the frame is split into vertical
// bands rendered on separate threads.
#include "scene.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dukescene {
namespace {

// ---- guest addresses (AD9P) -------------------------------------------------
constexpr std::uint32_t kLines = 0x03002884, kVertices = 0x03002E7C, kSides = 0x03002728,
                        kSectors = 0x03002824, kSubsectors = 0x03002F3C, kSegs = 0x0300267C,
                        kNodes = 0x03002840, kRootNode = 0x03002E74;
constexpr std::uint32_t kViewX = 0x03002E64, kViewY = 0x03002870, kViewZ = 0x03002828,
                        kViewAngle = 0x03002820, kPitch = 0x03002F10;
constexpr std::uint32_t kColormaps = 0x03002E6C, kAnimTable = 0x03000D94,
                        kLightTimer = 0x03002F44, kSpriteBase = 0x0300105C,
                        kPlayer = 0x0300002C, kScrollY = 0x03002E50, kScrollX = 0x03002E68;
constexpr std::uint32_t kRemapTables = 0x03002A40;   // three 256-byte sprite remaps
constexpr std::uint32_t kTexDir = 0x08280E64, kSpecialLight = 0x08035F94,
                        kFlickerTable = 0x08035F74;
constexpr int kTexCount = 1252;
constexpr std::uint32_t kTexData = kTexDir + kTexCount * 0x40;
constexpr int kLightLevels = 0x2E;
constexpr float kFocal160 = 128.5f;   // focal length in pixels of the 240x160 view
constexpr float kNear = 2.0f;

struct Guest {
    Memory m;
    const std::uint8_t* at(std::uint32_t addr, std::size_t len) const {
        const std::uint32_t off = addr & 0x00FFFFFF;
        switch (addr >> 24) {
            case 2: return off + len <= 0x40000 ? m.ewram + off : nullptr;
            case 3: return off + len <= 0x8000 ? m.iwram + off : nullptr;
            case 8: case 9: {
                const std::size_t o = addr - 0x08000000u;
                return o + len <= m.rom_size ? m.rom + o : nullptr;
            }
            default: return nullptr;
        }
    }
    std::uint32_t u32(std::uint32_t a) const {
        const auto* p = at(a, 4);
        if (!p) return 0;
        std::uint32_t v; std::memcpy(&v, p, 4); return v;
    }
    std::int32_t s32(std::uint32_t a) const { return static_cast<std::int32_t>(u32(a)); }
    std::uint16_t u16(std::uint32_t a) const {
        const auto* p = at(a, 2);
        if (!p) return 0;
        std::uint16_t v; std::memcpy(&v, p, 2); return v;
    }
    std::int16_t s16(std::uint32_t a) const { return static_cast<std::int16_t>(u16(a)); }
    std::uint8_t u8(std::uint32_t a) const { const auto* p = at(a, 1); return p ? *p : 0; }
};

// ---- textures (ROM, immutable, cached for the process lifetime) ---------------
struct Texture {
    int w = 0, h = 0;
    int origin_x = 0, world_h = 0;
    std::uint32_t flags = 0;
    bool pow2 = false;
    std::vector<std::uint8_t> px;   // column-major, 0 = transparent (masked use)
};

struct Flat {
    bool solid = true;
    std::uint8_t color = 0;
    std::uint8_t px[32 * 32] = {};
};

std::mutex g_cache_mutex;
std::atomic<const Texture*> g_textures[2][kTexCount];
std::atomic<const Flat*> g_flats[kTexCount];

const Texture* texture(const Guest& g, int index, bool masked) {
    if (index < 0 || index >= kTexCount) return nullptr;
    if (const Texture* t = g_textures[masked][index].load(std::memory_order_acquire)) return t;
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    if (const Texture* t = g_textures[masked][index].load(std::memory_order_acquire)) return t;
    auto tex = std::make_unique<Texture>();
    const std::uint32_t e = kTexDir + index * 0x40;
    const int h = static_cast<int>(g.u32(e)), w = static_cast<int>(g.u32(e + 4));
    const std::uint32_t mip0 = g.u32(e + 0x14);
    tex->flags = g.u32(e + 0x3C);
    tex->origin_x = g.s32(e + 0x0C);
    tex->world_h = g.s32(e + 0x10);
    if (w > 0 && h > 0 && w <= 1024 && h <= 1024 && mip0 != 0xFFFFFFFFu &&
        g.at(kTexData + mip0, static_cast<std::size_t>(w) * 4)) {
        tex->w = w; tex->h = h;
        tex->pow2 = (w & (w - 1)) == 0 && (h & (h - 1)) == 0;
        tex->px.assign(static_cast<std::size_t>(w) * h, 0);
        for (int x = 0; x < w; ++x) {
            const std::uint32_t off = g.u32(kTexData + mip0 + x * 4);
            if (!off) continue;
            std::uint32_t p = kTexData + off;
            int y0 = 0, y1 = h;
            if (masked && g.u8(p) == 0x0A) {
                y0 = g.u8(p + 1); y1 = std::min<int>(g.u8(p + 2), h); p += 3;
            }
            if (y1 <= y0) continue;
            std::uint8_t* col = &tex->px[static_cast<std::size_t>(x) * h];
            if (const auto* src = g.at(p, static_cast<std::size_t>(y1 - y0)))
                std::memcpy(col + y0, src, y1 - y0);
            if (!masked) continue;
            // A transparent run is stored as 00 0A <row it ends at>, repeated
            // every 8 bytes through the run. Index 0x0A is never a real pixel.
            for (int y = y0; y < y1;) {
                if (col[y] == 0 && y + 2 < y1 && col[y + 1] == 0x0A) {
                    int end = col[y + 2];
                    if (end <= y + 1 || end > y1) end = std::min(y1, y + 8);
                    std::memset(col + y, 0, static_cast<std::size_t>(end - y));
                    y = end;
                } else {
                    ++y;
                }
            }
        }
    }
    const Texture* out = tex.release();
    g_textures[masked][index].store(out, std::memory_order_release);
    return out;
}

// Floors and ceilings: a 64x64 tile stored from its second mip level down,
// i.e. a raw 32x32 block. Entries with no data are a solid colour.
const Flat* flat(const Guest& g, int index) {
    if (index < 0 || index >= kTexCount) return nullptr;
    if (const Flat* f = g_flats[index].load(std::memory_order_acquire)) return f;
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    if (const Flat* f = g_flats[index].load(std::memory_order_acquire)) return f;
    auto fl = std::make_unique<Flat>();
    const std::uint32_t e = kTexDir + index * 0x40;
    fl->color = static_cast<std::uint8_t>(g.u32(e + 0x3C) >> 8);
    const std::uint32_t mip1 = g.u32(e + 0x18);
    if (g.u32(e) == 64 && g.u32(e + 4) == 64 && mip1) {
        if (const auto* src = g.at(kTexData + mip1, 1024)) {
            std::memcpy(fl->px, src, 1024);
            fl->solid = false;
        }
    }
    const Flat* out = fl.release();
    g_flats[index].store(out, std::memory_order_release);
    return out;
}

// ---- per-frame state ---------------------------------------------------------
struct Sector {
    float floor = 0, ceil = 0;
    int light = 0;
    const Flat* floor_flat = nullptr;
    const Flat* ceil_flat = nullptr;
    bool has_ceiling = true;
};

struct SpriteDraw {
    float tx, tz, z_bottom, scale;
    const Texture* tex;
    int light;
    int remap;      // 1..3: extra palette remap table (tinted sprites)
    bool sparkle;   // teleporter shimmer
};

struct MaskedSeg {
    float sx1, sx2, iz1, iz2, uz1, uz2;   // screen span and perspective terms
    float top, bottom;                    // world z range of the opening
    float v_top;                          // texture v at `top`
    const Texture* tex;
    int light, u_offset;
    float depth;
};

struct Frame {
    Guest g;
    int w = 0, h = 0;
    float vx = 0, vy = 0, vz = 0;
    float fx = 0, fy = 0, rx = 0, ry = 0;      // forward and right unit vectors
    float focal = 0, cx = 0, horizon = 0;
    std::uint32_t lines = 0, vertices = 0, sides = 0, sectors = 0, subsectors = 0, segs = 0,
                  nodes = 0, sprites = 0, anim = 0;
    int root = 0;
    int n_sectors = 0;
    int scroll_x = 0, scroll_y = 0;
    std::vector<Sector> sector;
    std::vector<std::uint32_t> lut;            // [light level][palette index] -> ARGB
    std::uint32_t palette[256] = {};           // palette index -> ARGB
    std::vector<float> row_inv;                // focal / (horizon - row centre)
    std::uint32_t* color = nullptr;            // column-major
    float* depth = nullptr;                    // column-major
    bool draw_sprites = true;

    const std::uint32_t* lut_for(int level) const {
        return &lut[static_cast<std::size_t>(std::clamp(level, 0, kLightLevels - 1)) * 256];
    }
};

int animated(const Frame& f, int index, int* scroll_u, int* scroll_v) {
    if (index < 0 || index >= kTexCount) return index;
    const std::uint32_t flags = f.g.u32(kTexDir + index * 0x40 + 0x3C);
    if (!(flags & 0x10)) return index;
    const int next = index + static_cast<std::int8_t>(f.g.u8(f.anim + index));
    if (next < 0 || next >= kTexCount) return index;
    if (flags & 0x80) {
        if (f.g.u32(kTexDir + next * 0x40 + 0x0C) == 0) *scroll_v += f.scroll_y;
        else *scroll_u += f.scroll_x;
    }
    return next;
}

int sector_light(const Frame& f, int index, std::uint32_t p) {
    const std::uint16_t flags = f.g.u16(p + 0x18);
    const int a = f.g.u8(p), b = f.g.u8(p + 1);
    if (!(flags & 1)) {
        if (a < 0xF && a != 0 && a != 7 && a != 8)
            return static_cast<int>(f.g.u32(kSpecialLight + a * 4));
        return 0x1F - (a >> 3);
    }
    const std::uint32_t timer = f.g.u32(kLightTimer);
    std::uint32_t t = timer;
    if (!(flags & 2))
        t = (t + ((static_cast<std::uint32_t>(index) << 4) ^ (static_cast<std::uint32_t>(index) << 11))) & 0x7FFF;
    int v;
    if (!(flags & 8)) {
        bool first;
        if (flags & 0x10)
            first = f.g.u8(kFlickerTable + (((timer >> 10) + index * 0x10) & 0x1F)) != 0;
        else if (flags & 4)
            first = (t & 0x1FFF) <= 0xEFF;
        else
            first = (t & 0x3FFF) <= 0x1DFF;
        v = first ? a : b;
    } else {
        if (t > 0x4000) t = 0x8000 - t;
        v = (b + ((static_cast<int>(t) * (a - b)) >> 14)) & 0xFF;
    }
    return 0x1F - (v >> 3);
}

// One vertical band of the screen and its column clip state.
struct Band {
    const Frame& f;
    int x0, x1;
    std::vector<int> top, bot;      // open row window per column, [top, bot)
    int open = 0;
    std::vector<SpriteDraw> sprites;
    std::vector<MaskedSeg> masked;
    std::vector<std::uint8_t> sub_seen;

    Band(const Frame& fr, int a, int b)
        : f(fr), x0(a), x1(b), top(b - a, 0), bot(b - a, fr.h), open(b - a) {}

    int row_of(float world_z, float z) const {
        const float y = f.horizon - (world_z - f.vz) * f.focal / z;
        // First row whose centre is at or below y.
        const float r = std::ceil(y - 0.5f);
        if (r < -1.0e6f) return -1000000;
        if (r > 1.0e6f) return 1000000;
        return static_cast<int>(r);
    }

    void plane(int x, int ya, int yb, const Flat* fl, float plane_z, int light) {
        if (ya >= yb || !fl) return;
        std::uint32_t* c = f.color + static_cast<std::size_t>(x) * f.h;
        float* d = f.depth + static_cast<std::size_t>(x) * f.h;
        const std::uint32_t* lut = f.lut_for(light);
        const float dh = plane_z - f.vz;
        if (fl->solid) {
            const std::uint32_t colour = lut[fl->color];
            for (int y = ya; y < yb; ++y) {
                c[y] = colour;
                d[y] = dh * f.row_inv[y];
            }
            return;
        }
        const float k = (static_cast<float>(x) + 0.5f - f.cx) / f.focal;
        const float dx = (f.fx + f.rx * k) * 0.5f, dy = (f.fy + f.ry * k) * 0.5f;
        const float ox = f.vx * 0.5f, oy = f.vy * 0.5f;
        for (int y = ya; y < yb; ++y) {
            const float z = dh * f.row_inv[y];
            const int tx = static_cast<int>(std::floor(ox + dx * z)) & 31;
            const int ty = static_cast<int>(std::floor(oy + dy * z)) & 31;
            c[y] = lut[fl->px[(31 - ty) * 32 + tx]];
            d[y] = z;
        }
    }

    // Draws rows [ya, yb) of a wall column. `v_at_top` is the texture v at
    // world height `top_z`; v grows downwards.
    void wall(int x, int ya, int yb, const Texture* t, int u, float top_z, float v_at_top,
              float z, int light, bool masked) {
        if (ya >= yb || !t || !t->w) return;
        std::uint32_t* c = f.color + static_cast<std::size_t>(x) * f.h;
        float* d = f.depth + static_cast<std::size_t>(x) * f.h;
        const std::uint32_t* lut = f.lut_for(light);
        u %= t->w; if (u < 0) u += t->w;
        const std::uint8_t* col = &t->px[static_cast<std::size_t>(u) * t->h];
        const double step = static_cast<double>(z) / f.focal;
        const double v0 = v_at_top + (top_z - f.vz) + (ya + 0.5 - f.horizon) * step;
        if (!masked) {
            // 16.16 fixed point, wrapped up front so the loop cannot overflow.
            const double wrapped = v0 - std::floor(v0 / t->h) * t->h;
            std::uint32_t vf = static_cast<std::uint32_t>(wrapped * 65536.0);
            const std::uint32_t sf = static_cast<std::uint32_t>(step * 65536.0);
            if (t->pow2) {
                const std::uint32_t mask = static_cast<std::uint32_t>(t->h - 1);
                for (int y = ya; y < yb; ++y, vf += sf) {
                    c[y] = lut[col[(vf >> 16) & mask]];
                    d[y] = z;
                }
            } else {
                const std::uint32_t hf = static_cast<std::uint32_t>(t->h) << 16;
                for (int y = ya; y < yb; ++y) {
                    c[y] = lut[col[vf >> 16]];
                    d[y] = z;
                    vf += sf;
                    while (vf >= hf) vf -= hf;
                }
            }
            return;
        }
        for (int y = ya; y < yb; ++y) {
            const int v = static_cast<int>(std::floor(v0 + (y - ya) * step));
            if (v < 0) continue;
            if (v >= t->h) break;
            const std::uint8_t p = col[v];
            if (p && z < d[y]) c[y] = lut[p];
        }
    }

    void seg(std::uint32_t sp);
    void subsector(int index);
    void node(int index, int depth);
    void draw_masked(const MaskedSeg& m);
    void draw_sprite(const SpriteDraw& s);
    void run();
};

void Band::seg(std::uint32_t sp) {
    const Guest& g = f.g;
    const std::uint16_t flags = g.u16(sp + 0x0E);
    const int side_i = g.s16(sp + 6);
    if (side_i < 0) return;
    const std::uint32_t side = f.sides + side_i * 0x14;
    const std::uint32_t va = f.vertices + g.u16(sp + 0x0A) * 0x14;
    const std::uint32_t vb = f.vertices + g.u16(sp + 0x0C) * 0x14;
    const float ax = g.s16(va) - f.vx, ay = g.s16(va + 2) - f.vy;
    const float bx = g.s16(vb) - f.vx, by = g.s16(vb + 2) - f.vy;
    float tx1 = ax * f.rx + ay * f.ry, tz1 = ax * f.fx + ay * f.fy;
    float tx2 = bx * f.rx + by * f.ry, tz2 = bx * f.fx + by * f.fy;
    if (tz1 < kNear && tz2 < kNear) return;
    // Facing test: the viewer must be on the seg's right.
    if (tx1 * tz2 - tx2 * tz1 >= 0.0f) return;
    const float len = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
    float u1 = 0.0f, u2 = len;
    if (tz1 < kNear) {
        const float t = (kNear - tz1) / (tz2 - tz1);
        tx1 += (tx2 - tx1) * t; tz1 = kNear; u1 = len * t;
    } else if (tz2 < kNear) {
        const float t = (kNear - tz1) / (tz2 - tz1);
        tx2 = tx1 + (tx2 - tx1) * t; tz2 = kNear; u2 = len * t;
    }
    const float sx1 = f.cx + f.focal * tx1 / tz1, sx2 = f.cx + f.focal * tx2 / tz2;
    if (sx2 <= sx1) return;
    const int xa = std::max(x0, static_cast<int>(std::ceil(sx1 - 0.5f)));
    const int xb = std::min(x1, static_cast<int>(std::ceil(sx2 - 0.5f)));
    if (xa >= xb) return;

    const int front_i = g.s16(side + 0x0E);
    if (front_i < 0 || front_i >= f.n_sectors) return;
    const Sector& fs = f.sector[front_i];
    const std::uint32_t line = f.lines + g.s16(side + 0x0C) * 0x14;
    const std::uint16_t line_flags = g.u16(line + 8);
    const Sector* bs = nullptr;
    if (flags & 0x2000) {
        int other = g.s16(line + 4);
        if (other == side_i) other = g.s16(line + 6);
        if (other >= 0) {
            const int back_i = g.s16(f.sides + other * 0x14 + 0x0E);
            if (back_i >= 0 && back_i < f.n_sectors) bs = &f.sector[back_i];
        }
    }

    const int x_off = g.s16(side + 0x10) + (g.u16(sp) >> 4);
    const int y_off = g.s16(side + 0x12);
    const float iz1 = 1.0f / tz1, iz2 = 1.0f / tz2;
    const float uz1 = u1 * iz1, uz2 = u2 * iz2;
    const float inv_span = 1.0f / (sx2 - sx1);

    // Texture set-up, mirroring the game's wall preparation.
    const Texture* mid = nullptr; const Texture* upper = nullptr; const Texture* lower = nullptr;
    float mid_top = 0, mid_v = 0, up_v = 0, low_v = 0;
    int mid_u = 0, up_u = 0, low_u = 0;
    const bool textured = flags & 1;
    if (!bs) {
        int su = 0, sv = 0;
        const int index = animated(f, g.s16(side + 2), &su, &sv);
        mid = textured ? texture(g, index, false) : nullptr;
        mid_top = fs.ceil;
        mid_v = static_cast<float>(y_off + sv);
        if (line_flags & 0x10) mid_v += fs.floor - fs.ceil;
        mid_u = x_off + su;
    } else if (textured) {
        if (bs->ceil < fs.ceil) {
            int su = 0, sv = 0;
            const int index = animated(f, g.s16(side), &su, &sv);
            upper = texture(g, index, false);
            up_v = static_cast<float>(y_off + sv);
            if (!(line_flags & 8)) up_v += bs->ceil - fs.ceil;
            up_u = x_off + su;
        }
        if (bs->floor > fs.floor) {
            int su = 0, sv = 0;
            const int index = animated(f, g.s16(side + 4), &su, &sv);
            lower = texture(g, index, false);
            low_v = static_cast<float>(y_off + sv);
            if (flags & 0x1000) low_v += fs.ceil - bs->floor;
            low_u = x_off + su;
        }
        const int mid_index = g.s16(side + 2);
        if (mid_index >= 0) {
            int su = 0, sv = 0;
            const int index = animated(f, mid_index, &su, &sv);
            if (const Texture* t = texture(g, index, true); t && t->w) {
                MaskedSeg m{};
                m.sx1 = sx1; m.sx2 = sx2; m.iz1 = iz1; m.iz2 = iz2; m.uz1 = uz1; m.uz2 = uz2;
                m.top = std::min(fs.ceil, bs->ceil);
                m.bottom = std::max(fs.floor, bs->floor);
                float v = static_cast<float>(y_off + sv);
                if (line_flags & 0x10) v += m.bottom - m.top;
                if (line_flags & 2) v = 0;
                v -= std::floor(v / t->h) * t->h;
                m.v_top = v;
                m.tex = t; m.light = fs.light; m.u_offset = x_off + su;
                m.depth = (tz1 + tz2) * 0.5f;
                masked.push_back(m);
            }
        }
    }

    for (int x = xa; x < xb; ++x) {
        const int i = x - x0;
        int t = top[i], b = bot[i];
        if (t >= b) continue;
        const float s = (static_cast<float>(x) + 0.5f - sx1) * inv_span;
        const float iz = iz1 + (iz2 - iz1) * s;
        const float z = 1.0f / iz;
        const int u = static_cast<int>(std::floor((uz1 + (uz2 - uz1) * s) * z));
        const int yc = row_of(fs.ceil, z), yf = row_of(fs.floor, z);

        if (fs.has_ceiling && fs.ceil > f.vz) plane(x, t, std::min(yc, b), fs.ceil_flat, fs.ceil, fs.light);
        if (fs.floor < f.vz) plane(x, std::max(yf, t), b, fs.floor_flat, fs.floor, fs.light);
        t = std::max(t, yc);
        b = std::min(b, yf);

        if (!bs) {
            wall(x, t, b, mid, mid_u + u, mid_top, mid_v, z, fs.light, false);
            top[i] = bot[i] = 0;
            --open;
            continue;
        }
        if (t < b && bs->ceil < fs.ceil) {
            const int ycb = std::min(row_of(bs->ceil, z), b);
            if (upper && upper->w) {
                wall(x, t, ycb, upper, up_u + u, fs.ceil, up_v, z, fs.light, false);
                t = std::max(t, ycb);
            } else if (fs.has_ceiling) {
                // Untextured gap: the game leaves it to the ceiling behind.
            }
        }
        if (t < b && bs->floor > fs.floor) {
            const int yfb = std::max(row_of(bs->floor, z), t);
            if (lower && lower->w) {
                wall(x, yfb, b, lower, low_u + u, bs->floor, low_v, z, fs.light, false);
                b = std::min(b, yfb);
            }
        }
        if (t >= b || bs->ceil <= bs->floor) {
            top[i] = bot[i] = 0;
            --open;
        } else {
            top[i] = t; bot[i] = b;
        }
    }
}

void Band::subsector(int index) {
    const Guest& g = f.g;
    if (index < 0) return;
    const std::uint32_t ss = f.subsectors + index * 0x0C;
    if (!g.at(ss, 0x0C)) return;
    if (f.draw_sprites) {
        int guard = 0;
        for (int si = g.s16(ss + 8); si != -1 && guard < 64; ++guard) {
            const std::uint32_t p = f.sprites + static_cast<std::uint32_t>(si) * 4;
            if (!g.at(p, 0x24)) break;
            const std::uint8_t f0 = g.u8(p);
            if (f0 & 0x20) {
                const float dx = g.s32(p + 0x14) / 256.0f - f.vx;
                const float dy = g.s32(p + 0x1C) / 256.0f - f.vy;
                const float tz = dx * f.fx + dy * f.fy;
                const int tex_i = g.s16(p + 2);
                if (tz > kNear && tex_i >= 0) {
                    if (const Texture* t = texture(g, tex_i, true); t && t->w) {
                        SpriteDraw s{};
                        s.tx = dx * f.rx + dy * f.ry; s.tz = tz;
                        s.z_bottom = g.s32(p + 0x18) / 256.0f;
                        s.scale = (g.u8(p + 1) + 1) / 256.0f;
                        s.tex = t;
                        const std::uint32_t f10 = g.u32(p + 0x10);
                        const int sec = g.s16(ss);
                        int light = sec >= 0 && sec < f.n_sectors ? f.sector[sec].light : 0;
                        if (f10 & 0x10000) {
                            const int v = (g.u32(kLightTimer) >> 8) & 0x1F;
                            light = v > 0xF ? 0x1F - v : v;
                        }
                        if ((f0 & 0x40) && light < 0x22) light = 0;
                        if (f10 & 4) light = 0;
                        if (f0 & 0x80) light = 0x21;
                        s.light = light;
                        s.remap = static_cast<int>((f10 >> 4) & 3);
                        s.sparkle = (f10 & 2) != 0;
                        sprites.push_back(s);
                    }
                }
            }
            si = g.s16(p + 8);
        }
    }
    int guard = 0;
    for (int si = g.s16(ss + 2); si != -1 && guard < 512 && open > 0; ++guard) {
        const std::uint32_t sp = f.segs + static_cast<std::uint32_t>(si) * 0x10;
        if (!g.at(sp, 0x10)) break;
        seg(sp);
        si = g.s16(sp + 4);
    }
}

void Band::node(int index, int depth) {
    if (open <= 0 || depth > 64 || index < 0) return;
    const Guest& g = f.g;
    const std::uint32_t n = f.nodes + static_cast<std::uint32_t>(index) * 0x24;
    if (!g.at(n, 0x24)) return;
    const float d = g.s16(n + 8) * (f.vx - g.s16(n)) + g.s16(n + 10) * (f.vy - g.s16(n + 2));
    const std::uint32_t first = d > 0.0f ? n + 0x0C : n + 0x18;
    const std::uint32_t second = d > 0.0f ? n + 0x18 : n + 0x0C;
    for (const std::uint32_t child : {first, second}) {
        if (open <= 0) return;
        const int sub = g.s16(child + 8);
        if (sub != -1) node(sub, depth + 1);
        else subsector(g.s16(child + 10));
    }
}

void Band::draw_masked(const MaskedSeg& m) {
    const int xa = std::max(x0, static_cast<int>(std::ceil(m.sx1 - 0.5f)));
    const int xb = std::min(x1, static_cast<int>(std::ceil(m.sx2 - 0.5f)));
    const float inv_span = 1.0f / (m.sx2 - m.sx1);
    for (int x = xa; x < xb; ++x) {
        const float s = (static_cast<float>(x) + 0.5f - m.sx1) * inv_span;
        const float z = 1.0f / (m.iz1 + (m.iz2 - m.iz1) * s);
        const int u = static_cast<int>(std::floor((m.uz1 + (m.uz2 - m.uz1) * s) * z));
        const int ya = std::max(0, row_of(m.top, z)), yb = std::min(f.h, row_of(m.bottom, z));
        wall(x, ya, yb, m.tex, m.u_offset + u, m.top, m.v_top, z, m.light, true);
    }
}

void Band::draw_sprite(const SpriteDraw& s) {
    const Texture* t = s.tex;
    const float k = f.focal / s.tz;
    const float left = f.cx + (s.tx - t->origin_x * s.scale) * k;
    const float width = t->w * s.scale * k;
    const float bottom = f.horizon - (s.z_bottom - f.vz) * k;
    const float height = (t->world_h > 0 ? t->world_h : t->h) * s.scale * k;
    const float topy = bottom - height;
    if (width <= 0.0f || height <= 0.0f) return;
    const int xa = std::max(x0, static_cast<int>(std::ceil(left - 0.5f)));
    const int xb = std::min(x1, static_cast<int>(std::ceil(left + width - 0.5f)));
    const int ya = std::max(0, static_cast<int>(std::ceil(topy - 0.5f)));
    const int yb = std::min(f.h, static_cast<int>(std::ceil(bottom - 0.5f)));
    if (xa >= xb || ya >= yb) return;
    const std::uint32_t* lut = f.lut_for(s.light);
    std::uint32_t special[256];
    if (s.sparkle) {
        // The game indexes its light tables with an out-of-range level here,
        // which lands in sprite attribute memory: the shimmer is that data.
        for (int i = 0; i < 256; ++i)
            special[i] = f.g.m.oam ? f.palette[f.g.m.oam[(0x264 + i) & 0x3FF]] : lut[i];
        lut = special;
    } else if (s.remap) {
        const std::uint8_t* table = f.g.at(kRemapTables + (s.remap - 1) * 0x100, 0x100);
        if (table) {
            for (int i = 0; i < 256; ++i) special[i] = lut[table[i]];
            lut = special;
        }
    }
    const float du = t->w / width, dv = t->h / height;
    for (int x = xa; x < xb; ++x) {
        const int u = std::clamp(static_cast<int>((static_cast<float>(x) + 0.5f - left) * du), 0, t->w - 1);
        const std::uint8_t* col = &t->px[static_cast<std::size_t>(u) * t->h];
        std::uint32_t* c = f.color + static_cast<std::size_t>(x) * f.h;
        const float* d = f.depth + static_cast<std::size_t>(x) * f.h;
        for (int y = ya; y < yb; ++y) {
            const int v = static_cast<int>((static_cast<float>(y) + 0.5f - topy) * dv);
            if (v < 0 || v >= t->h) continue;
            const std::uint8_t p = col[v];
            if (!p || s.tz >= d[y]) continue;
            c[y] = lut[p];
        }
    }
}

void Band::run() {
    for (int x = x0; x < x1; ++x) {
        std::uint32_t* c = f.color + static_cast<std::size_t>(x) * f.h;
        float* d = f.depth + static_cast<std::size_t>(x) * f.h;
        std::fill(c, c + f.h, 0xFF000000u);
        std::fill(d, d + f.h, 1.0e30f);
    }
    node(f.root, 0);
    // Far to near; see-through walls and sprites share one ordering.
    struct Item { float depth; int kind, index; };
    std::vector<Item> items;
    items.reserve(masked.size() + sprites.size());
    for (std::size_t i = 0; i < masked.size(); ++i) items.push_back({masked[i].depth, 0, static_cast<int>(i)});
    for (std::size_t i = 0; i < sprites.size(); ++i) items.push_back({sprites[i].tz, 1, static_cast<int>(i)});
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.depth > b.depth; });
    for (const Item& it : items) {
        if (it.kind == 0) draw_masked(masked[it.index]);
        else draw_sprite(sprites[it.index]);
    }
}

bool setup(Frame& f, const Memory& mem, const Options& o) {
    f.g.m = mem;
    const Guest& g = f.g;
    if (!mem.ewram || !mem.iwram || !mem.rom || !mem.palette) return false;
    f.lines = g.u32(kLines); f.vertices = g.u32(kVertices); f.sides = g.u32(kSides);
    f.sectors = g.u32(kSectors); f.subsectors = g.u32(kSubsectors); f.segs = g.u32(kSegs);
    f.nodes = g.u32(kNodes); f.sprites = g.u32(kSpriteBase); f.anim = g.u32(kAnimTable);
    for (std::uint32_t p : {f.lines, f.vertices, f.sides, f.sectors, f.subsectors, f.segs, f.nodes})
        if ((p >> 24) != 2) return false;
    if ((g.u32(kPlayer) >> 24) != 2) return false;
    f.root = g.s32(kRootNode);
    if (f.root < 0) return false;
    // Sectors are stored directly before the lines.
    f.n_sectors = f.lines > f.sectors ? static_cast<int>((f.lines - f.sectors) / 0x28) : 0;
    if (f.n_sectors <= 0 || f.n_sectors > 4096) return false;

    f.w = o.width; f.h = o.height;
    f.vx = g.s32(kViewX) / 256.0f; f.vy = g.s32(kViewY) / 256.0f; f.vz = g.s32(kViewZ) / 256.0f;
    std::uint16_t angle = g.u16(kViewAngle);
    if (o.use_view) { f.vx = o.view_x; f.vy = o.view_y; f.vz = o.view_z; angle = o.view_angle; }
    const float a = angle * (6.28318530718f / 65536.0f);
    f.fx = std::sin(a); f.fy = std::cos(a);
    f.rx = std::cos(a); f.ry = -std::sin(a);
    const float scale = static_cast<float>(f.h) / 160.0f;
    f.focal = kFocal160 * scale / std::max(0.25f, o.fov_scale);
    f.cx = f.w * 0.5f;
    f.horizon = f.h * 0.5f + (g.s32(kPitch) + o.pitch_offset) * scale;
    f.scroll_x = g.s32(kScrollX); f.scroll_y = g.s32(kScrollY);
    f.draw_sprites = o.sprites;

    f.row_inv.resize(f.h);
    for (int y = 0; y < f.h; ++y) {
        float dy = f.horizon - (static_cast<float>(y) + 0.5f);
        if (std::fabs(dy) < 0.05f) dy = dy < 0 ? -0.05f : 0.05f;
        f.row_inv[y] = f.focal / dy;
    }

    const std::uint32_t maps = g.u32(kColormaps);
    f.lut.assign(static_cast<std::size_t>(kLightLevels) * 256, 0xFF000000u);
    std::uint32_t* pal = f.palette;
    for (int i = 0; i < 256; ++i) {
        std::uint16_t c; std::memcpy(&c, mem.palette + i * 2, 2);
        const std::uint32_t r = c & 31, gr = (c >> 5) & 31, b = (c >> 10) & 31;
        pal[i] = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((gr << 3 | gr >> 2) << 8) | (b << 3 | b >> 2);
    }
    for (int level = 0; level < kLightLevels; ++level) {
        const std::uint8_t* map = g.at(maps + level * 256, 256);
        for (int i = 0; i < 256; ++i) f.lut[level * 256 + i] = pal[map ? map[i] : i];
    }

    f.sector.resize(f.n_sectors);
    for (int i = 0; i < f.n_sectors; ++i) {
        const std::uint32_t p = f.sectors + i * 0x28;
        Sector& s = f.sector[i];
        s.floor = g.s32(p + 0x20) / 256.0f;
        s.ceil = g.s32(p + 0x24) / 256.0f;
        s.light = sector_light(f, i, p);
        const std::uint16_t ft = g.u16(p + 0x0C), ct = g.u16(p + 0x0E);
        s.floor_flat = flat(g, ft & 0x7FFF);
        s.has_ceiling = ct != 0xFFFE;
        s.ceil_flat = s.has_ceiling ? flat(g, ct & 0x7FFF) : nullptr;
    }
    return true;
}

}  // namespace

bool in_level(const Memory& mem) {
    Guest g{mem};
    if (!mem.ewram || !mem.iwram) return false;
    return (g.u32(kPlayer) >> 24) == 2 && (g.u32(kLines) >> 24) == 2 && (g.u32(kNodes) >> 24) == 2;
}

bool render(const Memory& mem, const Options& opts, std::uint32_t* out) {
    if (opts.width <= 0 || opts.height <= 0 || !out) return false;
    Frame f;
    if (!setup(f, mem, opts)) return false;

    static thread_local std::vector<std::uint32_t> color;
    static thread_local std::vector<float> depth;
    const std::size_t n = static_cast<std::size_t>(f.w) * f.h;
    if (color.size() < n) { color.resize(n); depth.resize(n); }
    f.color = color.data(); f.depth = depth.data();

    int threads = opts.threads > 0 ? opts.threads
                                   : static_cast<int>(std::thread::hardware_concurrency());
    threads = std::clamp(threads, 1, std::max(1, f.w / 64));
    threads = std::min(threads, 32);
    auto work = [&f, out, threads](int t) {
        const int a = static_cast<int>(static_cast<std::int64_t>(f.w) * t / threads);
        const int b = static_cast<int>(static_cast<std::int64_t>(f.w) * (t + 1) / threads);
        Band band(f, a, b);
        band.run();
        // Column-major to row-major, in small blocks for the cache.
        for (int bx = a; bx < b; bx += 32) {
            const int ex = std::min(b, bx + 32);
            for (int y = 0; y < f.h; ++y) {
                std::uint32_t* row = out + static_cast<std::size_t>(y) * f.w;
                for (int x = bx; x < ex; ++x) row[x] = f.color[static_cast<std::size_t>(x) * f.h + y];
            }
        }
    };
    if (threads == 1) {
        work(0);
    } else {
        std::vector<std::thread> pool;
        pool.reserve(threads - 1);
        for (int t = 1; t < threads; ++t) pool.emplace_back(work, t);
        work(0);
        for (auto& th : pool) th.join();
    }
    return true;
}

}  // namespace dukescene

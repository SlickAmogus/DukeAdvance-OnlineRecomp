// Native replacement of the game's per-wall routine (ROM 0x080210A0) for the
// occlusion it computes, under the native renderer.
//
// The game's scene pass decides gameplay: the BSP walk visits the subsectors
// whose bounding boxes still cover an open screen column, and every sprite in
// a visited subsector raises the "seen" event (enemies wake, entities count as
// seen). Which columns are open is what the walls drawn so far leave of the
// 120-column clip array. The walk, the box tests and the sprite pass stay the
// game's own code; this file computes, bit for bit, what each wall does to:
//
//   0x03002730  clip array: per column u16, top << 8 | bottom (open rows)
//   0x03002690  per-column "closed" bytes, 0x0300266C their count
//   0x03002D60  per-column plane rows (view+0xC0), view+0x110 plane bits
//   0x03002F00  word count of the drawing list sprites share (its limit
//               stops sprite events): see-through columns add 7, a
//               see-through wall's header 13
//   vertex records (+4 x, +8 depth, +0xC column, +0x10 frame stamp)
//   line flags (+8 |= 0x400, the automap's "seen")
//   0x03003338  the divide routine's cache (its results depend on it)
//   the self-modifying slots and operands of the IWRAM column loops
//
// It does not produce pixels, visplanes, sprite clip records or see-through
// column records: only the game's own drawers read those, and they are
// skipped under the native renderer. The game's first wall of each frame
// still runs as the game's code (it waits for the page flip there).
//
// Every routine below follows the game's instruction order and integer
// arithmetic (32-bit wrapping multiplies, arithmetic shifts, the table-based
// divide and its cache), from the disassembly; names give the ROM or IWRAM
// address each mirrors. DUKE_HLE_SHADOW=1 runs it on a copy of RAM beside the
// game's own code and compares the results after every wall.
#include "scene_hle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dukehle {
namespace {

std::uint32_t lsl(std::uint32_t v, unsigned n) { n &= 0xFFu; return n >= 32 ? 0 : v << n; }
std::uint32_t lsr(std::uint32_t v, unsigned n) { n &= 0xFFu; return n >= 32 ? 0 : v >> n; }
std::int32_t asr(std::int32_t v, unsigned n) { n &= 0xFFu; return n >= 32 ? (v < 0 ? -1 : 0) : v >> n; }
std::int32_t mul(std::int32_t a, std::int32_t b) { return std::int32_t(std::uint32_t(a) * std::uint32_t(b)); }
std::int32_t add(std::int32_t a, std::int32_t b) { return std::int32_t(std::uint32_t(a) + std::uint32_t(b)); }
std::int32_t sub(std::int32_t a, std::int32_t b) { return std::int32_t(std::uint32_t(a) - std::uint32_t(b)); }
std::int32_t smull_hi(std::int32_t a, std::int32_t b) { return std::int32_t((std::int64_t(a) * std::int64_t(b)) >> 32); }

constexpr std::uint32_t kView = 0x03002490u;
constexpr std::uint32_t kClip = 0x03002730u;
constexpr std::uint32_t kSolid = 0x03002690u;
constexpr std::uint32_t kSolidCount = 0x0300266Cu;
constexpr std::uint32_t kListCount = 0x03002F00u;
constexpr std::uint32_t kDivCache = 0x03003338u;

struct Hle {
    std::uint8_t* iw;
    std::uint8_t* ew;
    const std::uint8_t* rom;
    std::size_t rom_size;

    std::uint8_t* ptr(std::uint32_t a) const {
        switch (a >> 24) {
        case 2: return ew + (a & 0x3FFFFu);
        case 3: return iw + (a & 0x7FFFu);
        default: {
            const std::uint32_t off = a & 0x01FFFFFFu;
            if ((a >> 24) >= 8 && (a >> 24) <= 0xD && off + 4 <= rom_size) return const_cast<std::uint8_t*>(rom) + off;
            static std::uint8_t zero[8];
            std::memset(zero, 0, sizeof zero);
            return zero;
        }
        }
    }
    std::int32_t r32(std::uint32_t a) const { std::int32_t v; std::memcpy(&v, ptr(a), 4); return v; }
    std::uint16_t r16(std::uint32_t a) const { std::uint16_t v; std::memcpy(&v, ptr(a), 2); return v; }
    std::int32_t rs16(std::uint32_t a) const { return std::int16_t(r16(a)); }
    std::uint8_t r8(std::uint32_t a) const { return *ptr(a); }
    std::int32_t rs8(std::uint32_t a) const { return std::int8_t(r8(a)); }
    void w32(std::uint32_t a, std::int32_t v) { std::memcpy(ptr(a), &v, 4); }
    void w16(std::uint32_t a, std::uint32_t v) { const std::uint16_t h = std::uint16_t(v); std::memcpy(ptr(a), &h, 2); }
    void w8(std::uint32_t a, std::uint32_t v) { *ptr(a) = std::uint8_t(v); }
    std::int32_t view(unsigned off) const { return r32(kView + off); }
    void set_view(unsigned off, std::int32_t v) { w32(kView + off, v); }

    // ---- 0x030031EC divide / 0x030032B8 reciprocal (shared cache) ----------------
    std::int32_t recip_core(std::int32_t r1, std::int32_t& r4) {
        std::int32_t r3;
        r4 = r32(kDivCache + 4);
        if (r1 == r32(kDivCache)) {
            r3 = r32(kDivCache + 8);
        } else {
            r3 = r32(kDivCache + 0xC);
            for (int guard = 0; guard < 64 && r3 > 1 && r1 < r3; ++guard) { --r4; r3 = std::int32_t(std::uint32_t(r3) >> 1); }
            for (int guard = 0; guard < 64 && r1 > std::int32_t(std::uint32_t(r3) << 1); ++guard) { ++r4; r3 = std::int32_t(std::uint32_t(r3) << 1); }
            w32(kDivCache + 0xC, r3);
            w32(kDivCache + 4, r4);
            w32(kDivCache, r1);
            const std::uint32_t idx = r4 >= 0 ? lsr(std::uint32_t(r1), unsigned(r4)) : lsl(std::uint32_t(r1), unsigned(-r4));
            r3 = r32(0x08000770u + idx * 4u);
            w32(kDivCache + 8, r3);
        }
        return r3;
    }
    std::int32_t div(std::int32_t r0, std::int32_t r1) {
        if (r1 == 0) return 0;
        if (r1 < 0) { r0 = sub(0, r0); r1 = sub(0, r1); }
        if (r1 == 1) return r0;
        if (std::uint32_t(r0) < (std::uint32_t(r1) << 3)) {
            std::uint32_t r2 = std::uint32_t(r0);
            std::int32_t q = 0;
            for (;;) {
                const bool carry = r2 >= std::uint32_t(r1);
                r2 -= std::uint32_t(r1);
                q += carry ? 1 : 0;
                if (!carry) break;
            }
            return q;
        }
        std::int32_t r4;
        const std::int32_t r3 = recip_core(r1, r4);
        const std::int32_t s = r4 + 8;
        if (s < 0) r0 = std::int32_t(lsl(std::uint32_t(r0), unsigned(-s)));
        std::int32_t q = smull_hi(r3, r0);
        if (s > 0) q = asr(q, unsigned(s));
        return q;
    }
    // r0 = reciprocal, r1 = shift + 8
    void recip(std::int32_t r1, std::int32_t& out_r0, std::int32_t& out_r1) {
        std::int32_t r4;
        out_r0 = recip_core(r1, r4);
        out_r1 = r4 + 8;
    }

    // ---- 0x03004018: narrow [L, R] to the open columns of [x1, x2) --------------
    bool open_range(std::int32_t r0, std::int32_t r1, std::int32_t& L, std::int32_t& R) {
        const std::int32_t r4 = L, r5 = R;
        if (r0 < r4) r0 = r4;
        if (r1 > r5) r1 = r5;
        r1 -= 1;
        if (r1 >= 0x78) r1 = 0x77;
        // The game scans from r0 + 1 up to r1 (reading at least one byte).
        std::int32_t first = -1;
        for (std::int32_t c = r0 + 1; ; ++c) {
            if (r8(kSolid + std::uint32_t(c)) == 0) { first = c; break; }
            if (c >= r1) break;
        }
        if (first < 0) { L = r4; R = r5; return false; }
        std::int32_t last = r1;
        while (r8(kSolid + std::uint32_t(last)) != 0) --last;
        std::int32_t nl = first - 2, nr = last + 2;
        if (nl < -1) nl = -1;
        if (nr > 0x78) nr = 0x78;
        L = nl; R = nr;
        return true;
    }

    // ---- 0x030041D4: view space, near clip, columns ------------------------------
    // b: the game's stack buffer (piece 0): x1 x2 d1 d2 u1 u2 col1 col2 ...
    int clip(std::int32_t* b, std::uint32_t seg, std::int32_t& L, std::int32_t& R) {
        const std::uint32_t verts = std::uint32_t(r32(0x03002E7Cu));
        const std::uint32_t v2 = verts + 0x14u * r16(seg + 0xC);
        const std::uint32_t v1 = verts + 0x14u * r16(seg + 0xA);
        const std::int32_t frame = r32(0x0300264Cu);
        const std::uint32_t vt = 0x03002E90u;
        std::int32_t ldx = 0, ldy = 0;
        if (std::int32_t(r16(v1 + 0x10)) != frame) {
            ldx = sub(std::int32_t(std::uint32_t(rs16(v1)) << 8), r32(vt + 0x14));
            ldy = sub(std::int32_t(std::uint32_t(rs16(v1 + 2)) << 8), r32(vt + 0x1C));
            const std::int32_t d = asr(add(mul(r32(vt + 0x3C), ldx), mul(r32(vt + 0x40), ldy)), 8);
            w32(v1 + 8, d);
            b[2] = d;
        } else {
            b[0] = r32(v1 + 4);
            b[2] = r32(v1 + 8);
        }
        if (std::int32_t(r16(v2 + 0x10)) != frame) {
            const std::int32_t dx = sub(std::int32_t(std::uint32_t(rs16(v2)) << 8), r32(vt + 0x14));
            const std::int32_t dy = sub(std::int32_t(std::uint32_t(rs16(v2 + 2)) << 8), r32(vt + 0x1C));
            const std::int32_t d = asr(add(mul(r32(vt + 0x3C), dx), mul(r32(vt + 0x40), dy)), 8);
            w32(v2 + 8, d);
            b[3] = d;
            if (d > 0x200) {
                const std::int32_t ip = add(mul(dx, r32(vt + 0x34)), mul(dy, r32(vt + 0x38)));
                const std::int32_t x = asr(ip, 8);
                w32(v2 + 4, x); b[1] = x;
                w16(v2 + 0x10, std::uint32_t(frame));
                w32(v2 + 0xC, div(std::int32_t(std::uint32_t(ip) << 4), d));
            } else {
                if (b[2] <= 0x200) return 1;
                const std::int32_t ip = asr(add(mul(dx, r32(vt + 0x34)), mul(dy, r32(vt + 0x38))), 8);
                w32(v2 + 4, ip); b[1] = ip;
                w16(v2 + 0x10, std::uint32_t(frame));
            }
        } else {
            b[1] = r32(v2 + 4);
            b[3] = r32(v2 + 8);
        }
        if (std::int32_t(r16(v1 + 0x10)) != frame) {
            const std::int32_t ip = add(mul(ldx, r32(vt + 0x34)), mul(ldy, r32(vt + 0x38)));
            const std::int32_t x = asr(ip, 8);
            b[0] = x; w32(v1 + 4, x);
            if (b[2] > 0x200) w32(v1 + 0xC, div(std::int32_t(std::uint32_t(ip) << 4), r32(v1 + 8)));
        }
        w32(v1 + 0x10, frame);

        // side planes: |x| * 4 against depth
        {
            std::int32_t fp = b[0];
            const std::int32_t sb = std::int32_t(std::uint32_t(fp) << 2);
            const std::int32_t r0 = b[2];
            if (std::int64_t(r0) + sb >= 0) {
                if (r0 < sb) {
                    const std::int32_t r1 = std::int32_t(std::uint32_t(b[1]) << 2);
                    if (b[3] < r1) return 1;
                }
            } else {
                const std::int32_t r1 = std::int32_t(std::uint32_t(b[1]) << 2);
                const std::int32_t r2 = b[3];
                if (std::int64_t(r2) + r1 < 0) return 1;
                if (r0 < sb && r2 < r1) return 1;
            }
        }
        b[4] = rs16(seg);
        b[5] = rs16(seg + 2);
        std::int32_t c1, c2;
        std::int32_t fp = b[0];
        if (b[2] <= 0x200) {
            const std::int32_t r0 = b[2];
            const std::int32_t sb = sub(b[3], r0), sl = sub(b[1], fp);
            const std::int32_t t = div(std::int32_t(std::uint32_t(0x400 - r0) << 12), sb);
            const std::int32_t d = add(r0, asr(mul(sb, t), 12));
            if (d <= 0x200) return 1;
            fp = add(fp, asr(mul(sl, t), 12));
            b[0] = fp; b[2] = d;
            if (b[1] == fp && b[3] == d) return 1;
            b[4] = add(b[4], asr(mul(sub(b[5], b[4]), t), 12));
            c1 = div(std::int32_t(std::uint32_t(fp) << 12), d);
            b[6] = c1;
            c2 = r32(v2 + 0xC);
            b[7] = c2;
        } else if (b[3] > 0x200) {
            c1 = r32(v1 + 0xC); b[6] = c1;
            c2 = r32(v2 + 0xC); b[7] = c2;
        } else {
            const std::int32_t r1 = b[3], r2 = b[1];
            const std::int32_t sb = sub(b[2], r1), sl = sub(fp, r2);
            const std::int32_t t = div(std::int32_t(std::uint32_t(0x400 - r1) << 12), sb);
            const std::int32_t d = add(r1, asr(mul(sb, t), 12));
            if (d <= 0x200) return 1;
            const std::int32_t x = add(r2, asr(mul(sl, t), 12));
            b[1] = x; b[3] = d;
            if (b[0] == x && b[2] == d) return 1;
            b[5] = sub(b[5], asr(mul(sub(b[5], b[4]), t), 12));
            c2 = div(std::int32_t(std::uint32_t(x) << 12), d);
            b[7] = c2;
            c1 = r32(v1 + 0xC);
            b[6] = c1;
        }
        const std::int32_t r5 = asr(c1, 4), sb = asr(c2, 4);
        const int hidden = open_range(r5 + 0x3B, sb + 0x3D, L, R) ? 0 : 1;
        if (sb < r5 || sb < -0x3C || r5 >= 0x3C) return 1;
        if (sb < L - 0x3C) return 1;
        if (r5 > R - 0x3C) return 1;
        return hidden;
    }

    // ---- 0x08020BAC: clip against the view's side planes -------------------------
    int cull(std::int32_t* b) {
        const std::int32_t r7 = asr(mul(b[0], 60), 4), r6 = asr(mul(b[1], 60), 4);
        if (std::int64_t(b[2]) + r7 < 0) {
            const std::int32_t r2 = add(r7, b[2]), r1 = add(r6, b[3]);
            if (r2 != r1) {
                const std::int32_t t = div(std::int32_t(std::uint32_t(r2) << 12), sub(r2, r1));
                const std::int32_t r5 = add(b[2], asr(mul(sub(b[3], b[2]), t), 12));
                b[2] = r5;
                if (r5 <= 0) return 1;
                const std::int32_t r0 = add(b[0], asr(mul(sub(b[1], b[0]), t), 12));
                b[0] = r0;
                b[4] = add(b[4], asr(mul(sub(b[5], b[4]), t), 12));
                b[6] = div(std::int32_t(std::uint32_t(r0) << 12), r5);
            }
        }
        if (b[3] < r6) {
            const std::int32_t r2 = sub(r7, b[2]), r1 = sub(r6, b[3]);
            if (r2 != r1) {
                const std::int32_t t = div(std::int32_t(std::uint32_t(r1) << 12), sub(r1, r2));
                const std::int32_t r5 = sub(b[3], asr(mul(sub(b[3], b[2]), t), 12));
                b[3] = r5;
                if (r5 <= 0) return 1;
                const std::int32_t r0 = sub(b[1], asr(mul(sub(b[1], b[0]), t), 12));
                b[1] = r0;
                b[5] = sub(b[5], asr(mul(sub(b[5], b[4]), t), 12));
                b[7] = div(std::int32_t(std::uint32_t(r0) << 12), r5);
            }
        }
        return b[7] < b[6] ? 1 : 0;
    }

    // ---- 0x03004BA0: wall edges per column, and the "already hidden" test --------
    int project(std::int32_t* b, std::int32_t d1, std::int32_t d2) {
        std::int32_t sb, sl, fp, ip;
        recip(d1, sb, sl);
        recip(d2, fp, ip);
        std::int32_t r6 = 8, r7 = 8;
        if (sl < 0) r6 = 8 - sl;
        if (sl <= 0) sl = 0;
        if (ip < 0) r7 = 8 - ip;
        if (ip <= 0) ip = 0;
        std::int32_t lr = view(0xE4);
        auto edge1 = [&](std::int32_t v) { return add(lr, asr(smull_hi(sb, std::int32_t(lsl(std::uint32_t(v), unsigned(r6)))), unsigned(sl))); };
        auto edge2 = [&](std::int32_t v) { return add(lr, asr(smull_hi(fp, std::int32_t(lsl(std::uint32_t(v), unsigned(r7)))), unsigned(ip))); };
        std::int32_t r0 = view(0xF4);
        set_view(0x40, edge1(r0));
        std::int32_t r2 = edge2(r0);
        set_view(0x48, r2);
        r0 = view(0xF0);
        set_view(0x3C, edge1(r0));
        r2 = edge2(r0);
        set_view(0x44, r2);
        if (view(0x1C) != 0) {
            r0 = view(0xE8);
            set_view(0x4C, edge1(r0));
            set_view(0x54, edge2(r0));
        } else {
            set_view(0x54, r2);
            set_view(0x4C, view(0x3C));
            r0 = view(0xF0);
        }
        r0 = add(r0, 0x8000);
        set_view(0x5C, edge1(r0));
        set_view(0x60, edge2(r0));
        r0 = view(0xEC);
        if (view(0x20) != 0) {
            if (r0 > 0) lr -= 1;
            set_view(0x50, edge1(r0));
            set_view(0x58, edge2(r0));
        } else {
            set_view(0x50, view(0x40) - (r0 > 0 ? 1 : 0));
            set_view(0x58, view(0x48) - (r0 > 0 ? 1 : 0));
        }
        set_view(0xAC, b[7]);
        set_view(0xA8, b[6]);
        const std::int32_t span = sub(asr(b[7], 4), asr(b[6], 4));
        if (span != 0) {
            std::int32_t m, sh, rr;
            if (span >= 0x100) {
                recip(span, m, sh);
                rr = 8;
                if (sh < 0) rr = 8 - sh;
                if (sh <= 0) sh = 0;
            } else {
                m = r32(std::uint32_t(view(0x11C)) + std::uint32_t(span) * 4u);
                sh = 0;
                rr = 10;
            }
            auto step = [&](unsigned a, unsigned c) {
                return asr(smull_hi(m, std::int32_t(lsl(std::uint32_t(sub(view(c), view(a))), unsigned(rr)))), unsigned(sh));
            };
            const std::int32_t s94 = step(0x3C, 0x44);
            set_view(0x94, s94);
            const std::int32_t s98 = step(0x40, 0x48);
            set_view(0x98, s98);
            set_view(0xA4, step(0x5C, 0x60));
            set_view(0x9C, view(0x1C) == 0 ? s94 : step(0x4C, 0x54));
            set_view(0xA0, view(0x20) == 0 ? s98 : step(0x50, 0x58));
        }
        // hidden test
        const std::uint32_t F = std::uint32_t(view(0x104)), B = std::uint32_t(view(0x108));
        set_view(0x118, 0);
        if (view(0x74) != -1) return 0;
        if (rs16(F + 0xE) == -2 && rs16(B + 0xE) == -2) {
            const std::int32_t vz = r32(0x03002828u);
            if (r32(F + 0x20) - vz <= 0 && r32(B + 0x20) - vz <= 0 && rs16(B + 2) != rs16(B + 4))
                set_view(0x118, 1);
        }
        if (r32(F + 0x18) & 1) return 0;
        const std::int32_t lightdiff = sub(r32(F), r32(B));
        const std::int32_t fct = rs16(F + 0xE);
        const std::uint32_t clip_base = std::uint32_t(view(0xBC));
        if (!(fct == rs16(B + 0xE) && lightdiff == 0 && r32(F + 0x24) == r32(B + 0x24))) {
            std::int32_t c5 = asr(b[6], 4), c8 = asr(b[7], 4);
            if (fct == -2) c8 += 1;
            std::int32_t e = view(0x4C), st = view(0x3C);
            if (st > e) { e = st; st = view(0x94); } else st = view(0x9C);
            e = std::int32_t(std::uint32_t(e) << 8);
            const std::int32_t d = sub(view(0xF8), c5);
            if (d > 0) { e = add(e, mul(st, d)); c5 += d; }
            c5 += 1;
            if (c8 >= view(0xFC)) c8 = view(0xFC) - 1;
            std::uint32_t p6 = clip_base + std::uint32_t(c5) * 2u;
            const std::uint32_t p8 = clip_base + std::uint32_t(c8) * 2u;
            if (!(p6 > p8)) {
                p6 += 1;
                std::uint32_t s = kSolid + std::uint32_t(c5);
                for (;;) {
                    if (r8(s++) == 0) {
                        const std::int32_t top = r8(p6);
                        p6 += 2;
                        if (top < asr(e, 8)) return 0;
                    }
                    e = add(e, st);
                    if (p6 > p8) break;
                }
            }
        }
        if (lightdiff == 0 && r32(F + 0x20) == r32(B + 0x20) && rs16(F + 0xC) == rs16(B + 0xC)) return 1;
        {
            std::int32_t c5 = asr(b[6], 4), c8 = asr(b[7], 4);
            if (fct == -2) c8 += 1;
            std::int32_t e = view(0x50), st = view(0x40);
            if (st < e) { e = st; st = view(0x98); } else st = view(0xA0);
            e = std::int32_t(std::uint32_t(e) << 8);
            const std::int32_t d = sub(view(0xF8), c5);
            if (d > 0) { e = add(e, mul(st, d)); c5 += d; }
            c5 += 1;
            if (c8 >= view(0xFC)) c8 = view(0xFC) - 1;
            std::uint32_t p6 = clip_base + std::uint32_t(c5) * 2u;
            const std::uint32_t p8 = clip_base + std::uint32_t(c8) * 2u;
            if (!(p6 > p8)) {
                std::uint32_t s = kSolid + std::uint32_t(c5);
                for (;;) {
                    if (r8(s++) == 0) {
                        const std::int32_t bottom = r8(p6);
                        p6 += 2;
                        if (bottom > asr(e, 8)) return 0;
                    }
                    e = add(e, st);
                    if (p6 > p8) break;
                }
            }
        }
        return 1;
    }

    // ---- 0x080211F8: sectors, wall parts, flags (occlusion side) -----------------
    int setup(std::int32_t* b, std::uint32_t seg) {
        const std::int32_t side = view(0x34);
        const std::uint32_t sides = std::uint32_t(r32(0x03002728u));
        const std::uint32_t S = sides + std::uint32_t(side) * 0x14u;
        const std::uint32_t sectors = std::uint32_t(r32(0x03002824u));
        const std::uint32_t F = sectors + std::uint32_t(rs16(S + 0xE)) * 0x28u;
        const std::uint32_t Ln = std::uint32_t(r32(0x03002884u)) + std::uint32_t(rs16(S + 0xC)) * 0x14u;
        const std::int32_t d1 = b[2], d2 = b[3];
        const std::uint32_t flags = r16(seg + 0xE);
        w8(kView + 4, flags & 1);
        b[6] = add(b[6], 0x3C0);
        b[7] = add(b[7], 0x3C0);
        const std::int32_t fc = asr(r32(F + 0x24), 8), ff = asr(r32(F + 0x20), 8);
        const std::int32_t vz = r32(0x03002828u);
        const std::int32_t fcr = sub(vz, std::int32_t(std::uint32_t(fc) << 8));
        const std::int32_t ffr = sub(vz, std::int32_t(std::uint32_t(ff) << 8));
        w32(0x030028D0u + 0x20, 0);
        std::int32_t bcr, bfr, bc = 0, bf = 0;
        std::uint32_t B = 0;
        if (!(flags & 0x2000)) {
            set_view(0x14, 0); set_view(0x18, 0); set_view(0x1C, 0); set_view(0x20, 0);
            bcr = fcr; bfr = ffr;
        } else {
            std::int32_t other = rs16(Ln + 4);
            if (other == side) other = rs16(Ln + 6);
            B = sectors + std::uint32_t(rs16(sides + std::uint32_t(other) * 0x14u + 0xE)) * 0x28u;
            bc = asr(r32(B + 0x24), 8);
            bf = asr(r32(B + 0x20), 8);
            if ((flags & 4) && bc == fc && bf == ff) return 1;
            const std::int32_t vz2 = r32(0x03002828u);
            bcr = sub(vz2, std::int32_t(std::uint32_t(bc) << 8));
            bfr = sub(vz2, std::int32_t(std::uint32_t(bf) << 8));
            set_view(0x14, (bf < ff || vz2 < std::int32_t(std::uint32_t(bf) << 8)) ? 1 : 0);
            set_view(0x18, (fc < bc || std::int32_t(std::uint32_t(bc) << 8) < r32(0x03002828u)) ? 1 : 0);
            if (r32(0x0300284Cu) != 0 && bf == ff) w32(0x030028D0u + 0x20, 1);
            set_view(0x1C, fcr < bcr ? 1 : 0);
            set_view(0x20, bfr < ffr ? 1 : 0);
            if (bcr < fcr) bcr = fcr;
            if (ffr < bfr) bfr = ffr;
        }
        set_view(0x28, std::int32_t(flags & 0x80));
        if (!(flags & 0x80)) {
            if (flags & 0x800) { set_view(0x1C, 0); set_view(0x74, -1); }
            w32(0x03002E80u, 1);
        }
        set_view(0x74, rs16(S + 2));
        set_view(0xE8, fcr); set_view(0xEC, ffr); set_view(0xF0, bcr); set_view(0xF4, bfr);
        set_view(0x104, std::int32_t(F)); set_view(0x108, std::int32_t(B));
        if (project(b, d1, d2) == 1) return 1;
        if (r8(kView + 4) != 0) {
            std::int32_t s48 = 0, s3c = 0;   // s3c: texture only, kept for the read order
            set_view(0xDC, (flags & 0x4000) ? 0x08020E19 : 0x08020DBD);
            // Upper and lower: only whether a texture exists matters here.
            if (view(0x1C) != 0 && rs16(S) == -1) set_view(0x1C, 0);
            if (view(0x20) != 0 && rs16(S + 4) == -1) set_view(0x20, 0);
            std::int32_t mid = view(0x74);
            if (mid >= 0) {
                // middle: animation follows the original entry's flags, the
                // scroll the animated entry's
                const std::uint32_t dir = std::uint32_t(r32(0x03002474u));
                std::uint32_t e = dir + std::uint32_t(mid) * 0x40u;
                if (r8(e + 0x3C) & 0x10) {
                    mid = mid + rs8(std::uint32_t(r32(0x03000D94u)) + std::uint32_t(mid));
                    set_view(0x74, mid);
                    e = dir + std::uint32_t(mid) * 0x40u;
                    if (rs8(e + 0x3C) & 0x80) {
                        if (r32(e + 0xC) == 0) s48 = r32(0x03002E50u);
                        else s3c = r32(0x03002E68u);
                    }
                }
                const std::int32_t h1 = r32(e) - 1;
                set_view(0x7C, h1);
                std::int32_t yoff = rs16(S + 0x12);
                if (r16(Ln + 8) & 0x10) {
                    std::int32_t v;
                    if (B == 0) v = ff - fc;
                    else {
                        std::int32_t lo = ff < bf ? bf : ff;
                        std::int32_t hi = bc < fc ? bc : fc;
                        v = lo - hi;
                    }
                    set_view(0x78, v);
                    yoff = add(view(0x78), yoff);
                }
                set_view(0x78, std::int32_t(std::uint32_t(add(yoff, s48) & h1) << 12));
                if ((r8(e + 0x3C) & 2) && (r16(Ln + 8) & 4)) {
                    set_view(0xDC, 0x08020CF9);
                    if (r16(Ln + 8) & 2) set_view(0x78, 0);
                    const std::int32_t need = sub(std::int32_t(std::uint32_t(view(0x7C) + 1) << 12), view(0x78));
                    const std::int32_t top = bc < fc ? bc : fc;
                    const std::int32_t bottom = ff < bf ? bf : ff;
                    const std::int32_t h = top - bottom;
                    set_view(0x2C, need <= std::int32_t(std::uint32_t(h) << 12) ? div(need, h) : 0x1000);
                }
            }
        }
        if (!(flags & 0x40)) { set_view(0x24, 0); w32(0x03002E80u, 1); }
        else set_view(0x24, 1);
        if (r32(0x030028DCu) == 0) w16(Ln + 8, r16(Ln + 8) | 0x400u);
        return 0;
    }

    // ---- 0x08002D20: perspective pieces ------------------------------------------
    void subdivide(std::int32_t* b) {
        std::int32_t lim = b[3];
        if (lim >= b[2]) lim = b[2];
        lim = asr(lim + 0x800, 8);
        lim = asr(mul(lim, lim), 7);
        if (lim < 0x180) lim = 0x180;
        std::int32_t done = 0, count = view(0xD8);
        std::int32_t* p = b;
        do {
            if (sub(p[7], p[6]) < lim) { ++done; p += 10; }
            else {
                std::int32_t* n = b + count * 10;
                n[7] = p[7];
                n[1] = p[1];
                const std::int32_t x = asr(add(p[1], p[0]), 1);
                n[3] = p[3];
                const std::int32_t d = asr(add(p[3], p[2]), 1);
                n[5] = p[5];
                const std::int32_t u = asr(add(p[5], p[4]), 1);
                n[0] = x; n[2] = d;
                const std::int32_t c = add(div(std::int32_t(std::uint32_t(x) << 12), d), 0x3C0);
                const std::int32_t col = asr(c, 4);
                if (col > view(0xFC)) { p[7] = c; p[5] = u; p[1] = n[0]; p[3] = n[2]; }
                else if (col < view(0xF8)) { p[6] = c; p[4] = u; p[0] = n[0]; p[2] = n[2]; }
                else { p[5] = u; n[4] = u; p[1] = n[0]; p[3] = n[2]; p[7] = c; n[6] = c; ++count; }
            }
        } while (count < 0x14 && done < count);
        set_view(0xD8, count);
    }

    // The loops' self-modifying set-up (0x030060C4, 0x03006BAC) is not
    // reproduced: the game rewrites those slots and operands itself before
    // every use of the loops, and nothing else reads them.

    void close_column(std::int32_t col) {
        w32(kSolidCount, r32(kSolidCount) + 1);
        w8(kSolid + std::uint32_t(col), 1);
    }

    // ---- 0x03006300: the general column loop (occlusion, plane rows, counts) -----
    void loop_general(std::int32_t* piece, std::int32_t clipL, std::int32_t clipR) {
        const std::int32_t x1 = piece[6], x2 = piece[7];
        std::int32_t r6 = sub(x2, x1);
        if (r6 < 0) return;
        std::int32_t r5 = asr(x2, 4), r7 = asr(x1, 4), r8 = 0;
        std::int32_t sb = sub(r7, asr(view(0xA8), 4));
        if (r7 < clipL && r7 != r5) { r8 = clipL - r7; sb += r8; r7 += r8; }
        std::int32_t s4c = std::int32_t(std::uint32_t(view(0x5C)) << 8);
        std::int32_t s0 = std::int32_t(std::uint32_t(view(0x3C)) << 8);
        std::int32_t s4 = std::int32_t(std::uint32_t(view(0x40)) << 8);
        std::int32_t s8 = std::int32_t(std::uint32_t(view(0x4C)) << 8);
        std::int32_t sC = std::int32_t(std::uint32_t(view(0x50)) << 8);
        if (sb != 0) {
            s4c = add(s4c, mul(view(0xA4), sb));
            s0 = add(s0, mul(view(0x94), sb));
            s4 = add(s4, mul(view(0x98), sb));
            s8 = add(s8, mul(view(0x9C), sb));
            sC = add(sC, mul(view(0xA0), sb));
        }
        if (r5 >= clipR) r5 = clipR - 1;
        r7 += 1;
        if (r7 > r5) return;
        const bool textured = r32(kView + 4) != 0;   // the loop tests the word
        if (r6 != 0 && textured && r6 >= 0x100)
            (void)div(std::int32_t(std::uint32_t(sub(piece[5], piece[4])) << 12), r6);
        const bool sky = view(0x118) != 0;
        const bool upper = view(0x1C) != 0, lower = view(0x20) != 0;
        const bool mid = view(0x74) != -1, seethrough = this->r8(kView + 5) != 0;
        const std::uint32_t clip_base = std::uint32_t(view(0xBC)), planes = std::uint32_t(view(0xC0));
        for (;;) {
            const std::uint32_t cp = clip_base + std::uint32_t(r7) * 2u;
            const std::uint32_t v = r16(cp);
            std::int32_t top = std::int32_t(v >> 8), bot = std::int32_t(v & 0xFF);
            bool closed = false;
            if (!(top >= bot - 1) && sub(sC, s8) > 0 && sub(s4c, s8) > 0) {
                const std::int32_t scale = sub(s4c, s8);
                std::int32_t pb = view(0x110);
                std::int32_t a = asr(sC, 8);
                if (a > 0xA1) a = 0xA1;
                a -= 1;
                if (a < 0) a = 0;
                if (a < bot) pb |= 2;
                if (!sky) {
                    std::int32_t c = add(asr(s8, 8), 1);
                    if (c < 0) c = 0;
                    if (c > 0xA1) c = 0xA1;
                    if (c > top) pb |= 1;
                    set_view(0x110, pb);
                    w16(planes + std::uint32_t(r7) * 2u, std::uint32_t(a) | std::uint32_t(c) << 8);
                } else {
                    set_view(0x110, pb);
                    w8(planes + std::uint32_t(r7) * 2u, std::uint32_t(a));
                }
                if (textured) {
                    const std::uint32_t q = std::uint32_t(scale) >> 8;
                    if (q > 0xFF0u) (void)div(0x80000, std::int32_t(q));
                }
                // ceiling
                std::int32_t e5 = add(asr(s8, 8), 1);
                if (e5 > top) {
                    if (e5 > bot) e5 = bot;
                    top = e5 - 1;
                }
                std::int32_t e6 = asr(s0, 8);
                const std::int32_t ip = asr(sC, 8);
                if (upper && e6 > top) {
                    if (e6 > bot) e6 = bot;
                    if (e6 > ip) e6 = ip;
                    top = e6 - 1;
                }
                std::int32_t r2 = e6;
                e6 = asr(s4, 8);
                if (mid && e6 > top) {
                    if (seethrough) {
                        std::int32_t lim = add(r2, asr(mul(sub(e6, r2), view(0x2C)), 12));
                        if (lim > bot) lim = bot;
                        if (lim > ip) lim = ip;
                        std::int32_t r3 = r2;
                        if (top >= r3) r3 = top + 1;
                        if (lim > r3) {
                            if (r32(kListCount) > 0x750) set_view(0x30, -1);
                            else {
                                w32(kListCount, r32(kListCount) + 7);
                                set_view(0x30, view(0x30) + 1);
                            }
                        }
                    } else {
                        if (e6 > bot) e6 = bot;
                        if (e6 > ip) e6 = ip;
                        top = e6 - 1;
                    }
                }
                if (ip < bot) {
                    std::int32_t r1 = ip;
                    if (r1 <= top) r1 = top + 1;
                    bot = r1;
                }
                if (lower && e6 < bot) {
                    if (e6 < e5) e6 = e5;
                    std::int32_t r1 = e6;
                    if (r1 <= top) r1 = top + 1;
                    bot = r1;
                }
                if (!sky && top >= bot - 1) closed = true;
            }
            if (closed) close_column(r7);
            if (sky) {
                w8(cp, std::uint32_t(bot));
            } else {
                w16(cp, std::uint32_t(bot) | std::uint32_t(top) << 8);
            }
            r7 += 1;
            if (r7 > r5) break;
            s0 = add(s0, view(0x94));
            s4 = add(s4, view(0x98));
            s8 = add(s8, view(0x9C));
            sC = add(sC, view(0xA0));
            s4c = add(s4c, view(0xA4));
        }
    }

    // ---- 0x03006C10: the one-sided column loop -----------------------------------
    void loop_onesided(std::int32_t* piece, std::int32_t clipL, std::int32_t clipR) {
        const std::int32_t x1 = piece[6], x2 = piece[7];
        const std::int32_t r6 = sub(x2, x1);
        if (r6 < 0) return;
        std::int32_t r4 = asr(x1, 4), r0 = asr(x2, 4);
        std::int32_t r8 = sub(r4, asr(view(0xA8), 4));
        if (r4 < clipL && r4 != r0) { const std::int32_t d = clipL - r4; r8 += d; r4 += d; }
        if (r0 >= clipR) r0 = clipR - 1;
        r4 += 1;
        if (r4 > r0) return;
        if (r6 != 0 && r6 >= 0x100) (void)div(std::int32_t(std::uint32_t(sub(piece[5], piece[4])) << 12), r6);
        std::int32_t a0 = std::int32_t(std::uint32_t(view(0x4C)) << 8);
        std::int32_t a4 = std::int32_t(std::uint32_t(view(0x50)) << 8);
        std::int32_t a20 = std::int32_t(std::uint32_t(view(0x5C)) << 8);
        if (r8 != 0) {
            a0 = add(mul(view(0x9C), r8), a0);
            a4 = add(mul(view(0xA0), r8), a4);
            a20 = add(mul(view(0xA4), r8), a20);
        }
        const std::uint32_t clip_base = std::uint32_t(view(0xBC)), planes = std::uint32_t(view(0xC0));
        for (;;) {
            const std::uint32_t cp = clip_base + std::uint32_t(r4) * 2u;
            const std::uint32_t v = r16(cp);
            std::int32_t bot = std::int32_t(v & 0xFF), top = std::int32_t(v >> 8);
            if (!(top >= bot - 1) && sub(a4, a0) > 0 && sub(a20, a0) > 0) {
                std::int32_t e8 = asr(a0, 8);
                const std::int32_t e9 = asr(a4, 8);
                const std::uint32_t q = std::uint32_t(add(sub(a20, a0), 0x100));
                std::int32_t pb = view(0x110);
                std::int32_t a = e9;
                if (a > 0xA1) a = 0xA1;
                a -= 1;
                if (a < 0) a = 0;
                if (a < bot) pb |= 2;
                std::int32_t c = e8;
                if (c < 0) c = 0;
                if (c > 0xA1) c = 0xA1;
                if (c > top) pb |= 1;
                set_view(0x110, pb);
                w16(planes + std::uint32_t(r4) * 2u, std::uint32_t(a) | std::uint32_t(c) << 8);
                if ((q >> 8) > 0xFF0u) (void)div(0x80000, std::int32_t(q >> 8));
                if (e8 > top) {
                    if (e8 > bot) e8 = bot;
                    top = e8 - 1;
                }
                if (e9 > top) {
                    std::int32_t r1 = e9;
                    if (r1 > bot) r1 = bot;
                    top = r1 - 1;
                }
                if (e9 <= bot) bot = e9 <= top ? top + 1 : e9;
                close_column(r4);
            }
            w16(cp, std::uint32_t(bot) | std::uint32_t(top) << 8);
            r4 += 1;
            if (r4 > r0) break;
            a0 = add(a0, view(0x9C));
            a4 = add(a4, view(0xA0));
            a20 = add(a20, view(0xA4));
        }
    }

    // ---- 0x080210A0 ----------------------------------------------------------------
    int wall(std::uint32_t seg, std::int32_t L, std::int32_t R) {
        std::int32_t b[10 * 21] = {};
        set_view(0xD8, 1);
        if (r32(0x0300284Cu) != 0 && (r16(seg + 0xE) & 8)) return 1;
        if (clip(b, seg, L, R) == 1) return 1;
        if (cull(b) == 1) return 1;
        set_view(0x34, std::int32_t(r16(seg + 6)));
        set_view(0xF8, L);
        set_view(0xFC, R);
        if (setup(b, seg) == 1) return 1;
        set_view(0x110, 0);   // 0x0801FABC starts the wall's plane bits from zero
        if (r8(kView + 4) == 0) {
            b[9] = 1;
            w8(kView + 5, 0);
            loop_general(b, L, R);
        } else {
            subdivide(b);
            const std::int32_t part = view(0xDC);
            const std::int32_t n = view(0xD8);
            if (part == 0x08020E19) {
                for (std::int32_t i = 0; i < n; ++i) loop_onesided(b + i * 10, L, R);
            } else if (part == 0x08020CF9) {
                const std::int32_t before = r32(kListCount);
                set_view(0x30, 0);
                w8(kView + 5, 1);
                for (std::int32_t i = 0; i < n; ++i) loop_general(b + i * 10, L, R);
                // 0x08020CF8: a header record when any column was recorded,
                // otherwise the count goes back to where it was
                if (view(0x30) < 1) w32(kListCount, before);
                else w32(kListCount, r32(kListCount) + 0xD);
            } else {
                w8(kView + 5, 0);
                for (std::int32_t i = 0; i < n; ++i) loop_general(b + i * 10, L, R);
            }
        }
        return 0;
    }
};

}  // namespace

bool supported(const std::uint8_t* iwram, const std::uint8_t* ewram, std::uint32_t seg_addr) {
    Hle h{const_cast<std::uint8_t*>(iwram), const_cast<std::uint8_t*>(ewram), nullptr, 0};
    if (h.r32(kView + 0x10) == 0) return false;      // the frame's first wall waits for the flip
    if (h.r32(0x030028DCu) != 0) return false;       // mirror pass
    if (h.r32(0x03002834u) != 0x08002F51 || h.r32(0x03002E84u) == 0) return false;
    const std::uint32_t flags = h.r16(seg_addr + 0xE);
    if (!(flags & 0x2000)) {                         // one-sided without a middle: game's code
        const std::uint32_t S = std::uint32_t(h.r32(0x03002728u)) + std::uint32_t(h.r16(seg_addr + 6)) * 0x14u;
        if (h.rs16(S + 2) == -1) return false;
    }
    return true;
}

int run(std::uint8_t* iwram, std::uint8_t* ewram, const std::uint8_t* rom, std::size_t rom_size,
        std::uint32_t seg_addr, std::int32_t left, std::int32_t right) {
    Hle h{iwram, ewram, rom, rom_size};
    return h.wall(seg_addr, left, right);
}

}  // namespace dukehle

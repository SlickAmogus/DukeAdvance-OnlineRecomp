// Native scene renderer for Duke Nukem Advance (AD9P).
//
// Reads the level the game has loaded (BSP, sectors, walls, sprites) straight
// out of guest memory and draws the 3D view at any resolution and aspect
// ratio. It never writes guest memory. Formats: docs/ENGINE_NOTES.md.
#pragma once

#include <cstddef>
#include <cstdint>

namespace dukescene {

// Read-only views of guest memory for one frame.
struct Memory {
    const std::uint8_t* ewram = nullptr;    // 0x02000000, 256 KiB
    const std::uint8_t* iwram = nullptr;    // 0x03000000, 32 KiB
    const std::uint8_t* rom = nullptr;      // 0x08000000
    std::size_t rom_size = 0;
    const std::uint8_t* palette = nullptr;  // 0x05000000, BG palette (512 bytes)
    const std::uint8_t* oam = nullptr;      // 0x07000000, 1 KiB (optional)
};

struct Options {
    int width = 240;
    int height = 160;
    // 1.0 keeps the game's vertical field of view; the horizontal one follows
    // the aspect ratio.
    float fov_scale = 1.0f;
    int threads = 0;             // 0 = pick from hardware
    bool sprites = true;
    // Extra fractional look pitch (in 160-line pixels) on top of the game's.
    float pitch_offset = 0.0f;
    // Camera to use instead of the game's current view globals (which a
    // mirror pass changes temporarily). Angle: 0x10000 = 360 degrees.
    bool use_view = false;
    float view_x = 0, view_y = 0, view_z = 0;
    std::uint16_t view_angle = 0;
};

// True when a level is loaded and the game is in its 3D view.
bool in_level(const Memory& mem);

// Renders into `out` (width*height, 0xAARRGGBB, row-major). Returns false and
// leaves `out` untouched when no level is loaded.
bool render(const Memory& mem, const Options& opts, std::uint32_t* out);

}  // namespace dukescene

// Native replacement of the game's per-wall routine (ROM 0x080210A0) for the
// occlusion, counters and caches it updates; see scene_hle.cpp.
#pragma once
#include <cstddef>
#include <cstdint>

namespace dukehle {

// False for walls the native code leaves to the game: the frame's first wall
// (which waits for the page flip), mirror passes, unexpected loop pointers,
// one-sided walls without a middle texture.
bool supported(const std::uint8_t* iwram, const std::uint8_t* ewram, std::uint32_t seg_addr);

// Runs the wall on the given RAM; returns what the game's routine returns
// (1 = not drawn).
int run(std::uint8_t* iwram, std::uint8_t* ewram, const std::uint8_t* rom, std::size_t rom_size,
        std::uint32_t seg_addr, std::int32_t left, std::int32_t right);

}  // namespace dukehle

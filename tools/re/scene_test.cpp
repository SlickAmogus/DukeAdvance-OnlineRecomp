// Offline harness for src/scene: renders a snapshot captured by snapshot.py.
//   scene_test <rom> <snapshot prefix> <width> <height> <out.ppm> [fov] [threads] [repeat]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../../src/scene/scene.h"

static std::vector<unsigned char> load(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(f), {});
}

int main(int argc, char** argv) {
    if (argc < 6) return 2;
    const std::string pre = argv[2];
    auto rom = load(argv[1]), ew = load(pre + ".ewram"), iw = load(pre + ".iwram"), pal = load(pre + ".pal");
    if (rom.empty() || ew.size() < 0x40000 || iw.size() < 0x8000 || pal.size() < 512) return 3;
    dukescene::Memory m{ew.data(), iw.data(), rom.data(), rom.size(), pal.data()};
    dukescene::Options o;
    o.width = std::atoi(argv[3]); o.height = std::atoi(argv[4]);
    if (argc > 6) o.fov_scale = static_cast<float>(std::atof(argv[6]));
    if (argc > 7) o.threads = std::atoi(argv[7]);
    const int repeat = argc > 8 ? std::atoi(argv[8]) : 1;
    std::vector<std::uint32_t> out(static_cast<std::size_t>(o.width) * o.height);
    bool ok = false;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < repeat; ++i) ok = dukescene::render(m, o, out.data());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / repeat;
    std::printf("render %s, %.2f ms/frame\n", ok ? "ok" : "FAILED", ms);
    std::ofstream f(argv[5], std::ios::binary);
    f << "P6\n" << o.width << " " << o.height << "\n255\n";
    for (std::uint32_t c : out) { const char px[3] = {char(c >> 16), char(c >> 8), char(c)}; f.write(px, 3); }
    return ok ? 0 : 1;
}

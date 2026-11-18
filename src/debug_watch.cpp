// Debug-only RAM write watch, reachable over the debug TCP server:
//
//   {"cmd":"duke_watch","addr":50338448,"len":4}   arm (len 0 disarms)
//   {"cmd":"duke_watch_log"}                        dump + clear hits
//   {"cmd":"duke_mouse","dx":50,"neg":0,"dy":9,"negy":1}   synthetic mouse counts
//                                       (neg/negy = 1 makes that axis negative)
//
// While armed every RAM store goes through the bus (the native RAM fast path
// stands down when an observer is installed), so it is slow. Reverse
// engineering aid only; nothing in normal play installs it.
#include "debug_watch.h"
#include "pc_controls.h"
#include "scene_host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gba_bus.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

namespace {

struct Hit { uint32_t addr, pc, lr, old_value, new_value; uint8_t width; };

struct Watch final : gba::BusWriteObserver {
    uint32_t lo = 0, hi = 0;
    std::vector<Hit> hits;
    bool on_bus_write(gba::BusWriteRegion, uint32_t, uint32_t addr, uint8_t width,
                      uint32_t old_value, uint32_t new_value) override {
        if (addr < hi && addr + width > lo && hits.size() < 4096)
            hits.push_back({addr, g_cpu.R[15], g_cpu.R[14], old_value, new_value, width});
        return false;   // never suppress the store
    }
} g_watch;

bool find_uint(const char* req, const char* key, unsigned long long* out) {
    const char* p = std::strstr(req, key);
    if (!p) return false;
    p = std::strchr(p, ':');
    if (!p) return false;
    *out = std::strtoull(p + 1, nullptr, 0);
    return true;
}

}  // namespace

int duke_debug_tcp_command(const char* request,
                           void (*write)(void*, const char*, std::size_t),
                           void* ctx) {
    auto reply = [&](const std::string& s) { write(ctx, s.data(), s.size()); };
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (std::strstr(request, "\"duke_mouse\"")) {
        unsigned long long dx = 0, neg = 0, dy = 0, negy = 0;
        find_uint(request, "\"dx\"", &dx);
        find_uint(request, "\"neg\"", &neg);
        find_uint(request, "\"dy\"", &dy);
        find_uint(request, "\"negy\"", &negy);
        duke_pc_controls_debug_mouse(neg ? -static_cast<int>(dx) : static_cast<int>(dx),
                                     negy ? -static_cast<int>(dy) : static_cast<int>(dy));
        reply("{\"ok\":true}");
        return 1;
    }
    if (std::strstr(request, "\"duke_scene_shot\"")) {
        // {"cmd":"duke_scene_shot","w":1720,"h":720,"native":1} -> scene_shot.ppm
        unsigned long long w = 960, h = 640, native = 1;
        find_uint(request, "\"w\"", &w);
        find_uint(request, "\"h\"", &h);
        if (find_uint(request, "\"native\"", &native)) duke_scene_settings()->native = native != 0;
        const bool ok = duke_scene_debug_shot("scene_shot.ppm", static_cast<int>(w), static_cast<int>(h));
        if (!ok) { reply("{\"ok\":false,\"error\":\"not in the 3D view\"}"); return 1; }
        const DukeSceneView v = duke_scene_debug_view();
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "{\"ok\":true,\"angle\":%u,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"player\":%u}",
                      v.angle, static_cast<double>(v.x), static_cast<double>(v.y),
                      static_cast<double>(v.z), duke_pc_controls_player_entity());
        reply(buf);
        return 1;
    }
    if (std::strstr(request, "\"duke_watch_log\"")) {
        std::string out = "{\"ok\":true,\"hits\":[";
        char buf[160];
        for (std::size_t i = 0; i < g_watch.hits.size(); ++i) {
            const Hit& h = g_watch.hits[i];
            std::snprintf(buf, sizeof(buf),
                          "%s{\"addr\":%u,\"pc\":%u,\"lr\":%u,\"old\":%u,\"new\":%u,\"w\":%u}",
                          i ? "," : "", h.addr, h.pc, h.lr, h.old_value, h.new_value, h.width);
            out += buf;
        }
        out += "]}";
        g_watch.hits.clear();
        reply(out);
        return 1;
    }
    if (std::strstr(request, "\"duke_watch\"")) {
        unsigned long long addr = 0, len = 0;
        find_uint(request, "\"addr\"", &addr);
        find_uint(request, "\"len\"", &len);
        if (!bus) { reply("{\"ok\":false,\"error\":\"no bus\"}"); return 1; }
        g_watch.lo = static_cast<uint32_t>(addr);
        g_watch.hi = static_cast<uint32_t>(addr + len);
        g_watch.hits.clear();
        bus->set_write_observer(len ? &g_watch : nullptr);
        reply("{\"ok\":true}");
        return 1;
    }
    return 0;
}

// PC menus drawn over the game in its own style (see pc_menu.h).
//
// Everything is drawn through the runtime's host overlay: rows added to the
// game's own menus are drawn in GBA pixels over its picture (so the mouse
// mapping of src/pc_controls.cpp still lines up), the full menus in a panel
// across the window. The text is a small bitmap font (menu_font.h) coloured
// like the game's option list: a yellow gradient with a black outline.
#include "pc_menu.h"

#include "menu_font.h"
#include "pc_controls.h"
#include "scene_host.h"

#include "gba_bus.h"
#include "host_overlay.h"
#include "recomp_runtime_ui.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"
#include "touch_input.h"

#if defined(DUKE_NETPLAY)
#include "online/online_match.h"
#include "online/room_client.h"
#endif

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

using gbarecomp::HostOverlay;
using gbarecomp::OverlayColor;
using gbarecomp::OverlaySpace;

// ── the game's menus (AD9P) ─────────────────────────────────────────────────
constexpr std::uint32_t kMainMenu = 0x08033CE0u;     // START, CONTINUE, OPTIONS, MULTIPLAYER
constexpr std::uint32_t kOptionsMenu = 0x080348FCu;  // EDIT CONTROL SETTINGS ... GAMMA
constexpr int kMainOptions = 2, kOptionsGamma = 5;
enum : unsigned { kA = 1u << 0, kB = 1u << 1, kStart = 1u << 3, kRight = 1u << 4, kLeft = 1u << 5,
                  kUp = 1u << 6, kDown = 1u << 7 };

// Where the added entries sit on the GBA screen.
struct Box { int x0, y0, x1, y1; bool has(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; } };
constexpr Box kPcSettingsRow{40, 129, 200, 141};        // under GAMMA (y 119)
constexpr Box kGammaRow{40, 116, 200, 128};
constexpr Box kMultiplayerItem{62, 125, 178, 141};      // the game's own MULTIPLAYER sprite
constexpr Box kOptionsItem{62, 101, 178, 118};

// ── text ────────────────────────────────────────────────────────────────────
enum class Ink { Normal, Selected, Dim, Title, White };

struct Pen {
    HostOverlay* ov = nullptr;
    OverlaySpace space = OverlaySpace::View;
    float px = 1.0f;          // one font pixel in `space` units
    float ox = 0, oy = 0;     // origin
};

OverlayColor ramp(Ink ink, int row, int alpha) {
    const float t = std::clamp(row / 8.0f, 0.0f, 1.0f);
    auto mix = [&](int r0, int g0, int b0, int r1, int g1, int b1) {
        return OverlayColor{static_cast<std::uint8_t>(r0 + (r1 - r0) * t), static_cast<std::uint8_t>(g0 + (g1 - g0) * t),
                            static_cast<std::uint8_t>(b0 + (b1 - b0) * t), static_cast<std::uint8_t>(alpha)};
    };
    switch (ink) {
    case Ink::Selected: return mix(255, 255, 255, 255, 214, 70);
    case Ink::Dim:      return mix(150, 140, 120, 95, 85, 70);
    case Ink::Title:    return mix(255, 110, 70, 170, 20, 10);
    case Ink::White:    return mix(255, 255, 255, 200, 200, 200);
    default:            return mix(255, 250, 60, 225, 120, 25);
    }
}

int text_width(const std::string& s) {
    int w = 0;
    for (char ch : s) {
        const int c = static_cast<unsigned char>(ch);
        w += (c >= 0x20 && c < 0x7F) ? kMenuFontWidth[c - 0x20] : 6;
    }
    return w;
}

// Horizontal runs of each glyph row: an outline pass (each run grown by one
// pixel all round, in black), then the coloured pass.
void draw_text(const Pen& pen, float x, float y, const std::string& s, Ink ink, int alpha = 255) {
    if (!pen.ov) return;
    for (int pass = 0; pass < 2; ++pass) {
        float cx = x;
        for (char ch : s) {
            const int c = static_cast<unsigned char>(ch);
            if (c > 0x20 && c < 0x7F) {
                for (int row = 0; row < kMenuFontHeight; ++row) {
                    const std::uint8_t bits = kMenuFontRows[c - 0x20][row];
                    for (int bx = 0; bx < 8;) {
                        if (!(bits & (0x80 >> bx))) { ++bx; continue; }
                        int ex = bx;
                        while (ex < 8 && (bits & (0x80 >> ex))) ++ex;
                        const float rx = pen.ox + (cx + bx) * pen.px, ry = pen.oy + (y + row) * pen.px;
                        if (pass == 0)
                            pen.ov->fill_rect(rx - pen.px, ry - pen.px, (ex - bx + 2) * pen.px, 3 * pen.px, 0,
                                              {0, 0, 0, static_cast<std::uint8_t>(alpha)}, pen.space);
                        else
                            pen.ov->fill_rect(rx, ry, (ex - bx) * pen.px, pen.px, 0, ramp(ink, row, alpha), pen.space);
                        bx = ex;
                    }
                }
            }
            cx += (c >= 0x20 && c < 0x7F) ? kMenuFontWidth[c - 0x20] : 6;
        }
    }
}

std::string upper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

// A panel like the game's: dark rust with a two-tone frame.
void panel(const Pen& pen, float x, float y, float w, float h) {
    auto r = [&](float a, float b, float c, float d, OverlayColor col) {
        pen.ov->fill_rect(pen.ox + a * pen.px, pen.oy + b * pen.px, c * pen.px, d * pen.px, 0, col, pen.space);
    };
    r(x, y, w, h, {24, 12, 8, 236});
    r(x, y, w, 2, {150, 92, 44, 255}); r(x, y + h - 2, w, 2, {70, 38, 18, 255});
    r(x, y, 2, h, {150, 92, 44, 255}); r(x + w - 2, y, 2, h, {70, 38, 18, 255});
    r(x + 3, y + 3, w - 6, 1, {60, 30, 16, 255});
}

// ── state ───────────────────────────────────────────────────────────────────
enum class Screen { Closed, Settings,
#if defined(DUKE_NETPLAY)
    Online, Name, Code, List, Room, Chat, Starting, Message,
#endif
};
enum class Entry { None, PcSettings, Online };

struct Hit { float x0, y0, x1, y1; int item; };
enum : int { kNoHit = -1000, kHitOk = -10, kHitCancel = -11, kHitChat = -12 };

struct Ui {
    Screen screen = Screen::Closed;
    int cursor = 0, scroll = 0;
    Entry focus = Entry::None;      // our row has the selection on a game menu
    Entry entry = Entry::None;      // which game menu row is shown this frame
    bool draining = false;          // keys stay from the game until all are up
    bool settling = false;          // just opened: wait for the opening press to end
    std::uint16_t prev_keys = 0x03FF;
    // full menus: virtual canvas
    int scale = 1, vw = 240, vh = 160, drawable_w = 0, drawable_h = 0;
    std::vector<Hit> hits;
    float mouse_vx = -1, mouse_vy = -1;
    std::uint32_t opened_ms = 0;
    // headless tests
    std::string test_key, test_text;
    float test_mx = -1, test_my = -1;
} g_ui;

bool is_open() { return g_ui.screen != Screen::Closed; }

void open_screen(Screen s) {
    g_ui.screen = s;
    g_ui.cursor = 0;
    g_ui.scroll = 0;
    g_ui.settling = true;
    g_ui.focus = Entry::None;
    g_ui.opened_ms = SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetTicks() : 0;
}

#if defined(DUKE_NETPLAY)
// ── online ──────────────────────────────────────────────────────────────────
using dukeonline::RoomClient;

std::filesystem::path g_ini;
std::string g_name = "Duke", g_server = dukeonline::kDefaultServer, g_build, g_exe, g_save_path;

void load_ini() {
    std::ifstream in(g_ini);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        if (k == "name" && !v.empty()) g_name = v.substr(0, 15);
        if (k == "server" && !v.empty()) g_server = v;
    }
}
void save_ini() {
    std::ofstream out(g_ini, std::ios::trunc);
    out << "# Duke Nukem Advance online: your name in rooms and chat, and the room server.\n"
        << "name=" << g_name << "\nserver=" << g_server << "\n";
}

// The link game the host picks. The game's own MULTIPLAYER menus set the
// rest; the opening script (opening_script) steps both consoles there.
// The link game is always DEATH MATCH for two players; the host picks the
// world and the limit (SERVER SETUP rows, in the game's own order: RIGHT steps
// forward and wraps). Sent as one byte: world | limit << 4.
const char* const kWorlds[] = {"AREA 51", "EGYPT 1", "EGYPT 2", "SYDNEY 1", "SYDNEY 2", "ALIEN 1", "ALIEN 2"};
const char* const kLimits[] = {"FIRST TO 10", "FIRST TO 20", "FIRST TO 30", "UNLIMITED", "5 MIN", "10 MIN", "20 MIN"};
constexpr int kWorldCount = 7, kLimitCount = 7;
int mode_world(int mode) { return (mode & 15) % kWorldCount; }
int mode_limit(int mode) { return ((mode >> 4) & 7) % kLimitCount; }
// Bit 7: the game runs 60 frames a second (the scene pass discounted on every
// console), as in single player. Off, it keeps its own pace (about 50 in open
// areas); 60 needs a faster PC, since each PC runs both consoles.
bool mode_fps60(int mode) { return (mode & 0x80) != 0; }

enum ControlKind : std::uint8_t { kStartNote = 1, kStartAck = 2, kChatNote = 3, kSettingsNote = 4 };

struct Online {
    std::unique_ptr<RoomClient> client;
    std::string edit, message;
    Screen back_to = Screen::Online;
    bool want_host = false, list_wanted = false, acked = false;
    std::uint16_t want_join = 0;
    int mode = 0;
    std::uint32_t match = 0, peer = 0;
    std::uint64_t start_sent_ms = 0, settings_sent_ms = 0, start_deadline = 0;
    std::vector<std::pair<std::string, bool>> pending_say;
} g_on;

void show_message(const std::string& text, Screen back) {
    g_on.message = upper(text);
    g_on.back_to = back;
    g_ui.screen = Screen::Message;
}

void ensure_client() {
    if (g_on.client && g_on.client->state() != RoomClient::State::Failed &&
        g_on.client->state() != RoomClient::State::Idle) return;
    g_on.client = std::make_unique<RoomClient>();
    g_on.client->on_relay = [](RoomClient::Channel ch, std::uint32_t from, const std::uint8_t* p, int len) {
        if (ch != RoomClient::kControl || len < 1) return;
        RoomClient& c = *g_on.client;
        std::string who = "?";
        for (const auto& m : c.members()) if (m.id == from) who = m.name;
        if (p[0] == kChatNote && len > 1) c.add_chat(who, std::string(reinterpret_cast<const char*>(p + 1), len - 1), true);
        if (p[0] == kSettingsNote && len >= 2 && from == c.host_id() && !c.is_host()) g_on.mode = p[1];
        if (p[0] == kStartNote && len >= 6 && from == c.host_id() && !c.is_host()) {
            std::memcpy(&g_on.match, p + 1, 4);
            g_on.mode = p[5];
            g_on.peer = from;
            g_ui.screen = Screen::Starting;
            g_on.start_deadline = dukeonline::now_ms() + 600;
        }
        if (p[0] == kStartAck && len >= 5 && c.is_host()) {
            std::uint32_t m; std::memcpy(&m, p + 1, 4);
            if (m == g_on.match) { g_on.acked = true; g_on.peer = from; }
        }
    };
    g_on.client->connect(g_server, g_name, g_build);
}

void send_settings() {
    const std::uint8_t note[2] = {kSettingsNote, static_cast<std::uint8_t>(g_on.mode)};
    g_on.client->send_relay(RoomClient::kControl, note, 2);
    g_on.settings_sent_ms = dukeonline::now_ms();
}

// Both consoles' pads from power-on to the first frame of the deathmatch, as
// the framework's agreed opening (GBARECOMP_NETPLAY_OPENING): "seat first
// last mask" per press. Every PC runs it alone at full speed from blank
// cartridges before connecting, so the match starts in the level a few
// seconds after START. Seat 0's console is the cable master (DUKE 1), which
// gets SERVER SETUP; the other waits, then both confirm the settings.
std::string opening_rows(int mode) {
    std::string out;
    unsigned t = 1500;
    auto press = [&](int seat, unsigned mask, unsigned gap) {
        for (int s = 0; s < 2; ++s)
            if (seat < 0 || seat == s)
                out += std::to_string(s) + " " + std::to_string(t) + " " + std::to_string(t + 3) + " " +
                       std::to_string(mask) + "\n";
        t += 4 + gap;
    };
    press(-1, kA, 200);                                  // language
    press(-1, kStart, 200);                              // title
    for (int i = 0; i < 3; ++i) press(-1, kDown, 40);    // MULTIPLAYER
    press(-1, kA, 400);
    // SERVER SETUP on the host: GAME TYPE (DEATH MATCH only), WORLD, SET
    // LIMIT, LAUNCH. RIGHT steps each choice forward from its first entry.
    // After moving to a row its value box slides in, and a RIGHT pressed
    // before it has (about 60 ticks) is lost: wait 150 after each DOWN.
    press(0, kDown, 150);
    for (int i = 0; i < mode_world(mode); ++i) press(0, kRight, 56);
    press(0, kDown, 150);
    for (int i = 0; i < mode_limit(mode); ++i) press(0, kRight, 56);
    press(0, kDown, 56);
    press(0, kA, 300);                                   // LAUNCH
    press(1, kA, 300);                                   // the other confirms
    press(0, kA, 0);                                     // and the host
    return out;
}

void restart_into_match() {
    RoomClient& c = *g_on.client;
    const bool host = c.is_host();
    const std::filesystem::path script = std::filesystem::temp_directory_path() /
        ("duke_online_" + std::to_string(g_on.match) + (host ? "_0" : "_1") + ".opening");
    { std::ofstream out(script, std::ios::trunc); out << opening_rows(g_on.mode); }
    std::string cmd = "\"" + g_exe + "\" --online-server \"" + g_server + "\" --online-name \"" + g_name +
        "\" --online-resume " + std::to_string(c.session()) + " --online-seat " + (host ? "0" : "1") +
        " --online-peer " + std::to_string(g_on.peer) + " --online-match " + std::to_string(g_on.match) +
        " --online-room " + std::to_string(c.room());
    if (!g_save_path.empty()) cmd += " --save-path \"" + g_save_path + "\"";
#if defined(_WIN32)
    SetEnvironmentVariableA("GBARECOMP_NETPLAY_OPENING", script.string().c_str());
    SetEnvironmentVariableA("GBARECOMP_NETPLAY_SCRIPT", nullptr);
    SetEnvironmentVariableA("DUKE_MATCH_FPS60", mode_fps60(g_on.mode) ? "1" : nullptr);
    // The new game writes to the same log as this one (when there is one).
    // Only those two handles are inherited: no sockets, no files.
    STARTUPINFOEXA si{}; si.StartupInfo.cb = sizeof(si);
    std::vector<HANDLE> inherit;
    for (DWORD which : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
        const HANDLE h = GetStdHandle(which);
        if (h && h != INVALID_HANDLE_VALUE && std::find(inherit.begin(), inherit.end(), h) == inherit.end() &&
            SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
            inherit.push_back(h);
    }
    std::vector<char> attrs;
    DWORD flags = 0;
    if (!inherit.empty()) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attrs.resize(size);
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
        if (InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size) &&
            UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                                      inherit.size() * sizeof(HANDLE), nullptr, nullptr)) {
            flags = EXTENDED_STARTUPINFO_PRESENT;
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
            si.StartupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        } else {
            si.lpAttributeList = nullptr;
        }
    }
    PROCESS_INFORMATION pi{};
    std::vector<char> line(cmd.begin(), cmd.end()); line.push_back('\0');
    const std::string dir = std::filesystem::path(g_exe).parent_path().string();
    std::fflush(stdout); std::fflush(stderr);
    const BOOL started = CreateProcessA(nullptr, line.data(), nullptr, nullptr, flags ? TRUE : FALSE, flags,
                                        nullptr, dir.c_str(), &si.StartupInfo, &pi);
    if (si.lpAttributeList) DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!started) {
        show_message("could not restart the game", Screen::Room);
        return;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
#else
    setenv("GBARECOMP_NETPLAY_OPENING", script.string().c_str(), 1);
    unsetenv("GBARECOMP_NETPLAY_SCRIPT");
    if (mode_fps60(g_on.mode)) setenv("DUKE_MATCH_FPS60", "1", 1); else unsetenv("DUKE_MATCH_FPS60");
    if (std::system((cmd + " &").c_str()) != 0) { show_message("could not restart the game", Screen::Room); return; }
#endif
    // Keep the room seat for the new process: no goodbye to the server.
    c.abandon();
    g_on.client.reset();
    g_ui.screen = Screen::Closed;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) { std::fflush(stderr); std::_Exit(0); }
    SDL_Event quit{}; quit.type = SDL_QUIT;
    SDL_PushEvent(&quit);
}

void pump_network() {
    if (!g_on.client) return;
    RoomClient& c = *g_on.client;
    c.pump();
    if (c.state() == RoomClient::State::Failed) {
        const std::string e = c.error();
        g_on.client.reset();
        g_on.want_host = false; g_on.want_join = 0;
        if (g_ui.screen != Screen::Closed && g_ui.screen != Screen::Settings)
            show_message(e.empty() ? "no connection" : e, Screen::Online);
        return;
    }
    if (c.state() != RoomClient::State::Connected) return;
    for (const auto& [text, room] : g_on.pending_say) c.say(text, room && c.room() != 0);
    g_on.pending_say.clear();
    if (g_on.list_wanted) { c.request_list(); g_on.list_wanted = false; }
    if (g_on.want_host) { c.create_room(2); g_on.want_host = false; }
    if (g_on.want_join) { c.join_room(g_on.want_join); g_on.want_join = 0; }
    if (!c.room_error().empty()) {
        const std::string e = c.room_error();
        c.clear_room_error();
        if (g_ui.screen == Screen::Message) show_message(e, Screen::Online);
    }
    if (c.room() && (g_ui.screen == Screen::Message || g_ui.screen == Screen::List || g_ui.screen == Screen::Code)) {
        g_ui.screen = Screen::Room; g_ui.cursor = 0;
    }
    if (!c.room() && g_ui.screen == Screen::Room) show_message("the room closed", Screen::Online);
    const auto now = dukeonline::now_ms();
    if (c.room() && c.is_host() && g_ui.screen == Screen::Room && now - g_on.settings_sent_ms > 2000) send_settings();
    if (g_ui.screen == Screen::Starting) {
        if (c.is_host()) {
            if (g_on.acked) { restart_into_match(); return; }
            if (now - g_on.start_sent_ms > 200) {
                std::uint8_t note[6] = {kStartNote};
                std::memcpy(note + 1, &g_on.match, 4);
                note[5] = static_cast<std::uint8_t>(g_on.mode);
                c.send_relay(RoomClient::kControl, note, 6);
                g_on.start_sent_ms = now;
            }
            if (now > g_on.start_deadline || c.members().size() < 2)
                show_message("the other player did not answer", Screen::Room);
        } else {
            std::uint8_t note[5] = {kStartAck};
            std::memcpy(note + 1, &g_on.match, 4);
            for (int i = 0; i < 4; ++i) c.send_relay(RoomClient::kControl, note, 5);
            restart_into_match();
        }
    }
}
#endif  // DUKE_NETPLAY

// ── rows of the full menus ──────────────────────────────────────────────────
enum Item { kSettingRow, kBack,
#if defined(DUKE_NETPLAY)
    kHost, kFind, kCode, kName, kSay, kWorldRow, kLimitRow, kFpsRow, kStartRow, kLeave, kRoomRow,
#endif
};
struct Row { Item item; int index; std::string text; bool enabled = true; bool value = false; };

std::string setting_value(const RecompRuntimeUiItem& it) {
    int v = 0;
    duke_pc_controls_get(it.key, &v);
    switch (it.type) {
    case RECOMP_RUNTIME_UI_BOOL: return v ? "ON" : "OFF";
    case RECOMP_RUNTIME_UI_CHOICE: {
        const int i = v - it.minimum;
        return (it.choices && i >= 0 && static_cast<std::size_t>(i) < it.choice_count) ? upper(it.choices[i]) : std::to_string(v);
    }
    default: return std::to_string(v);
    }
}

std::vector<Row> rows() {
    std::vector<Row> r;
    switch (g_ui.screen) {
    case Screen::Settings: {
        int n = 0;
        const RecompRuntimeUiItem* items = duke_pc_controls_items(&n);
        for (int i = 0; i < n; ++i)
            r.push_back({kSettingRow, i, upper(items[i].label) + ": " + setting_value(items[i]), true, true});
        r.push_back({kBack, 0, "BACK"});
        break;
    }
#if defined(DUKE_NETPLAY)
    case Screen::Online:
        r.push_back({kHost, 0, "HOST A GAME"});
        r.push_back({kFind, 0, "FIND GAMES"});
        r.push_back({kCode, 0, "JOIN BY CODE"});
        r.push_back({kName, 0, "NAME: " + upper(g_name)});
        r.push_back({kSay, 0, "CHAT"});
        r.push_back({kBack, 0, "BACK"});
        break;
    case Screen::List:
        if (g_on.client)
            for (std::size_t i = 0; i < g_on.client->listing().size(); ++i) {
                const auto& l = g_on.client->listing()[i];
                r.push_back({kRoomRow, static_cast<int>(i), upper(l.host) + "  " + std::to_string(l.players) + "/" +
                             std::to_string(l.max_players) + "  " + std::to_string(l.code),
                             l.players < l.max_players});
            }
        r.push_back({kFind, 0, "REFRESH"});
        r.push_back({kBack, 0, "BACK"});
        break;
    case Screen::Room: {
        const bool host = g_on.client && g_on.client->is_host();
        r.push_back({kWorldRow, 0, std::string("WORLD: ") + kWorlds[mode_world(g_on.mode)], host, host});
        r.push_back({kLimitRow, 0, std::string("LIMIT: ") + kLimits[mode_limit(g_on.mode)], host, host});
        r.push_back({kFpsRow, 0, mode_fps60(g_on.mode) ? "FRAME RATE: 60" : "FRAME RATE: GAME", host, host});
        if (host) r.push_back({kStartRow, 0, "START", g_on.client->members().size() >= 2});
        r.push_back({kSay, 0, "CHAT"});
        r.push_back({kLeave, 0, "LEAVE ROOM"});
        break;
    }
#endif
    default: break;
    }
    return r;
}

void close_menu() {
#if defined(DUKE_NETPLAY)
    if (g_on.client) { g_on.client->disconnect(); g_on.client.reset(); }
#endif
    g_ui.screen = Screen::Closed;
    g_ui.draining = true;
}

void change(const Row& row, int dir) {
    if (!row.enabled) return;
    if (row.item == kSettingRow) {
        int n = 0;
        const RecompRuntimeUiItem& it = duke_pc_controls_items(&n)[row.index];
        int v = 0;
        duke_pc_controls_get(it.key, &v);
        const int step = it.step > 0 ? it.step : 1;
        if (it.type == RECOMP_RUNTIME_UI_BOOL) v = !v;
        else {
            v += dir * step;
            if (it.type == RECOMP_RUNTIME_UI_CHOICE) {    // choices wrap round
                if (v > it.maximum) v = it.minimum;
                if (v < it.minimum) v = it.maximum;
            }
            v = std::clamp(v, it.minimum, it.maximum);
        }
        duke_pc_controls_set(it.key, v);
    }
#if defined(DUKE_NETPLAY)
    if ((row.item == kWorldRow || row.item == kLimitRow || row.item == kFpsRow) && g_on.client &&
        g_on.client->is_host()) {
        int w = mode_world(g_on.mode), l = mode_limit(g_on.mode);
        bool fps60 = mode_fps60(g_on.mode);
        if (row.item == kWorldRow) w = (w + dir + kWorldCount) % kWorldCount;
        else if (row.item == kLimitRow) l = (l + dir + kLimitCount) % kLimitCount;
        else fps60 = !fps60;
        g_on.mode = w | l << 4 | (fps60 ? 0x80 : 0);
        send_settings();
    }
#endif
}

void choose(const Row& row) {
    if (!row.enabled) return;
    switch (row.item) {
    case kSettingRow: change(row, 1); break;
    case kBack: close_menu(); break;
#if defined(DUKE_NETPLAY)
    case kWorldRow: case kLimitRow: case kFpsRow: change(row, 1); break;
    case kHost:
        ensure_client(); g_on.want_host = true; g_on.want_join = 0; g_on.mode = 0;
        show_message("opening a room...", Screen::Online);
        break;
    case kFind:
        ensure_client(); g_on.list_wanted = true;
        if (g_on.client->state() == RoomClient::State::Connected) g_on.client->request_list();
        g_ui.screen = Screen::List; g_ui.cursor = 0;
        break;
    case kCode: g_on.edit.clear(); g_ui.screen = Screen::Code; break;
    case kName: g_on.edit = g_name; g_ui.screen = Screen::Name; break;
    case kSay: g_on.edit.clear(); g_on.back_to = g_ui.screen; g_ui.screen = Screen::Chat; break;
    case kRoomRow: {
        const auto& l = g_on.client->listing()[row.index];
        g_on.want_join = l.code; g_on.want_host = false;
        show_message("joining...", Screen::List);
        break;
    }
    case kStartRow: {
        const std::uint32_t r = static_cast<std::uint32_t>(dukeonline::now_ms()) * 2654435761u;
        g_on.match = (r ^ (std::uint32_t(g_on.client->room()) << 16)) | 1u;
        g_on.acked = false;
        for (const auto& m : g_on.client->members()) if (m.id != g_on.client->my_id()) g_on.peer = m.id;
        g_ui.screen = Screen::Starting;
        g_on.start_deadline = dukeonline::now_ms() + 15000;
        g_on.start_sent_ms = 0;
        break;
    }
    case kLeave:
        g_on.client->leave_room();
        g_ui.screen = Screen::Online; g_ui.cursor = 0;
        break;
#endif
    default: break;
    }
}

// ── keys and mouse for the full menus ───────────────────────────────────────
struct Keys { bool up, down, left, right, ok, back, erase, enter; std::string typed; };

bool text_screen() {
#if defined(DUKE_NETPLAY)
    return g_ui.screen == Screen::Name || g_ui.screen == Screen::Code || g_ui.screen == Screen::Chat;
#else
    return false;
#endif
}

// The game's pad (its own key bindings and the WASD layout) plus Enter,
// Escape and text for the name/code/chat lines.
Keys read_keys(std::uint16_t pad_now) {
    Keys k{};
    if (!g_ui.test_key.empty() || !g_ui.test_text.empty()) {
        const std::string key = g_ui.test_key;
        g_ui.test_key.clear();
        k.up = key == "up"; k.down = key == "down"; k.left = key == "left"; k.right = key == "right";
        k.enter = key == "enter"; k.ok = k.enter || key == "ok"; k.back = key == "back"; k.erase = key == "erase";
        k.typed = g_ui.test_text; g_ui.test_text.clear();
        return k;
    }
    const std::uint16_t pressed = static_cast<std::uint16_t>(~pad_now & g_ui.prev_keys & 0x03FF);
    const bool text = text_screen();
    if (!text) {
        k.up = pressed & kUp; k.down = pressed & kDown; k.left = pressed & kLeft; k.right = pressed & kRight;
        k.ok = pressed & (kA | kStart); k.back = pressed & kB;
    }
    if (!SDL_WasInit(SDL_INIT_VIDEO) || !SDL_GetKeyboardFocus()) return k;
    static Uint8 prev[SDL_NUM_SCANCODES] = {};
    int n = 0;
    const Uint8* now = SDL_GetKeyboardState(&n);
    auto hit = [&](SDL_Scancode s) { return now[s] && !prev[s]; };
    k.up |= hit(SDL_SCANCODE_UP) || (!text && hit(SDL_SCANCODE_W));
    k.down |= hit(SDL_SCANCODE_DOWN) || (!text && hit(SDL_SCANCODE_S));
    k.left |= hit(SDL_SCANCODE_LEFT) || (!text && hit(SDL_SCANCODE_A));
    k.right |= hit(SDL_SCANCODE_RIGHT) || (!text && hit(SDL_SCANCODE_D));
    k.enter = hit(SDL_SCANCODE_RETURN) || hit(SDL_SCANCODE_KP_ENTER);
    k.ok |= k.enter || (!text && hit(SDL_SCANCODE_SPACE));
    k.erase = hit(SDL_SCANCODE_BACKSPACE);
    k.back |= (!text && k.erase) || hit(SDL_SCANCODE_DELETE);
    if (text) {
        const bool shift = now[SDL_SCANCODE_LSHIFT] || now[SDL_SCANCODE_RSHIFT];
        for (int s = SDL_SCANCODE_A; s <= SDL_SCANCODE_Z; ++s)
            if (hit(static_cast<SDL_Scancode>(s))) k.typed += static_cast<char>((shift ? 'A' : 'a') + (s - SDL_SCANCODE_A));
        for (int s = SDL_SCANCODE_1; s <= SDL_SCANCODE_0; ++s)
            if (hit(static_cast<SDL_Scancode>(s))) {
                static const char* kShifted = "!@#$%^&*()";
                k.typed += shift ? kShifted[s - SDL_SCANCODE_1] : "1234567890"[s - SDL_SCANCODE_1];
            }
        if (hit(SDL_SCANCODE_SPACE)) k.typed += ' ';
        if (hit(SDL_SCANCODE_MINUS)) k.typed += shift ? '_' : '-';
        if (hit(SDL_SCANCODE_PERIOD)) k.typed += '.';
        if (hit(SDL_SCANCODE_COMMA)) k.typed += ',';
        if (hit(SDL_SCANCODE_SLASH)) k.typed += shift ? '?' : '/';
        if (hit(SDL_SCANCODE_APOSTROPHE)) k.typed += '\'';
    }
    std::memcpy(prev, now, std::min<int>(n, SDL_NUM_SCANCODES));
    return k;
}

struct Mouse { bool moved = false, click = false, right = false; int item = kNoHit, wheel = 0; bool left_half = false; };

Mouse read_mouse() {
    Mouse m;
    int left = 0, right = 0, wheel = 0;
    duke_pc_controls_take_mouse(&left, &right, &wheel);
    m.click = left > 0; m.right = right > 0; m.wheel = std::clamp(wheel, -3, 3);
    static float last_x = -1, last_y = -1;
    float vx = -1, vy = -1;
    if (g_ui.test_mx >= 0) {
        vx = g_ui.test_mx; vy = g_ui.test_my;
    } else if (SDL_WasInit(SDL_INIT_VIDEO) && g_ui.drawable_w > 0) {
        SDL_Window* w = SDL_GetMouseFocus();
        if (w) {
            int mx = 0, my = 0, ww = 0, wh = 0;
            SDL_GetMouseState(&mx, &my);
            SDL_GetWindowSize(w, &ww, &wh);
            if (ww > 0 && wh > 0) {
                vx = (mx + 0.5f) * g_ui.drawable_w / ww / g_ui.scale;
                vy = (my + 0.5f) * g_ui.drawable_h / wh / g_ui.scale;
            }
        }
    }
    m.moved = vx != last_x || vy != last_y;
    last_x = vx; last_y = vy;
    g_ui.mouse_vx = vx; g_ui.mouse_vy = vy;
    for (const Hit& h : g_ui.hits)
        if (vx >= h.x0 && vx < h.x1 && vy >= h.y0 && vy < h.y1) m.item = h.item;
    m.left_half = vx >= 0 && vx < g_ui.vw / 2.0f;
    return m;
}

void handle_input(std::uint16_t pad) {
    const Keys k = read_keys(pad);
    const Mouse mouse = read_mouse();
#if defined(DUKE_NETPLAY)
    if (mouse.click && mouse.item == kHitChat && !text_screen() && g_ui.screen != Screen::Settings) {
        g_on.edit.clear(); g_on.back_to = g_ui.screen; g_ui.screen = Screen::Chat;
        return;
    }
    switch (g_ui.screen) {
    case Screen::Name: case Screen::Code: case Screen::Chat: {
        const std::size_t max = g_ui.screen == Screen::Name ? 15 : g_ui.screen == Screen::Code ? 4 : 120;
        for (char c : k.typed) {
            if (g_ui.screen == Screen::Code && (c < '0' || c > '9')) continue;
            if (g_on.edit.size() < max) g_on.edit += c;
        }
        if (k.erase && !g_on.edit.empty()) g_on.edit.pop_back();
        const Screen back = g_ui.screen == Screen::Chat ? g_on.back_to : Screen::Online;
        if (k.back || mouse.right || (mouse.click && mouse.item == kHitCancel)) { g_ui.screen = back; return; }
        if (k.enter || (mouse.click && mouse.item == kHitOk)) {
            if (g_ui.screen == Screen::Name) {
                std::string n = g_on.edit;
                while (!n.empty() && n.back() == ' ') n.pop_back();
                if (!n.empty()) { g_name = n; save_ini(); }
                if (g_on.client && !g_on.client->room()) { g_on.client->disconnect(); g_on.client.reset(); }
                ensure_client();
                g_ui.screen = Screen::Online;
            } else if (g_ui.screen == Screen::Code) {
                if (g_on.edit.size() == 4) {
                    ensure_client(); g_on.want_join = static_cast<std::uint16_t>(std::atoi(g_on.edit.c_str()));
                    show_message("joining...", Screen::Online);
                }
            } else {
                if (!g_on.edit.empty()) {
                    ensure_client();
                    const bool room = g_on.client->room() != 0;
                    if (g_on.client->state() == RoomClient::State::Connected) g_on.client->say(g_on.edit, room);
                    else g_on.pending_say.emplace_back(g_on.edit, room);
                }
                g_on.edit.clear();
                g_ui.screen = back;
            }
        }
        return;
    }
    case Screen::Message:
        if (k.ok || k.back || mouse.click || mouse.right) g_ui.screen = g_on.back_to;
        return;
    case Screen::Starting:
        return;
    default: break;
    }
#endif
    const auto r = rows();
    if (r.empty()) return;
    const int n = static_cast<int>(r.size());
    const bool on_row = mouse.item >= 0 && mouse.item < n;
    if (on_row && (mouse.moved || mouse.click)) g_ui.cursor = mouse.item;
    if (k.up) g_ui.cursor = (g_ui.cursor + n - 1) % n;
    if (k.down) g_ui.cursor = (g_ui.cursor + 1) % n;
    g_ui.cursor = std::clamp(g_ui.cursor, 0, n - 1);
    const Row& row = r[g_ui.cursor];
    if (mouse.wheel) {
        if (row.value && row.enabled && on_row) change(row, mouse.wheel > 0 ? 1 : -1);
        else g_ui.cursor = std::clamp(g_ui.cursor - mouse.wheel, 0, n - 1);
    }
    if (k.left && row.value) change(row, -1);
    if (k.right && row.value) change(row, 1);
    if (k.ok) choose(row);
    else if (mouse.click && on_row && mouse.item == g_ui.cursor) {
        if (row.value) change(row, mouse.left_half ? -1 : 1);
        else choose(row);
    }
    if (k.back || mouse.right) {
#if defined(DUKE_NETPLAY)
        if (g_ui.screen == Screen::Room) { g_on.client->leave_room(); g_ui.screen = Screen::Online; g_ui.cursor = 0; return; }
        if (g_ui.screen == Screen::List) { g_ui.screen = Screen::Online; g_ui.cursor = 0; return; }
#endif
        close_menu();
    }
}

// ── drawing ─────────────────────────────────────────────────────────────────
void draw_centered(const Pen& pen, float y, const std::string& s, Ink ink, int alpha = 255) {
    draw_text(pen, (g_ui.vw - text_width(s)) / 2.0f, y, s, ink, alpha);
}

// Wraps `s` to lines no wider than `w`.
std::vector<std::string> wrap(const std::string& s, int w) {
    std::vector<std::string> out;
    std::string line, word;
    auto flush_word = [&] {
        if (word.empty()) return;
        const std::string trial = line.empty() ? word : line + " " + word;
        if (text_width(trial) > w && !line.empty()) { out.push_back(line); line = word; }
        else line = trial;
        word.clear();
    };
    for (char c : s) { if (c == ' ') flush_word(); else word += c; }
    flush_word();
    if (!line.empty()) out.push_back(line);
    return out;
}

void draw_full(HostOverlay* ov) {
    const int dw = ov->drawable_width(), dh = ov->drawable_height();
    g_ui.drawable_w = dw; g_ui.drawable_h = dh;
    g_ui.scale = std::max(1, static_cast<int>(std::lround(dh / 190.0)));
    g_ui.vw = dw / g_ui.scale; g_ui.vh = dh / g_ui.scale;
    Pen pen{ov, OverlaySpace::Drawable, static_cast<float>(g_ui.scale), 0, 0};
    ov->fill_rect(0, 0, static_cast<float>(dw), static_cast<float>(dh), 0, {0, 0, 0, 150}, OverlaySpace::Drawable);
    const int pw = std::min(g_ui.vw - 8, 300), px = (g_ui.vw - pw) / 2;
    panel(pen, static_cast<float>(px), 4, static_cast<float>(pw), static_cast<float>(g_ui.vh - 8));
    g_ui.hits.clear();
    const int lh = 13;
    std::string title = "PC SETTINGS";
#if defined(DUKE_NETPLAY)
    if (g_ui.screen != Screen::Settings) title = "ONLINE";
    if (g_ui.screen == Screen::List) title = "FIND GAMES";
    if (g_ui.screen == Screen::Room && g_on.client)
        title = "ROOM " + std::to_string(g_on.client->room()) + (g_on.client->is_host() ? " (HOST)" : "");
#endif
    draw_centered(pen, 10, title, Ink::Title);
    float y = 28;
#if defined(DUKE_NETPLAY)
    if (g_ui.screen == Screen::Room && g_on.client) {
        std::string who;
        for (const auto& m : g_on.client->members()) who += (who.empty() ? "" : ", ") + upper(m.name);
        draw_centered(pen, y, "PLAYERS: " + who, Ink::White); y += lh;
        if (g_on.client->members().size() < 2) { draw_centered(pen, y, "WAITING FOR A SECOND PLAYER", Ink::Dim); y += lh; }
        else if (!g_on.client->is_host()) { draw_centered(pen, y, "WAITING FOR THE HOST TO START", Ink::Dim); y += lh; }
        else { draw_centered(pen, y, "START: BOTH GAMES GO TO THE MULTIPLAYER SETUP", Ink::Dim); y += lh; }
        y += 4;
    }
    switch (g_ui.screen) {
    case Screen::Name: case Screen::Code: case Screen::Chat: {
        const char* prompt = g_ui.screen == Screen::Name ? "YOUR NAME" : g_ui.screen == Screen::Code ? "ROOM CODE" : "SAY";
        draw_centered(pen, y + 8, prompt, Ink::Normal);
        const bool blink = (SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetTicks() / 400 : 0) & 1;
        draw_centered(pen, y + 26, upper(g_on.edit) + (blink ? "_" : " "), Ink::White);
        const float by = y + 50, gap = 24;
        const std::string ok = "OK", cancel = "CANCEL";
        const float okw = text_width(ok), cw = text_width(cancel);
        const float ox = (g_ui.vw - okw - gap - cw) / 2, cx = ox + okw + gap;
        const bool over_ok = g_ui.mouse_vy >= by - 3 && g_ui.mouse_vy < by + lh && g_ui.mouse_vx >= ox - 6 && g_ui.mouse_vx < ox + okw + 6;
        const bool over_cancel = g_ui.mouse_vy >= by - 3 && g_ui.mouse_vy < by + lh && g_ui.mouse_vx >= cx - 6 && g_ui.mouse_vx < cx + cw + 6;
        draw_text(pen, ox, by, ok, over_ok ? Ink::Selected : Ink::Normal);
        draw_text(pen, cx, by, cancel, over_cancel ? Ink::Selected : Ink::Normal);
        g_ui.hits.push_back({ox - 6, by - 3, ox + okw + 6, by + lh, kHitOk});
        g_ui.hits.push_back({cx - 6, by - 3, cx + cw + 6, by + lh, kHitCancel});
        draw_centered(pen, by + lh + 8, "ENTER: OK   DELETE OR RIGHT CLICK: CANCEL", Ink::Dim);
        return;
    }
    case Screen::Message:
        draw_centered(pen, g_ui.vh / 2.0f - 12, g_on.message, Ink::White);
        draw_centered(pen, g_ui.vh / 2.0f + 6, "CLICK OR PRESS ENTER", Ink::Dim);
        return;
    case Screen::Starting:
        draw_centered(pen, g_ui.vh / 2.0f - 6, "STARTING THE MATCH...", Ink::White);
        return;
    default: break;
    }
    const bool chat = g_ui.screen != Screen::Settings;
#else
    const bool chat = false;
#endif
    const auto r = rows();
    const int chat_lines = chat ? 3 : 0;
    const float help_h = g_ui.screen == Screen::Settings ? 3 * 11 + 6 : 0;
    const float bottom = g_ui.vh - 8 - chat_lines * 11 - help_h - (chat ? 6 : 0);
    const int visible = std::max(1, static_cast<int>((bottom - y) / lh));
    if (g_ui.cursor < g_ui.scroll) g_ui.scroll = g_ui.cursor;
    if (g_ui.cursor >= g_ui.scroll + visible) g_ui.scroll = g_ui.cursor - visible + 1;
    g_ui.scroll = std::clamp(g_ui.scroll, 0, std::max(0, static_cast<int>(r.size()) - visible));
#if defined(DUKE_NETPLAY)
    if (g_ui.screen == Screen::List && r.size() <= 2) {
        draw_centered(pen, y, (g_on.client && g_on.client->state() == RoomClient::State::Connected)
                                  ? "NO OPEN ROOMS" : "CONNECTING...", Ink::Dim);
        y += lh;
    }
#endif
    for (int i = g_ui.scroll; i < static_cast<int>(r.size()) && i < g_ui.scroll + visible; ++i) {
        const bool sel = i == g_ui.cursor;
        std::string text = r[i].text;
        if (sel && r[i].value && r[i].enabled) text = "< " + text + " >";
        draw_centered(pen, y, text, !r[i].enabled ? Ink::Dim : sel ? Ink::Selected : Ink::Normal);
        g_ui.hits.push_back({static_cast<float>(px), y - 1, static_cast<float>(px + pw), y + lh - 1, i});
        y += lh;
    }
    if (g_ui.scroll > 0) draw_text(pen, px + pw - 14.0f, 28, "^", Ink::Dim);
    if (g_ui.scroll + visible < static_cast<int>(r.size())) draw_text(pen, px + pw - 14.0f, y - lh, "v", Ink::Dim);
    if (g_ui.screen == Screen::Settings && g_ui.cursor < static_cast<int>(r.size()) && r[g_ui.cursor].item == kSettingRow) {
        int n = 0;
        const RecompRuntimeUiItem& it = duke_pc_controls_items(&n)[r[g_ui.cursor].index];
        float hy = g_ui.vh - 8 - help_h + 4;
        int lines = 0;
        for (const std::string& l : wrap(upper(it.description ? it.description : ""), pw - 16)) {
            if (++lines > 3) break;
            draw_text(pen, px + 8.0f, hy, l, Ink::Dim);
            hy += 11;
        }
    }
#if defined(DUKE_NETPLAY)
    if (chat) {
        const float top = g_ui.vh - 8 - chat_lines * 11 - 4;
        const bool over = g_ui.mouse_vy >= top && g_ui.mouse_vy < g_ui.vh - 4;
        g_ui.hits.push_back({static_cast<float>(px), top, static_cast<float>(px + pw), g_ui.vh - 4.0f, kHitChat});
        ov->fill_rect((px + 4.0f) * pen.px, top * pen.px, (pw - 8.0f) * pen.px, (chat_lines * 11 + 4.0f) * pen.px, 0,
                      {0, 0, 0, static_cast<std::uint8_t>(over ? 150 : 90)}, OverlaySpace::Drawable);
        if (!g_on.client || g_on.client->chat().empty())
            draw_text(pen, px + 8.0f, g_ui.vh - 8 - 11.0f, over ? "CLICK HERE TO CHAT" : "CHAT", Ink::Dim);
        if (g_on.client) {
            const auto& lines = g_on.client->chat();
            float cy = top + 3;
            for (std::size_t i = lines.size() > static_cast<std::size_t>(chat_lines) ? lines.size() - chat_lines : 0;
                 i < lines.size(); ++i, cy += 11) {
                std::string line = (lines[i].from.empty() ? "" : upper(lines[i].from) + ": ") + upper(lines[i].text);
                while (!line.empty() && text_width(line) > pw - 16) line.pop_back();
                draw_text(pen, px + 8.0f, cy, line, lines[i].room ? Ink::White : Ink::Normal);
            }
        }
    }
    if (g_on.client) {
        const std::string st = g_on.client->state() == RoomClient::State::Connected ? "ONLINE"
                             : g_on.client->state() == RoomClient::State::Connecting ? "CONNECTING" : "";
        if (!st.empty()) draw_text(pen, px + pw - text_width(st) - 8.0f, 10, st, Ink::Dim);
    }
#endif
}

// The added row on a game menu, in GBA pixels over its picture.
void draw_entry(HostOverlay* ov) {
    Pen pen{ov, OverlaySpace::View, 1.0f, 0, 0};
    const bool focus = g_ui.focus == g_ui.entry;
    const std::uint32_t t = ov->host_ms();
    const auto pulse = static_cast<std::uint8_t>(150 + 100 * (0.5 + 0.5 * std::sin(t / 160.0)));
    if (g_ui.entry == Entry::PcSettings) {
        const std::string s = "PC SETTINGS";
        if (focus) {
            // The game still shows GAMMA as selected: dim it.
            ov->fill_rect(kGammaRow.x0, kGammaRow.y0, kGammaRow.x1 - kGammaRow.x0, kGammaRow.y1 - kGammaRow.y0, 0,
                          {0, 0, 0, 120}, OverlaySpace::View);
        }
        draw_text(pen, (240 - text_width(s)) / 2.0f, kPcSettingsRow.y0 + 1.0f, s, focus ? Ink::Selected : Ink::Normal);
    } else if (g_ui.entry == Entry::Online && focus) {
        ov->fill_rect(kOptionsItem.x0, kOptionsItem.y0, kOptionsItem.x1 - kOptionsItem.x0,
                      kOptionsItem.y1 - kOptionsItem.y0, 0, {0, 0, 0, 170}, OverlaySpace::View);
        ov->stroke_rect(kMultiplayerItem.x0, kMultiplayerItem.y0, kMultiplayerItem.x1 - kMultiplayerItem.x0,
                        kMultiplayerItem.y1 - kMultiplayerItem.y0, 1, 1, {255, 210, 60, pulse}, OverlaySpace::View);
    }
}

void host_overlay(HostOverlay* ov) {
    if (is_open()) draw_full(ov);
    else if (g_ui.entry != Entry::None) draw_entry(ov);
}

// ── input, once per game frame ──────────────────────────────────────────────
bool all_released(gba::GbaBus* bus) {
    if (bus && (bus->io().host_keyinput() & 0x03FF) != 0x03FF) return false;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return true;
    if (SDL_GetMouseState(nullptr, nullptr) & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK)) return false;
    int n = 0;
    const Uint8* k = SDL_GetKeyboardState(&n);
    for (int i = 0; i < n; ++i) if (k[i]) return false;
    return true;
}

bool g_netplay_build = false;

bool overlay_input(const gbarecomp::TouchFrameInfo* info, std::uint16_t* keys) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    const std::uint16_t pad = bus ? static_cast<std::uint16_t>(bus->io().host_keyinput() & 0x03FF) : 0x03FF;
    struct PrevKeys { std::uint16_t now; ~PrevKeys() { g_ui.prev_keys = now; } } remember{pad};
    std::uint16_t pressed = static_cast<std::uint16_t>(~pad & g_ui.prev_keys & 0x03FF);
    {
        // The WASD layout's W / S / Space reach the game through
        // pc_controls, not the pad: count them as Up / Down / A here.
        static bool w_prev = false, s_prev = false, sp_prev = false;
        int wasd = 0;
        duke_pc_controls_get("duke.wasd", &wasd);
        const Uint8* k = (wasd && SDL_WasInit(SDL_INIT_VIDEO) && SDL_GetKeyboardFocus()) ? SDL_GetKeyboardState(nullptr) : nullptr;
        const bool w = k && k[SDL_SCANCODE_W], s = k && k[SDL_SCANCODE_S], sp = k && k[SDL_SCANCODE_SPACE];
        if (w && !w_prev) pressed |= kUp;
        if (s && !s_prev) pressed |= kDown;
        if (sp && !sp_prev) pressed |= kA;
        w_prev = w; s_prev = s; sp_prev = sp;
    }
#if defined(DUKE_NETPLAY)
    pump_network();
#endif
    if (is_open()) {
        if (bus) bus->io().set_host_keys_blocked(true);
        if (SDL_WasInit(SDL_INIT_VIDEO)) SDL_ShowCursor(SDL_ENABLE);
        if (g_ui.settling) {
            int l, r, w; duke_pc_controls_take_mouse(&l, &r, &w);
            if (!SDL_WasInit(SDL_INIT_VIDEO) || all_released(bus)) g_ui.settling = false;
            if (g_ui.test_key.empty() && g_ui.test_text.empty()) read_keys(pad);   // brings key states up to date
            return true;
        }
        handle_input(pad);
        if (!is_open()) g_ui.draining = true;
        *keys = 0x03FF;
        return true;
    }
    if (g_ui.draining) {
        if (bus) bus->io().set_host_keys_blocked(true);
        int l, r, w; duke_pc_controls_take_mouse(&l, &r, &w);
        if (all_released(bus)) {
            g_ui.draining = false;
            if (bus) bus->io().set_host_keys_blocked(false);
        }
        *keys = 0x03FF;
        return true;
    }

    // An added row on one of the game's menus?
    std::uint32_t menu = 0;
    int sel = -1;
    g_ui.entry = Entry::None;
    if (!duke_pc_controls_in_gameplay() && duke_pc_controls_menu(&menu, &sel)) {
        if (menu == kOptionsMenu) g_ui.entry = Entry::PcSettings;
        if (menu == kMainMenu && g_netplay_build) g_ui.entry = Entry::Online;
    }
    if (g_ui.entry == Entry::None) {
        g_ui.focus = Entry::None;
        if (bus && bus->io().host_keys_blocked()) bus->io().set_host_keys_blocked(false);
        return false;
    }
    const int anchor = g_ui.entry == Entry::PcSettings ? kOptionsGamma : kMainOptions;
    const Box& box = g_ui.entry == Entry::PcSettings ? kPcSettingsRow : kMultiplayerItem;
    float px = -1, py = -1;
    const bool pointer = duke_pc_controls_pointer(info, &px, &py);
    static float last_px = -1, last_py = -1;
    const bool moved = pointer && (std::fabs(px - last_px) >= 0.5f || std::fabs(py - last_py) >= 0.5f);
    if (pointer) { last_px = px; last_py = py; }
    const bool over = pointer && box.has(px, py);
    int left = 0, right = 0, wheel = 0;
    duke_pc_controls_peek_mouse(&left, &right, &wheel);
    auto open = [&] {
        int l, r, w; duke_pc_controls_take_mouse(&l, &r, &w);
#if defined(DUKE_NETPLAY)
        open_screen(g_ui.entry == Entry::PcSettings ? Screen::Settings : Screen::Online);
        if (g_ui.screen == Screen::Online) ensure_client();
#else
        open_screen(Screen::Settings);
#endif
        if (bus) bus->io().set_host_keys_blocked(true);
        *keys = 0x03FF;
        return true;
    };
    if (left > 0 && over) return open();
    if (moved && over) g_ui.focus = g_ui.entry;
    if (g_ui.focus == g_ui.entry) {
        if (sel != anchor || (moved && !over)) {
            g_ui.focus = Entry::None;          // the game's selection moved on (mouse)
        } else {
            if (bus) bus->io().set_host_keys_blocked(true);
            *keys = 0x03FF;
            if (pressed & (kA | kStart)) return open();
            if (pressed & (kUp | kB) || wheel > 0 || right > 0) {
                int l, r, w; duke_pc_controls_take_mouse(&l, &r, &w);
                g_ui.focus = Entry::None;
                g_ui.draining = true;          // the Up stays away from the game
            }
            return true;
        }
    }
    // The game's selection is on the item above our row: Down (or the wheel)
    // moves onto ours instead of wrapping round the game's list.
    if (sel == anchor) {
        if (bus) bus->io().set_host_keys_blocked(true);
        *keys = static_cast<std::uint16_t>(pad | kDown);
        if ((pressed & kDown) || wheel < 0) {
            if (wheel < 0) { int l, r, w; duke_pc_controls_take_mouse(&l, &r, &w); }
            g_ui.focus = g_ui.entry;
            *keys = 0x03FF;
            return true;
        }
        return false;
    }
    if (bus && bus->io().host_keys_blocked()) bus->io().set_host_keys_blocked(false);
    return false;
}

// Headless tests: draws the overlay into an image (rectangles only), to be
// laid over the game's screenshot. View space is 4x, the full menus 1920x1080.
class SoftOverlay final : public HostOverlay {
public:
    int w, h, view_scale;
    std::vector<std::uint32_t> px;
    SoftOverlay(int w_, int h_, int vs) : w(w_), h(h_), view_scale(vs), px(static_cast<std::size_t>(w_) * h_, 0) {}
    int drawable_width() const override { return w; }
    int drawable_height() const override { return h; }
    gbarecomp::PresentationLayout game_rect() const override { return {}; }
    int view_width() const override { return 240; }
    int view_height() const override { return 160; }
    float drawable_px_per_mm() const override { return 4; }
    Insets safe_insets() const override { return {}; }
    std::uint32_t host_ms() const override { return 0; }
    void to_drawable(float x, float y, OverlaySpace s, float* dx, float* dy) const override {
        const float k = s == OverlaySpace::View ? static_cast<float>(view_scale) : 1.0f;
        *dx = x * k; *dy = y * k;
    }
    void line(float, float, float, float, float, OverlayColor, OverlaySpace) override {}
    void polyline(const gbarecomp::OverlayPoint*, std::size_t, float, OverlayColor, OverlaySpace) override {}
    void fill_rect(float x, float y, float rw, float rh, float, OverlayColor c, OverlaySpace s) override {
        float x0, y0, x1, y1;
        to_drawable(x, y, s, &x0, &y0); to_drawable(x + rw, y + rh, s, &x1, &y1);
        for (int yy = std::max(0, static_cast<int>(y0)); yy < std::min(h, static_cast<int>(y1)); ++yy)
            for (int xx = std::max(0, static_cast<int>(x0)); xx < std::min(w, static_cast<int>(x1)); ++xx) {
                std::uint32_t& d = px[static_cast<std::size_t>(yy) * w + xx];
                const unsigned a = c.a, da = d >> 24;
                auto mixc = [&](int sh, unsigned sc) { return (((d >> sh) & 0xFF) * (255 - a) + sc * a) / 255; };
                d = (std::min(255u, a + da * (255 - a) / 255) << 24) | (mixc(16, c.r) << 16) | (mixc(8, c.g) << 8) | mixc(0, c.b);
            }
    }
    void stroke_rect(float x, float y, float rw, float rh, float, float t, OverlayColor c, OverlaySpace s) override {
        fill_rect(x, y, rw, t, 0, c, s); fill_rect(x, y + rh - t, rw, t, 0, c, s);
        fill_rect(x, y, t, rh, 0, c, s); fill_rect(x + rw - t, y, t, rh, 0, c, s);
    }
    void fill_circle(float, float, float, OverlayColor, OverlaySpace) override {}
    void stroke_circle(float, float, float, float, OverlayColor, OverlaySpace) override {}
    void arc(float, float, float, float, float, float, OverlayColor, OverlaySpace) override {}
    bool text_supported() const override { return false; }
    void text(float, float, float, OverlayColor, const char*, gbarecomp::OverlayAlign, OverlaySpace) override {}
    float text_width(const char*, float, OverlaySpace) const override { return 0; }
};

bool render_to(const std::string& path) {
    const bool full = is_open();
    SoftOverlay ov(full ? 1920 : 960, full ? 1080 : 640, 4);
    host_overlay(&ov);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", ov.w, ov.h);
    for (std::uint32_t p : ov.px) {
        const unsigned char b[4] = {static_cast<unsigned char>(p >> 16), static_cast<unsigned char>(p >> 8),
                                    static_cast<unsigned char>(p), static_cast<unsigned char>(p >> 24)};
        std::fwrite(b, 1, 4, f);
    }
    std::fclose(f);
    return true;
}

}  // namespace

void duke_menu_install(gbarecomp::RunOptions* opts, int argc, char** argv, const std::string& build_tag) {
    opts->host_overlay = host_overlay;
    duke_pc_controls_set_overlay_input(overlay_input);
#if defined(DUKE_NETPLAY)
    g_netplay_build = true;
    g_build = build_tag;
    std::error_code ec;
    const auto exe = std::filesystem::absolute(std::filesystem::path(argc > 0 ? argv[0] : "."), ec);
    g_exe = exe.string();
    g_ini = exe.parent_path() / "duke_online.ini";
    load_ini();
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--save-path")) g_save_path = argv[i + 1];
#else
    (void)argc; (void)argv; (void)build_tag;
#endif
}

int duke_menu_tcp_command(const char* request, void (*write)(void*, const char*, std::size_t), void* ctx) {
    if (!std::strstr(request, "\"duke_menu\"")) return 0;
    auto text = [&](const char* key) -> std::string {
        const char* at = std::strstr(request, key);
        if (!at) return "";
        at = std::strchr(at + std::strlen(key), ':');
        if (!at) return "";
        at = std::strchr(at, '"');
        if (!at) return "";
        const char* end = std::strchr(at + 1, '"');
        return end ? std::string(at + 1, end) : "";
    };
    auto number = [&](const char* key, float fallback) {
        const char* at = std::strstr(request, key);
        at = at ? std::strchr(at + std::strlen(key), ':') : nullptr;
        return at ? static_cast<float>(std::atof(at + 1)) : fallback;
    };
    const std::string open = text("\"open\""), key = text("\"key\""), typed = text("\"text\"");
    if (open == "settings") open_screen(Screen::Settings);
#if defined(DUKE_NETPLAY)
    if (open == "online") { open_screen(Screen::Online); ensure_client(); }
#endif
    if (open == "close") close_menu();
    if (!key.empty()) g_ui.test_key = key;
    if (!typed.empty()) g_ui.test_text = typed;
    if (std::strstr(request, "\"mx\"")) { g_ui.test_mx = number("\"mx\"", -1); g_ui.test_my = number("\"my\"", -1); }
    // Headless runs never draw: lay the full menu out on a 1920x1080 window.
    if (!SDL_WasInit(SDL_INIT_VIDEO)) { g_ui.drawable_w = 1920; g_ui.drawable_h = 1080; g_ui.scale = 6; g_ui.vw = 320; g_ui.vh = 180; }
    const std::string render = text("\"render\"");
    if (!render.empty()) render_to(render);
    const auto r = rows();
    std::string reply = "{\"ok\":true,\"screen\":" + std::to_string(static_cast<int>(g_ui.screen)) +
        ",\"cursor\":" + std::to_string(g_ui.cursor) + ",\"focus\":" + std::to_string(static_cast<int>(g_ui.focus)) +
        ",\"entry\":" + std::to_string(static_cast<int>(g_ui.entry)) +
#if defined(DUKE_NETPLAY)
        ",\"room\":" + std::to_string(g_on.client ? g_on.client->room() : 0) +
        ",\"members\":" + std::to_string(g_on.client ? g_on.client->members().size() : 0) +
#endif
        ",\"rows\":[";
    for (std::size_t i = 0; i < r.size(); ++i) reply += (i ? ",\"" : "\"") + r[i].text + "\"";
    reply += "]}";
    write(ctx, reply.data(), reply.size());
    return 1;
}

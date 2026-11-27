// The host without the game: the manifest loader, the page thread, the bridge and the input routing, driven by raw
// key and mouse calls as the hook makes them. Writes the pages composited on a 1920x1080 screen to a BMP for a look.
//   view_test.exe <stage root> <out.bmp>     (stage root holds red4ext\plugins\MagelightCP and r6\magelight)
#include "../src/Api.h"
#include "../src/Bridge.h"
#include "../src/Input.h"
#include "../src/Log.h"
#include "../src/Manifest.h"
#include "../src/Views.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace mlcp;
using namespace std::chrono_literals;

namespace {
int g_failed = 0;
std::vector<views::Message> g_heard;
int g_otherHeard = 0, g_reds = 0, g_all = 0;

void Heard(const char* mod, const char* view, const char* channel, const char* payload, void*) {
    g_heard.push_back({mod, view, channel, payload});
}
void HeardOther(const char*, const char*, const char*, const char*, void*) { ++g_otherHeard; }
void HeardAll(const char*, const char*, const char*, const char*, void*) { ++g_all; }
bool FakeReds(const views::Message&) { return ++g_reds, true; }

void Check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failed;
}

void Key(unsigned short vk, bool up) {
    input::OnKey(vk, static_cast<unsigned short>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)), false, up);
}

void Press(unsigned short vk) {
    Key(vk, false);
    Key(vk, true);
    std::this_thread::sleep_for(60ms);
}

void Type(const char* text) {
    for (const char* c = text; *c; ++c) {
        const SHORT scan = VkKeyScanW(static_cast<wchar_t>(*c));
        const bool shift = (scan >> 8) & 1;
        if (shift) Key(VK_SHIFT, false);
        Press(static_cast<unsigned short>(scan & 0xff));
        if (shift) Key(VK_SHIFT, true);
    }
}

void Click(int dx, int dy) {
    input::OnMouse(dx, dy, 0, 0);
    input::OnMouse(0, 0, RI_MOUSE_LEFT_BUTTON_DOWN, 0);
    input::OnMouse(0, 0, RI_MOUSE_LEFT_BUTTON_UP, 0);
    std::this_thread::sleep_for(80ms);
}

// Moves the cursor to backbuffer x, y (at 1080p a count is a pixel) and clicks there.
void ClickAt(int x, int y) {
    const auto c = input::CursorNow();
    Click(x - c.x, y - c.y);
}

void Chord(unsigned short modifier, unsigned short vk) {
    Key(modifier, false);
    Press(vk);
    Key(modifier, true);
}

// The text field's value in the newest demo.click a listener heard; "?" when none.
std::string LastClickText() {
    for (auto it = g_heard.rbegin(); it != g_heard.rend(); ++it) {
        if (it->channel != "demo.click") continue;
        const auto at = it->payload.find("\"text\":\"");
        if (at == std::string::npos) return "?";
        const auto start = at + 8;
        return it->payload.substr(start, it->payload.find('"', start) - start);
    }
    return "?";
}

int Clicks() {
    int n = 0;
    for (const auto& m : g_heard) n += m.channel == "demo.click";
    return n;
}

bool LogHas(const std::filesystem::path& log, const char* text) {
    FILE* f = _wfopen(log.c_str(), L"rb");
    if (!f) return false;
    std::string all;
    char buf[4096];
    for (std::size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) all.append(buf, n);
    fclose(f);
    return all.find(text) != std::string::npos;
}

// A second mod beside the demo: a panel bound to F9, whose page throws once and logs a line no player should see.
std::filesystem::path MakeModsRoot(const std::filesystem::path& stageMods) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / L"mlcp_view_test_mods";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / L"MlcpTest" / L"views" / L"other");
    fs::copy(stageMods, root, fs::copy_options::recursive, ec);
    const auto write = [](const fs::path& p, const char* text) {
        if (FILE* f = _wfopen(p.c_str(), L"wb")) fputs(text, f), fclose(f);
    };
    write(root / L"MlcpTest" / L"manifest.json",
          R"({"modId":"MlcpTest","views":{"other":{"path":"views/other/index.html","layer":"panel",)"
          R"("x":600,"y":40,"w":200,"h":100,"hotkey":"F9"}}})");
    write(root / L"MlcpTest" / L"views" / L"other" / L"index.html",
          "<!doctype html><body>other<script>console.log('ml-secret-log');"
          "setTimeout(function(){throw new Error('ml-test-throw');},0);</script></body>");
    return root;
}

const views::Row* Named(const std::vector<views::Row>& rows, const char* name) {
    for (const auto& r : rows)
        if (r.name == name) return &r;
    return nullptr;
}

// Premultiplied BGRA pages over a dark grey, in layout order.
bool WriteScreen(const wchar_t* path, const std::vector<views::Row>& rows, const std::map<int, views::Pixels>& frames,
                 int W, int H) {
    std::vector<std::uint8_t> px(std::size_t(W) * H * 4);
    for (std::size_t i = 0; i < px.size(); i += 4) px[i] = px[i + 1] = px[i + 2] = 40, px[i + 3] = 255;
    for (const auto& r : rows) {
        const auto it = frames.find(r.id);
        if (!r.visible || it == frames.end()) continue;
        const auto& f = it->second;
        for (UINT y = 0; y < f.height; ++y) {
            if (r.y + int(y) < 0 || r.y + int(y) >= H) continue;
            for (UINT x = 0; x < f.width; ++x) {
                if (r.x + int(x) < 0 || r.x + int(x) >= W) continue;
                const std::uint8_t* s = f.data + std::size_t(y) * f.rowBytes + x * 4;
                std::uint8_t* d = px.data() + (std::size_t(r.y + y) * W + r.x + x) * 4;
                for (int c = 0; c < 3; ++c) d[c] = std::uint8_t(s[c] + d[c] * (255 - s[3]) / 255);
            }
        }
    }
    FILE* out = _wfopen(path, L"wb");
    if (!out) return false;
    BITMAPFILEHEADER fh{0x4d42, DWORD(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + px.size()), 0, 0,
                        sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
    BITMAPINFOHEADER ih{sizeof ih, LONG(W), -LONG(H), 1, 32, BI_RGB};
    fwrite(&fh, sizeof fh, 1, out);
    fwrite(&ih, sizeof ih, 1, out);
    fwrite(px.data(), 1, px.size(), out);
    fclose(out);
    return true;
}

// Keeps the newest frame of every view; TakeFrames hands over only what changed.
struct Frames {
    std::map<int, std::vector<std::uint8_t>> buffers, kept;
    std::map<int, views::Pixels> changed, latest;
    void Pump() {
        changed.clear();
        views::TakeFrames(buffers, changed);
        for (auto& [id, px] : changed) {
            kept[id] = buffers[id];
            latest[id] = px;
            latest[id].data = kept[id].data();
        }
    }
};
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        fwprintf(stderr, L"usage: view_test <stage root> <out.bmp>\n");
        return 2;
    }
    const std::filesystem::path stage = argv[1];
    const auto plugin = stage / "red4ext" / "plugins" / "MagelightCP";
    const auto mods = MakeModsRoot(stage / "r6" / "magelight");
    const auto log = plugin / L"view_test.log";
    LogOpen(log);
    for (const wchar_t* name : {L"Magelight1Core.dll", L"MgWCore.dll", L"Magelight1.dll"})
        if (!LoadLibraryExW((plugin / name).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return 3;

    auto specs = manifest::LoadAll(mods);
    Check(specs.size() == 3, "the demo and the test mod give three views");
    views::SetScreen(1920, 1080);
    input::SetScreen(1920, 1080);
    views::Start(plugin, mods, std::move(specs));
    Frames frames;
    for (int i = 0; i < 75; ++i, std::this_thread::sleep_for(20ms)) frames.Pump();

    auto rows = views::Layout();
    const views::Row* hud = Named(rows, "hud");
    const views::Row* panel = Named(rows, "panel");
    Check(hud && panel, "both views are laid out");
    if (!hud || !panel) return 1;
    const int hudId = hud->id, panelId = panel->id;
    Check(hud->visible && !panel->visible, "hud starts shown, panel hidden");
    Check(hud->x == 1920 - 24 - 260 && hud->y == 24, "hud anchored top-right at 24,24");
    Check(hud->layer < panel->layer && rows.front().id == hudId, "hud draws below panel");

    Check(!input::OnMouse(5, 5, 0, 0), "the mouse is the game's while no view holds UI mode");
    Press(VK_F11);
    Check(input::UiModeView() == panelId, "F11 gives the panel UI mode");
    std::this_thread::sleep_for(200ms);
    Check(Named(views::Layout(), "panel")->visible, "the panel is shown");

    // The cursor starts at the panel's centre (280,160); CLICK is at page 330,116, screen 370,156.
    Click(90, -4);
    Click(0, 0);
    Click(0, 0);
    Click(-180, 2);   // the text box, page 150,118
    Type("Hello night city");
    Check(api::Send("magelightdemo", "panel", "demo.echo", "hello from the host"),
          "Magelight_Send finds the view, the mod id in any case");
    for (int i = 0; i < 40; ++i, std::this_thread::sleep_for(20ms)) frames.Pump();

    // Delivery: held while a game loads, then to every kind of listener on the game thread's pump.
    api::AddListener("MagelightDemo", &Heard, nullptr);
    api::AddListener("SomeOtherMod", &HeardOther, nullptr);
    api::AddListener("", &HeardAll, nullptr);   // every mod's, the test mod's '__ready' included
    api::SetRedsFire(&FakeReds);
    api::NextForCet();   // the CET door has asked once: its queue is on
    api::Pump(false);
    Check(g_heard.empty() && g_reds == 0, "nothing is delivered while a game loads");
    Check(bridge::Waiting(), "and the messages wait");
    api::Pump(true);
    int clicks = 0, ready = 0, uiOn = 0;
    for (const auto& m : g_heard) {
        std::printf("      heard %s/%s '%s' %s\n", m.mod.c_str(), m.view.c_str(), m.channel.c_str(), m.payload.c_str());
        clicks += m.channel == "demo.click" && m.view == "panel";
        ready += m.channel == "__ready";
        uiOn += m.channel == "__uimode" && m.payload == "1";
    }
    Check(clicks == 3, "three clicks reached a C++ listener through magelight.send");
    Check(ready == 2, "both pages told '__ready'");
    Check(uiOn == 1, "F11 told '__uimode' 1");
    Check(g_all > int(g_heard.size()) && g_reds == g_all, "redscript got every mod's messages");
    Check(g_otherHeard == 0, "another mod's listener heard none of them");
    int cet = 0;
    for (std::string raw; !(raw = api::NextForCet()).empty(); ++cet)
        if (cet == 0) std::printf("      cet  %s\n", raw.c_str());
    Check(cet == g_all, "the CET door's queue got them too");

    auto shot = views::Layout();
    Check(WriteScreen(argv[2], shot, frames.latest, 1920, 1080), "screen written");

    Press(VK_F10);
    std::this_thread::sleep_for(200ms);
    Check(!Named(views::Layout(), "hud")->visible, "F10 hides the hud");
    Check(input::UiModeView() == panelId, "the hud's key leaves UI mode alone");
    Check(api::UIModeView() == "MagelightDemo/panel", "Magelight_UIModeView names the panel");
    Press(VK_ESCAPE);
    std::this_thread::sleep_for(200ms);
    Check(input::UiModeView() == 0 && !Named(views::Layout(), "panel")->visible, "Escape closes the panel");
    Check(!input::OnMouse(5, 5, 0, 0), "and gives the mouse back to the game");
    g_heard.clear();
    api::Pump(true);
    Check(g_heard.size() == 1 && g_heard[0].channel == "__uimode" && g_heard[0].payload == "0",
          "Escape told '__uimode' 0");
    g_heard.clear();

    // An up goes where its down went: nothing stays held in the game across UI mode.
    Check(!input::OnKey('W', static_cast<unsigned short>(MapVirtualKeyW('W', MAPVK_VK_TO_VSC)), false, false),
          "W held before UI mode goes to the game");
    Check(!input::OnMouse(0, 0, RI_MOUSE_RIGHT_BUTTON_DOWN, 0), "and so does a held right button");
    Press(VK_F11);
    Check(input::UiModeView() == panelId, "F11 opens the panel over them");
    Check(!input::OnKey('W', static_cast<unsigned short>(MapVirtualKeyW('W', MAPVK_VK_TO_VSC)), false, true),
          "W's release still reaches the game");
    Check(!input::OnMouse(0, 0, RI_MOUSE_RIGHT_BUTTON_UP, 0), "and the right button's");
    Key(VK_ESCAPE, false);
    Check(input::UiModeView() == 0, "Escape's press closes UI mode");
    Check(input::OnKey(VK_ESCAPE, static_cast<unsigned short>(MapVirtualKeyW(VK_ESCAPE, MAPVK_VK_TO_VSC)), false, true),
          "and its release is kept from the game");
    std::this_thread::sleep_for(200ms);

    // A HUD key pressed twice before the page thread runs ends where it began.
    const bool hudWas = Named(views::Layout(), "hud")->visible;
    Key(VK_F10, false), Key(VK_F10, true), Key(VK_F10, false), Key(VK_F10, true);
    Check(Named(views::Layout(), "hud")->visible == hudWas, "a quick double press of the HUD's key toggles twice");

    // Below 1080p a slow mouse still moves: the part below a pixel is kept.
    input::SetScreen(1280, 720);
    Check(api::OpenUIMode("MagelightDemo", "panel"), "UI mode at 720p");
    const int x0 = input::CursorNow().x;
    for (int i = 0; i < 3; ++i) input::OnMouse(1, 0, 0, 0);
    Check(input::CursorNow().x == x0 + 2, "three one-count moves at 720p move two pixels");
    api::CloseUIMode();
    input::SetScreen(1920, 1080);
    Check(!api::Send("MagelightDemo", "panel", "demo.echo", "bad \xff text"), "invalid UTF-8 is refused");
    std::this_thread::sleep_for(200ms);
    api::Pump(true);
    g_heard.clear();

    // Keys reach the page with their modifiers: Ctrl+A selects the field, so typing replaces what was there.
    // The field is at page 150,118 (screen 190,158), CLICK at page 330,116 (screen 370,156).
    const auto settle = [&] {
        for (int i = 0; i < 15; ++i, std::this_thread::sleep_for(20ms)) frames.Pump();
        api::Pump(true);
    };
    Check(api::OpenUIMode("MagelightDemo", "panel"), "the panel opens again");
    std::this_thread::sleep_for(200ms);
    ClickAt(190, 158);
    Chord(VK_CONTROL, 'A');
    Type("x");
    ClickAt(370, 156);
    settle();
    Check(LastClickText() == "x", "Ctrl+A then x leaves 'x' in the field");

    // Shift+Left selects: an arrow is an E0 key, as raw input gives it.
    ClickAt(190, 158);
    Chord(VK_CONTROL, 'A');
    Type("abc");
    Key(VK_SHIFT, false);
    for (int i = 0; i < 2; ++i) {
        input::OnKey(VK_LEFT, 0x4B, true, false);
        input::OnKey(VK_LEFT, 0x4B, true, true);
        std::this_thread::sleep_for(60ms);
    }
    Key(VK_SHIFT, true);
    Type("x");
    ClickAt(370, 156);
    settle();
    Check(LastClickText() == "ax", "abc, Shift+Left twice, then x leaves 'ax'");

    // Focus lost with Ctrl held: Windows sends no up, so nothing may stay held.
    ClickAt(190, 158);
    Chord(VK_CONTROL, 'A');
    Key(VK_CONTROL, false);
    input::OnFocusLost();
    Type("z");
    ClickAt(370, 156);
    settle();
    Check(LastClickText() == "z", "after focus is lost with Ctrl held, typing types");

    // A dead key composes with the next letter: Spanish's acute, then a.
    if (const HKL es = LoadKeyboardLayoutW(L"0000040A", KLF_ACTIVATE)) {
        const SHORT acute = VkKeyScanExW(0xB4, es);
        ClickAt(190, 158);
        Chord(VK_CONTROL, 'A');
        Press(static_cast<unsigned short>(acute & 0xff));
        Press('A');
        ClickAt(370, 156);
        settle();
        Check(acute != -1 && LastClickText() == "\xC3\xA1", "Spanish acute then a types a with an acute");
        ActivateKeyboardLayout(reinterpret_cast<HKL>(HKL_PREV), 0);
        UnloadKeyboardLayout(es);
    } else {
        std::printf("SKIP  no Spanish layout for the dead-key check\n");
    }

    // A button held as UI mode closes: the page has its release, so the game must not get the up.
    ClickAt(190, 158);
    input::OnMouse(0, 0, RI_MOUSE_LEFT_BUTTON_DOWN, 0);
    api::CloseUIMode();
    Check(input::OnMouse(0, 0, RI_MOUSE_LEFT_BUTTON_UP, 0), "a page's press released after UI mode closed stays from the game");
    Check(!input::OnMouse(0, 0, RI_MOUSE_LEFT_BUTTON_DOWN, 0) && !input::OnMouse(0, 0, RI_MOUSE_LEFT_BUTTON_UP, 0),
          "the next click is the game's");
    Check(api::OpenUIMode("MagelightDemo", "panel"), "the panel opens again after");
    std::this_thread::sleep_for(200ms);

    // A key held through UI mode's close: its repeats and its release stay from the game.
    const auto q = static_cast<unsigned short>(MapVirtualKeyW('Q', MAPVK_VK_TO_VSC));
    input::OnKey('Q', q, false, false);
    api::CloseUIMode();
    Check(input::OnKey('Q', q, false, false) && input::OnKey('Q', q, false, true),
          "a key held as UI mode closes: its repeat and release are not the game's");

    // A load closes UI mode at the next tick and refuses it until the load is over.
    Check(api::OpenUIMode("MagelightDemo", "panel"), "the panel opens before a load");
    input::SetLoading(true);
    input::Tick();
    Check(input::UiModeView() == 0 && !api::OpenUIMode("MagelightDemo", "panel"), "a load closes UI mode and refuses it");
    input::SetLoading(false);
    Check(api::OpenUIMode("MagelightDemo", "panel"), "after the load the panel opens");
    std::this_thread::sleep_for(200ms);

    // A right-click on selected text in a field: WebKit's context menu faults there, so the guard must stop it.
    ClickAt(190, 158);
    Chord(VK_CONTROL, 'A');
    input::OnMouse(0, 0, RI_MOUSE_RIGHT_BUTTON_DOWN, 0);
    input::OnMouse(0, 0, RI_MOUSE_RIGHT_BUTTON_UP, 0);
    std::this_thread::sleep_for(200ms);
    const int before = Clicks();
    ClickAt(370, 156);
    settle();
    Check(Clicks() == before + 1, "after a right-click on selected text the page still answers");

    // While a panel holds UI mode, another panel's key does nothing; the HUD's still toggles (checked above).
    Press(VK_F9);
    std::this_thread::sleep_for(100ms);
    Check(input::UiModeView() == panelId, "another panel's hotkey leaves UI mode on the open one");
    api::CloseUIMode();
    std::this_thread::sleep_for(200ms);
    api::Pump(true);
    g_heard.clear();

    // A page's errors reach the log; its console.log does not (pages log secrets).
    Check(LogHas(log, "ml-test-throw"), "a page's uncaught error is logged");
    Check(!LogHas(log, "ml-secret-log"), "a page's console.log is not");

    // A script drives the views.
    Check(!api::OpenUIMode("MagelightDemo", "hud"), "a HUD view refuses UI mode");
    Check(!api::HasView("MagelightDemo", "nope") && !api::Show("MagelightDemo", "nope", true), "an unknown view is refused");
    Check(api::OpenUIMode("MagelightDemo", "panel") && input::UiModeView() == panelId, "Magelight_OpenUIMode opens it");
    std::this_thread::sleep_for(200ms);
    Check(api::IsVisible("MagelightDemo", "panel"), "and shows it");
    Check(api::Show("MagelightDemo", "panel", false) && input::UiModeView() == 0, "hiding it closes UI mode");
    std::this_thread::sleep_for(200ms);
    Check(!api::IsVisible("MagelightDemo", "panel"), "and hides it");
    Check(api::Show("MagelightDemo", "hud", true), "Magelight_Show brings the hud back");
    std::this_thread::sleep_for(200ms);
    Check(api::IsVisible("MagelightDemo", "hud"), "the hud is shown");
    api::Pump(true);
    int opened = 0, closed = 0;
    for (const auto& m : g_heard) opened += m.payload == "1", closed += m.payload == "0";
    Check(opened == 1 && closed == 1, "the script's open and close told '__uimode' 1 then 0");
    g_heard.clear();
    Check(api::Reload("MagelightDemo", "hud"), "Magelight_Reload");
    for (int i = 0; i < 25; ++i, std::this_thread::sleep_for(20ms)) api::Pump(true);
    Check(g_heard.size() == 1 && g_heard[0].channel == "__ready" && g_heard[0].view == "hud", "the reload told '__ready'");
    Check(api::Exported()->version == 1 && api::Exported()->HasView("MagelightDemo", "panel"), "the C++ export works");

    std::printf("%s\n", g_failed ? "FAILED" : "ALL PASSED");
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), g_failed ? 1 : 0);   // never run Ultralight's teardown
}

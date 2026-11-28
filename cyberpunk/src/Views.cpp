#include "Views.h"

#include "Bridge.h"
#include "Input.h"
#include "Log.h"
#include "Platform.h"

#include "../../src/InputCore.h"
#include "../../src/PathTextCore.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

namespace mlcp::views {
namespace {
namespace fs = std::filesystem;
using manifest::Anchor;

struct View {
    manifest::ViewSpec spec;
    ultralight::RefPtr<ultralight::View> ul;
    Row row;
    int errors = 0;   // logged since its page loaded
    bool uiMode = false;   // its last Focus command
    bool told = false;     // this document has had '__uimode' for it
};
constexpr int kErrorsPerLoad = 50;

struct Shared {
    std::vector<std::uint8_t> buf;
    Pixels px;
    bool fresh = false;
};

struct Cmd {
    enum Kind { Show, Focus, Send, Reload } kind;
    int id = 0;
    bool on = false;
    std::string channel, text;
};

// Leaked on purpose (Views.h): no static destructor may reach Ultralight.
auto& g_renderer = *new ultralight::RefPtr<ultralight::Renderer>();
auto& g_views = *new std::vector<View>();   // page thread only
auto& g_sessions = *new std::map<std::string, ultralight::RefPtr<ultralight::Session>>();

std::mutex g_shareMutex;
auto& g_shared = *new std::map<int, Shared>();
std::mutex g_layoutMutex;
auto& g_layout = *new std::vector<Row>();
std::mutex g_cmdMutex;
auto& g_cmds = *new std::vector<Cmd>();
std::atomic<int> g_screenW{1920}, g_screenH{1080};
std::atomic<bool> g_stop{false}, g_started{false};

std::string Utf8(const ultralight::String& s) {
    const auto& u = s.utf8();
    return std::string(u.data(), u.length());
}

View* Find(int id) {
    for (auto& v : g_views)
        if (v.row.id == id) return &v;
    return nullptr;
}

View* Find(ultralight::View* ul) {
    for (auto& v : g_views)
        if (v.ul.get() == ul) return &v;
    return nullptr;
}

class Loader final : public ultralight::LoadListener {
public:
    void OnWindowObjectReady(ultralight::View* caller, uint64_t, bool isMainFrame, const ultralight::String&) override {
        if (!isMainFrame) return;
        if (auto* v = Find(caller)) v->errors = 0, v->told = false, bridge::InstallCore(caller, v->row.id, v->spec.mod, v->spec.name);
    }
    void OnDOMReady(ultralight::View* caller, uint64_t, bool isMainFrame, const ultralight::String& url) override {
        if (!isMainFrame) return;
        Log("views: %s ready", Utf8(url).c_str());
        auto* v = Find(caller);
        if (!v) return;
        bridge::Post(v->spec.mod, v->spec.name, "__ready", "");
        if (v->uiMode && !v->told) {   // loaded or reloaded while it holds UI mode: the new page holds it too
            v->told = true;
            caller->Focus();
            bridge::Dispatch(caller, "__uimode", "1");
        }
    }
    void OnFailLoading(ultralight::View*, uint64_t, bool isMainFrame, const ultralight::String& url,
                       const ultralight::String& description, const ultralight::String&, int code) override {
        if (isMainFrame) Log("views: %s failed to load (%d %s)", Utf8(url).c_str(), code, Utf8(description).c_str());
    }
};
// A page's errors, its uncaught exceptions included, go to the log; its other console lines do not, as pages log
// what a player types.
class Console final : public ultralight::ViewListener {
public:
    void OnAddConsoleMessage(ultralight::View* caller, const ultralight::ConsoleMessage& m) override {
        if (m.level() != ultralight::kMessageLevel_Error) return;
        View* v = Find(caller);
        if (!v || v->errors > kErrorsPerLoad) return;
        if (++v->errors > kErrorsPerLoad) {
            Log("views: %s/%s: more errors this load, not logged", v->spec.mod.c_str(), v->spec.name.c_str());
            return;
        }
        Log("views: %s/%s error: %s (%s:%u)", v->spec.mod.c_str(), v->spec.name.c_str(), Utf8(m.message()).c_str(),
            Utf8(m.source_id()).c_str(), m.line_number());
    }
};
// Built at first use: the base classes' constructors are delay-loaded imports. Never freed.
Loader& TheLoader() {
    static Loader* loader = new Loader();
    return *loader;
}
Console& TheConsole() {
    static Console* console = new Console();
    return *console;
}

// Backbuffer rect for a view: the manifest's 1080p logical pixels, scaled to this screen.
void Place(View& v, int sw, int sh) {
    const double scale = sh / 1080.0;
    auto& s = v.spec;
    Row& r = v.row;
    r.scale = scale;
    if (s.fullscreen) {
        r.w = sw;
        r.h = sh;
        r.x = r.y = 0;
        return;
    }
    r.w = std::max(1, int(std::lround(s.w * scale)));
    r.h = std::max(1, int(std::lround(s.h * scale)));
    const int ox = int(std::lround(s.x * scale)), oy = int(std::lround(s.y * scale));
    const bool right = s.anchor == Anchor::TopRight || s.anchor == Anchor::BottomRight;
    const bool bottom = s.anchor == Anchor::BottomLeft || s.anchor == Anchor::BottomRight;
    r.x = right ? sw - r.w - ox : ox;
    r.y = bottom ? sh - r.h - oy : oy;
}

void Publish() {
    std::vector<Row> rows;
    for (const auto& v : g_views) rows.push_back(v.row);
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.layer < b.layer; });
    std::lock_guard lock(g_layoutMutex);
    g_layout.swap(rows);
}

void Create(manifest::ViewSpec spec, int id, int sw, int sh) {
    View v;
    v.spec = std::move(spec);
    v.row.id = id;
    v.row.mod = v.spec.mod;
    v.row.name = v.spec.name;
    v.row.layer = v.spec.layer;
    v.row.clickThrough = v.spec.clickThrough;
    v.row.hotkey = v.spec.hotkey;
    v.row.visible = v.spec.startVisible;
    Place(v, sw, sh);
    auto& session = g_sessions[v.spec.mod];
    if (!session) session = g_renderer->CreateSession(true, v.spec.mod.c_str());
    ultralight::ViewConfig vc;
    vc.is_accelerated = false;
    vc.is_transparent = true;
    vc.initial_device_scale = v.row.scale;
    v.ul = g_renderer->CreateView(uint32_t(v.row.w), uint32_t(v.row.h), vc, session);
    if (!v.ul) {
        Log("views: %s/%s could not be created", v.spec.mod.c_str(), v.spec.name.c_str());
        return;
    }
    v.ul->set_load_listener(&TheLoader());
    v.ul->set_view_listener(&TheConsole());
    v.ul->set_network_listener(platform::FileOnly());
    v.ul->set_visible(v.row.visible);
    // Escaped as a URL: a '#' or '%' in the manifest's path is part of the file name.
    const std::string url = Magelight::PathText::ToRelativeFileUrl(v.spec.mod + "/" + v.spec.path);
    g_views.push_back(std::move(v));
    g_views.back().ul->LoadURL(url.c_str());
    Log("views: %s/%s id %d, %dx%d at %d,%d (scale %.2f), layer %d%s", g_views.back().spec.mod.c_str(),
        g_views.back().spec.name.c_str(), id, g_views.back().row.w, g_views.back().row.h, g_views.back().row.x,
        g_views.back().row.y, g_views.back().row.scale, g_views.back().row.layer,
        g_views.back().row.visible ? ", shown" : "");
}

void Relayout(int sw, int sh) {
    for (auto& v : g_views) {
        const int w = v.row.w, h = v.row.h;
        Place(v, sw, sh);
        if (v.row.w != w || v.row.h != h) v.ul->Resize(uint32_t(v.row.w), uint32_t(v.row.h), v.row.scale);
    }
    Log("views: laid out for %dx%d", sw, sh);
    Publish();
}

void ApplyCommands(std::vector<Cmd>& cmds) {
    {
        std::lock_guard lock(g_cmdMutex);
        cmds.swap(g_cmds);
    }
    bool changed = false;
    for (const auto& c : cmds) {
        View* v = Find(c.id);
        if (!v) continue;
        switch (c.kind) {
        case Cmd::Show:
            if (v->row.visible != c.on) {
                v->row.visible = c.on;
                v->ul->set_visible(c.on);
                changed = true;
            }
            break;
        case Cmd::Focus:
            if (c.on) v->ul->Focus();
            else v->ul->Unfocus();
            v->uiMode = c.on;
            v->told = true;
            bridge::Dispatch(v->ul.get(), "__uimode", c.on ? "1" : "0");
            break;
        case Cmd::Send:
            bridge::Dispatch(v->ul.get(), c.channel, c.text);
            break;
        case Cmd::Reload:
            v->ul->Reload();
            break;
        }
    }
    cmds.clear();
    if (changed) Publish();
}

void FireInput(std::vector<input::Event>& events) {
    input::Drain(events);
    for (const auto& e : events) {
        View* v = Find(e.view);
        if (!v) continue;
        auto& ul = v->ul;
        const UINT msg = e.kind == input::Event::Up && e.button == ultralight::MouseEvent::kButton_Right ? WM_RBUTTONUP
                       : e.kind == input::Event::Key && !e.up ? WM_KEYDOWN : 0;
        if (msg && Magelight::Input::ArmsMenuGuard(msg, e.wparam)) bridge::ArmMenuGuard(ul.get());
        switch (e.kind) {
        case input::Event::Move:
        case input::Event::Down:
        case input::Event::Up: {
            ultralight::MouseEvent m;
            m.type = e.kind == input::Event::Move ? ultralight::MouseEvent::kType_MouseMoved
                   : e.kind == input::Event::Down ? ultralight::MouseEvent::kType_MouseDown
                                                  : ultralight::MouseEvent::kType_MouseUp;
            m.x = e.x;
            m.y = e.y;
            m.button = static_cast<ultralight::MouseEvent::Button>(e.button);
            ul->FireMouseEvent(m);
            break;
        }
        case input::Event::Wheel: {
            ultralight::ScrollEvent s;
            s.type = ultralight::ScrollEvent::kType_ScrollByPixel;
            s.delta_x = 0;
            s.delta_y = e.delta;
            s.x = e.x;
            s.y = e.y;
            ul->FireScrollEvent(s);
            break;
        }
        case input::Event::Key: {
            // The constructor reads Shift/Ctrl/Alt from this thread's key state: MakeKeyEvent sets it to the queued one.
            ul->FireKeyEvent(Magelight::Input::MakeKeyEvent(
                e.up ? ultralight::KeyEvent::kType_KeyUp : ultralight::KeyEvent::kType_RawKeyDown,
                e.wparam, e.lparam, false, e.keyBits));
            break;
        }
        case input::Event::Char: {
            // The whole code point: one outside the BMP arrives here already joined from its surrogate pair.
            const char32_t c = e.ch;
            char utf8[5] = {};
            if (c < 0x80) utf8[0] = char(c);
            else if (c < 0x800) utf8[0] = char(0xC0 | c >> 6), utf8[1] = char(0x80 | (c & 0x3F));
            else if (c < 0x10000)
                utf8[0] = char(0xE0 | c >> 12), utf8[1] = char(0x80 | (c >> 6 & 0x3F)), utf8[2] = char(0x80 | (c & 0x3F));
            else
                utf8[0] = char(0xF0 | c >> 18), utf8[1] = char(0x80 | (c >> 12 & 0x3F)),
                utf8[2] = char(0x80 | (c >> 6 & 0x3F)), utf8[3] = char(0x80 | (c & 0x3F));
            ultralight::KeyEvent k;
            k.type = ultralight::KeyEvent::kType_Char;
            k.text = utf8;
            k.unmodified_text = utf8;
            k.modifiers = Magelight::Input::ModsFromKeyBits(e.keyBits & ~(Magelight::Input::kKbLWin |
                                                                           Magelight::Input::kKbRWin));
            ul->FireKeyEvent(k);
            break;
        }
        }
    }
}

void ShareFrames() {
    for (auto& v : g_views) {
        auto* surface = static_cast<ultralight::BitmapSurface*>(v.ul->surface());
        if (!surface || surface->dirty_bounds().IsEmpty()) continue;
        auto bmp = surface->bitmap();
        if (!bmp) continue;
        const auto* px = static_cast<const std::uint8_t*>(bmp->LockPixels());
        {
            std::lock_guard lock(g_shareMutex);
            auto& s = g_shared[v.row.id];
            s.buf.assign(px, px + std::size_t(bmp->row_bytes()) * bmp->height());
            s.px = {nullptr, bmp->width(), bmp->height(), bmp->row_bytes()};
            s.fresh = true;
        }
        bmp->UnlockPixels();
        surface->ClearDirtyBounds();
    }
}

void ThreadMain(fs::path pluginDir, fs::path modsRoot, std::vector<manifest::ViewSpec> specs) {
    g_renderer = platform::CreateRenderer(pluginDir, modsRoot);
    if (!g_renderer) {
        Log("views: Renderer::Create failed, no pages this session");
        return;
    }
    int sw = g_screenW, sh = g_screenH;
    int id = 1;
    for (auto& s : specs) Create(std::move(s), id++, sw, sh);
    Publish();
    g_started = true;

    std::vector<Cmd> cmds;
    std::vector<input::Event> events;
    double busyMs = 0;
    std::uint32_t ticks = 0;
    auto next = std::chrono::steady_clock::now();
    while (!g_stop.load(std::memory_order_relaxed)) {
        const auto t0 = std::chrono::steady_clock::now();
        if (g_screenW != sw || g_screenH != sh) {
            sw = g_screenW;
            sh = g_screenH;
            Relayout(sw, sh);
        }
        ApplyCommands(cmds);
        FireInput(events);
        g_renderer->Update();
        g_renderer->RefreshDisplay(0);
        g_renderer->Render();
        ShareFrames();
        busyMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (++ticks == 600) {
            Log("views: %.3f ms a tick over the last 600", busyMs / ticks);
            busyMs = 0;
            ticks = 0;
        }
        next += std::chrono::milliseconds(16);
        std::this_thread::sleep_until(next);
    }
    Log("views: thread stopped");   // the renderer stays, never destroyed
}

// A page stuck in a script stalls the page thread: past the cap a script's messages to pages are dropped, never
// a show, focus or reload.
constexpr std::size_t kCmdCap = 4096;
void Queue(Cmd c) {
    std::lock_guard lock(g_cmdMutex);
    if (c.kind == Cmd::Send && g_cmds.size() >= kCmdCap) {
        static std::atomic<unsigned> dropped{0};
        if (dropped++ % 1000 == 0) Log("views: page thread behind, %u message(s) to pages dropped", dropped.load());
        return;
    }
    g_cmds.push_back(std::move(c));
}
}

void Start(const fs::path& pluginDir, const fs::path& modsRoot, std::vector<manifest::ViewSpec> specs) {
    std::thread(ThreadMain, pluginDir, modsRoot, std::move(specs)).detach();
}

void Stop() { g_stop = true; }

void SetScreen(int width, int height) {
    g_screenW = width;
    g_screenH = height;
}

std::vector<Row> Layout() {
    std::lock_guard lock(g_layoutMutex);
    return g_layout;
}

int IdOf(const std::string& mod, const std::string& view) {
    const auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string m = lower(mod);
    std::lock_guard lock(g_layoutMutex);
    for (const auto& r : g_layout)
        if (r.name == view && lower(r.mod) == m) return r.id;
    return 0;
}

bool Started() { return g_started; }

void TakeFrames(std::map<int, std::vector<std::uint8_t>>& buffers, std::map<int, Pixels>& out) {
    std::unique_lock lock(g_shareMutex, std::try_to_lock);
    if (!lock) return;
    for (auto& [id, s] : g_shared) {
        if (!s.fresh) continue;
        auto& buf = buffers[id];
        buf.swap(s.buf);
        Pixels px = s.px;
        px.data = buf.data();
        out[id] = px;
        s.fresh = false;
    }
}

void Show(int id, bool visible) {
    {   // published at once, so a second toggle or an IsVisible before the page thread's tick sees it
        std::lock_guard lock(g_layoutMutex);
        for (auto& r : g_layout)
            if (r.id == id) r.visible = visible;
    }
    Queue({Cmd::Show, id, visible});
}
void Focus(int id, bool focused) { Queue({Cmd::Focus, id, focused}); }
void Send(int id, const std::string& channel, const std::string& text) { Queue({Cmd::Send, id, false, channel, text}); }
void Reload(int id) { Queue({Cmd::Reload, id}); }
void TakeMessages(std::vector<Message>& out) { bridge::TakeMessages(out); }
}

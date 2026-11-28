#include "Api.h"

#include "Bridge.h"
#include "Input.h"
#include "Log.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

namespace mlcp::api {
namespace {
constexpr std::size_t kCetCap = 4096;

struct Listener {
    std::string mod;   // lowercased, "" = all
    MagelightCP::ListenerFn fn;
    void* user;
};

std::mutex g_listenMutex;
auto& g_listeners = *new std::vector<Listener>();
std::atomic<RedsFire> g_fire{nullptr};
std::mutex g_cetMutex;
auto& g_cet = *new std::deque<std::string>();
std::atomic<bool> g_cetOn{false};
std::atomic<std::size_t> g_cetCount{0};
auto& g_batch = *new std::vector<views::Message>();   // game thread only
std::uint32_t g_logged = 0;
bool g_redsSaid = false;

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void Deliver(const views::Message& m) {
    if (g_logged < 200) {
        ++g_logged;
        // The payload's size only: it can carry what the player typed.
        Log("bridge: %s/%s sent '%s' (%zu bytes)", m.mod.c_str(), m.view.c_str(), m.channel.c_str(), m.payload.size());
    }
    if (const RedsFire fire = g_fire.load()) {
        if (!fire(m) && !g_redsSaid) {
            g_redsSaid = true;
            Log("pump: redscript listeners not reached (Codeware or Magelight's reds missing): CET and C++ still are");
        }
    }
    std::vector<Listener> listeners;
    {
        std::lock_guard lock(g_listenMutex);
        listeners = g_listeners;
    }
    const std::string mod = Lower(m.mod);
    for (const auto& l : listeners)
        if (l.mod.empty() || l.mod == mod) l.fn(m.mod.c_str(), m.view.c_str(), m.channel.c_str(), m.payload.c_str(), l.user);
    if (g_cetOn.load(std::memory_order_relaxed)) {
        std::lock_guard lock(g_cetMutex);
        if (g_cet.size() < kCetCap) g_cet.push_back(m.mod + '\x1f' + m.view + '\x1f' + m.channel + '\x1f' + m.payload);
        g_cetCount.store(g_cet.size(), std::memory_order_relaxed);
    }
}

// C ABI wrappers for Exported().
std::string S(const char* s) { return s ? s : ""; }
}

// Drawing works: the views are made and the host has not given up (a fault, a compositor that could not start).
bool IsReady() { return views::Started() && !input::Disabled(); }
bool HasView(const std::string& mod, const std::string& view) { return views::IdOf(mod, view) != 0; }

bool Show(const std::string& mod, const std::string& view, bool show) {
    const int id = views::IdOf(mod, view);
    if (!id) return false;
    if (!show && input::UiModeView() == id) input::CloseUIMode();
    else views::Show(id, show);
    return true;
}

bool IsVisible(const std::string& mod, const std::string& view) {
    const int id = views::IdOf(mod, view);
    for (const auto& r : views::Layout())
        if (r.id == id) return r.visible;
    return false;
}

// JavaScriptCore drops a whole script that holds invalid UTF-8, with no exception: refuse it here, where it is said.
static bool ValidUtf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + n >= s.size()) return false;
        std::uint32_t cp = n == 0 ? c : c & (0x3F >> n);
        for (int k = 1; k <= n; ++k) {
            const auto t = static_cast<unsigned char>(s[i + k]);
            if ((t >> 6) != 2) return false;
            cp = cp << 6 | (t & 0x3F);
        }
        const std::uint32_t least[] = {0, 0x80, 0x800, 0x10000};
        if (cp < least[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return false;
        i += n + 1;
    }
    return true;
}

bool Send(const std::string& mod, const std::string& view, const std::string& channel, const std::string& text) {
    const int id = views::IdOf(mod, view);
    if (!id || channel.empty()) return false;
    if (!ValidUtf8(channel) || !ValidUtf8(text)) {
        Log("api: send to %s/%s refused, its channel or text is not valid UTF-8", mod.c_str(), view.c_str());
        return false;
    }
    views::Send(id, channel, text);
    return true;
}

bool OpenUIMode(const std::string& mod, const std::string& view) {
    const int id = views::IdOf(mod, view);
    return id && input::OpenUIMode(id);
}

void CloseUIMode() { input::CloseUIMode(); }

std::string UIModeView() {
    const int id = input::UiModeView();
    for (const auto& r : views::Layout())
        if (r.id == id) return r.mod + "/" + r.name;
    return {};
}

bool Reload(const std::string& mod, const std::string& view) {
    const int id = views::IdOf(mod, view);
    if (id) views::Reload(id);
    return id != 0;
}

void SetRedsFire(RedsFire fire) { g_fire = fire; }

void AddListener(const std::string& mod, MagelightCP::ListenerFn fn, void* user) {
    if (!fn) return;
    std::lock_guard lock(g_listenMutex);
    g_listeners.push_back({Lower(mod), fn, user});
}

void Pump(bool gameReady) {
    if (!gameReady || !bridge::Waiting()) return;
    bridge::TakeMessages(g_batch);
    for (const auto& m : g_batch) Deliver(m);
    g_batch.clear();
}

std::string NextForCet() {
    g_cetOn.store(true, std::memory_order_relaxed);
    if (g_cetCount.load(std::memory_order_relaxed) == 0) return {};   // CET asks every frame, loads included
    std::lock_guard lock(g_cetMutex);
    if (g_cet.empty()) return {};
    std::string out = std::move(g_cet.front());
    g_cet.pop_front();
    g_cetCount.store(g_cet.size(), std::memory_order_relaxed);
    return out;
}

const MagelightCP::Api1* Exported() {
    static const MagelightCP::Api1 api{
        1,
        [] { return IsReady(); },
        [](const char* m, const char* v) { return HasView(S(m), S(v)); },
        [](const char* m, const char* v, bool show) { return Show(S(m), S(v), show); },
        [](const char* m, const char* v) { return IsVisible(S(m), S(v)); },
        [](const char* m, const char* v, const char* c, const char* t) { return Send(S(m), S(v), S(c), S(t)); },
        [](const char* m, const char* v) { return OpenUIMode(S(m), S(v)); },
        [] { CloseUIMode(); },
        [](const char* m, const char* v) { return Reload(S(m), S(v)); },
        [](const char* m, MagelightCP::ListenerFn fn, void* user) { AddListener(S(m), fn, user); },
    };
    return &api;
}
}

#include "Session.h"

#include "Input.h"
#include "Log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace mlcp::session {
namespace {
// Adler-32 of the mangled names; RED4ext's address library resolves them (docs/CYBERPUNK.md, "Game states").
constexpr std::uint32_t kLoadingInit = 3416991615u;   // ?OnProcessInit@State_SessionLoading@gsm@@...
constexpr std::uint32_t kActiveInit = 2704353053u;    // ?OnProcessInit@State_SessionActive@gsm@@...
constexpr std::uint32_t kSessionEnd = 436804462u;     // ?OnProcessShutdown@State_Session@gsm@@...

// OnProcessInit(this, ILazyFunctionScheduler&, ArgsView const&, ParamsView&) -> bool; passed through untouched.
using InitFn = bool (*)(void*, void*, void*, void*);
InitFn g_loadingInit = nullptr, g_activeInit = nullptr;
using ShutdownFn = void (*)(void*, void*);
ShutdownFn g_sessionEnd = nullptr;

// Atomics only: these run as a load starts and ends (no lock, no allocation, no log).
std::atomic<bool> g_ready{false};
std::atomic<unsigned> g_loads{0}, g_ends{0};

bool HookedLoadingInit(void* self, void* sched, void* args, void* params) {
    g_ready.store(false, std::memory_order_relaxed);
    g_loads.fetch_add(1, std::memory_order_relaxed);
    input::SetLoading(true);   // no panel holds the input over a loading screen
    return g_loadingInit(self, sched, args, params);
}

bool HookedActiveInit(void* self, void* sched, void* args, void* params) {
    const bool r = g_activeInit(self, sched, args, params);
    g_ready.store(true, std::memory_order_relaxed);
    input::SetLoading(false);
    return r;
}

// The world is coming down (exit to the main menu, or a load from in game): held until the next load is over.
void HookedSessionEnd(void* self, void* sched) {
    g_ready.store(false, std::memory_order_relaxed);
    g_ends.fetch_add(1, std::memory_order_relaxed);
    input::AskClose();
    g_sessionEnd(self, sched);
}

std::uintptr_t Resolve(std::uint32_t hash) {
    using ResolveFn = std::uintptr_t (*)(std::uint32_t);
    HMODULE mod = GetModuleHandleW(L"RED4ext.dll");
    auto fn = mod ? reinterpret_cast<ResolveFn>(GetProcAddress(mod, "RED4ext_ResolveAddress")) : nullptr;
    return fn ? fn(hash) : 0;
}

bool Attach(RED4ext::v1::PluginHandle handle, const RED4ext::v1::Sdk* sdk, std::uint32_t hash, void* detour,
            void** original, const char* name) {
    const auto at = Resolve(hash);
    const bool ok = at && sdk->hooking->Attach(handle, reinterpret_cast<void*>(at), detour, original);
    Log("load: %s at %p %s", name, reinterpret_cast<void*>(at), ok ? "hooked" : "NOT hooked");
    return ok;
}
}

void Install(RED4ext::v1::PluginHandle handle, const RED4ext::v1::Sdk* sdk) {
    const bool loading = Attach(handle, sdk, kLoadingInit, reinterpret_cast<void*>(&HookedLoadingInit),
                                reinterpret_cast<void**>(&g_loadingInit), "gsm::State_SessionLoading::OnProcessInit");
    const bool active = Attach(handle, sdk, kActiveInit, reinterpret_cast<void*>(&HookedActiveInit),
                               reinterpret_cast<void**>(&g_activeInit), "gsm::State_SessionActive::OnProcessInit");
    if (!Attach(handle, sdk, kSessionEnd, reinterpret_cast<void*>(&HookedSessionEnd),
                reinterpret_cast<void**>(&g_sessionEnd), "gsm::State_Session::OnProcessShutdown"))
        Log("load: without the session-end hook, messages still go to scripts while a world comes down");
    if (!loading || !active) Log("load: without both session hooks no message reaches a script this session");
}

bool Ready() { return g_ready.load(std::memory_order_relaxed); }
unsigned Loads() { return g_loads.load(std::memory_order_relaxed); }
unsigned Ends() { return g_ends.load(std::memory_order_relaxed); }
}

// RED4ext entry points and the two hooks: GpuApi::Present, where the pages are drawn over the finished frame, and
// InputSystemWin32Game::MessageProc, where their input is taken.
#include <RED4ext/RED4ext.hpp>
#include <RED4ext/GpuApi/DeviceData.hpp>

#include "Api.h"
#include "Compositor.h"
#include "Input.h"
#include "Log.h"
#include "Manifest.h"
#include "Natives.h"
#include "Session.h"
#include "Views.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <vector>

namespace {
namespace fs = std::filesystem;
using mlcp::Log;

// GpuApi::Present(SwapChainRef const&, bool, uint, job::Builder*), 2.5. The arguments are passed through
// untouched, so their exact types do not matter here.
constexpr std::uint32_t kPresentHash = 2468877568u;
constexpr std::uint32_t kMessageProcHash = 1696011280u;   // InputSystemWin32Game::MessageProc, 2.5
using PresentFn = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);

PresentFn g_present = nullptr;
fs::path g_root, g_mods;
std::atomic<bool> g_dead{false};
std::mutex g_drawMutex;   // the game presents from more than one thread
auto& g_buffers = *new std::map<int, std::vector<std::uint8_t>>();
auto& g_changed = *new std::map<int, mlcp::views::Pixels>();
DWORD g_threads[4] = {};
bool g_started = false;
UINT g_screenW = 0, g_screenH = 0;
double g_drawMs = 0;
std::uint32_t g_frames = 0;

std::uintptr_t ResolveHash(std::uint32_t hash) {
    using ResolveFn = std::uintptr_t (*)(std::uint32_t);
    HMODULE mod = GetModuleHandleW(L"RED4ext.dll");
    auto fn = mod ? reinterpret_cast<ResolveFn>(GetProcAddress(mod, "RED4ext_ResolveAddress")) : nullptr;
    return fn ? fn(hash) : 0;
}

IDXGISwapChain4* SwapChainOf(std::uintptr_t ref) {
    auto* dd = RED4ext::GpuApi::GetDeviceData();
    if (!dd || !ref) return nullptr;
    const auto id = *reinterpret_cast<const std::uint32_t*>(ref);
    auto& pool = dd->swapChains;
    if (!pool.IsValidID(id) || !pool.IsUsedID(id)) return nullptr;
    return pool.GetData(id).swapChain.Get();
}

void NoteThread() {
    const DWORD tid = GetCurrentThreadId();
    for (auto& t : g_threads) {
        if (t == tid) return;
        if (!t) {
            t = tid;
            Log("present: called from thread %lu", tid);
            return;
        }
    }
}

// Under g_drawMutex, taken by HookedPresent outside the fault guard: built /EHsc, a fault runs no destructor.
void Frame(std::uintptr_t ref) {
    NoteThread();
    auto* swapChain = SwapChainOf(ref);
    auto* dd = RED4ext::GpuApi::GetDeviceData();
    if (!swapChain || !dd || !dd->directCommandQueue) {
        static bool told = false;
        if (!told) Log("present: no swapchain for this ref, skipped");
        told = true;
        return;
    }
    DXGI_SWAP_CHAIN_DESC1 d{};
    swapChain->GetDesc1(&d);
    if (d.Width != g_screenW || d.Height != g_screenH) {
        g_screenW = d.Width;
        g_screenH = d.Height;
        Log("present: swapchain %ux%u format %d buffers %u", d.Width, d.Height, int(d.Format), d.BufferCount);
        mlcp::views::SetScreen(int(d.Width), int(d.Height));
        mlcp::input::SetScreen(int(d.Width), int(d.Height));
    }
    if (!g_started) {
        g_started = true;
        mlcp::views::Start(g_root, g_mods, mlcp::manifest::LoadAll(g_mods));
    }
    const auto t0 = std::chrono::steady_clock::now();
    mlcp::views::TakeFrames(g_buffers, g_changed);
    if (mlcp::gfx::Draw(swapChain, dd->directCommandQueue.Get(), mlcp::views::Layout(), g_changed,
                        mlcp::input::CursorNow()))
        g_changed.clear();
    g_drawMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (++g_frames == 600) {
        Log("present: our work %.3f ms a frame over the last 600", g_drawMs / g_frames);
        g_frames = 0;
        g_drawMs = 0;
    }
}

void SafeFrame(std::uintptr_t ref) {
    __try {
        Frame(ref);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dead = true;
        Log("present: fault 0x%08lx, the pages are off for this session", GetExceptionCode());
        mlcp::input::Disable();
    }
}

std::uintptr_t HookedPresent(std::uintptr_t ref, std::uintptr_t a, std::uintptr_t sync, std::uintptr_t job) {
    if (!g_dead.load(std::memory_order_relaxed)) {
        g_drawMutex.lock();
        if (!g_dead.load(std::memory_order_relaxed)) SafeFrame(ref);   // a thread that waited behind a fault
        g_drawMutex.unlock();
    }
    return g_present(ref, a, sync, job);
}

bool LoadRuntime() {
    // Ultralight's DLLs under Magelight's names (tools/rename_ultralight.ps1); the delay-load imports are patched to match.
    for (const wchar_t* name : {L"Magelight1Core.dll", L"MgWCore.dll", L"Magelight1.dll"}) {
        const auto path = g_root / name;
        if (!LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
            Log("load: %ls failed (GetLastError=%lu)", name, GetLastError());
            return false;
        }
    }
    return true;
}
}

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle aHandle, RED4ext::v1::EMainReason aReason,
                                        const RED4ext::v1::Sdk* aSdk) {
    if (aReason == RED4ext::v1::EMainReason::Unload) {
        mlcp::views::Stop();
        return true;
    }
    if (aReason != RED4ext::v1::EMainReason::Load) return true;
    // Pinned: the page thread runs our code until the process ends, so the DLL must never be unmapped under it.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&Main), &pinned);
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(static_cast<HMODULE>(aHandle), self, MAX_PATH);
    g_root = fs::path(self).parent_path();
    // <game>/red4ext/plugins/MagelightCP -> <game>/r6/magelight
    g_mods = (g_root / ".." / ".." / ".." / "r6" / "magelight").lexically_normal();
    mlcp::LogOpen(g_root / "MagelightCP.log");
    Log("load: MagelightCP " MLCP_VERSION " from %ls, mods from %ls", g_root.c_str(), g_mods.c_str());
    // First, so Magelight.reds' natives are bound even with no pages: they answer false, never unbound.
    mlcp::session::Install(aHandle, aSdk);
    mlcp::natives::Install(aHandle, aSdk);
    if (!LoadRuntime()) return true;
    const auto at = ResolveHash(kPresentHash);
    if (!at || !aSdk || !aSdk->hooking) {
        Log("load: GpuApi::Present not resolved (hash %u), nothing hooked", kPresentHash);
        return true;
    }
    const bool ok = aSdk->hooking->Attach(aHandle, reinterpret_cast<void*>(at), reinterpret_cast<void*>(&HookedPresent),
                                          reinterpret_cast<void**>(&g_present));
    Log("load: GpuApi::Present at %p %s", reinterpret_cast<void*>(at), ok ? "hooked" : "NOT hooked");

    const auto sink = ResolveHash(kMessageProcHash);
    const bool inputOk = sink && aSdk->hooking->Attach(aHandle, reinterpret_cast<void*>(sink), mlcp::input::Detour(),
                                                       mlcp::input::Original());
    Log("load: InputSystemWin32Game::MessageProc at %p %s", reinterpret_cast<void*>(sink),
        inputOk ? "hooked" : "NOT hooked");

    std::error_code ec;
    if (fs::exists(g_root / "snapshot.on", ec)) mlcp::gfx::EnableSnapshots(g_root / "snapshot.bmp");
    return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* aInfo) {
    aInfo->name = L"MagelightCP";
    aInfo->author = L"Magelight";
    aInfo->version = RED4EXT_V1_SEMVER(MLCP_VERSION_MAJOR, MLCP_VERSION_MINOR, MLCP_VERSION_PATCH);
    aInfo->runtime = RED4EXT_V1_RUNTIME_VERSION_2_50;   // the Present hash and the SDK's layouts are 2.5's
    aInfo->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports() {
    return RED4EXT_API_VERSION_1_COMPAT_0;
}

// include/MagelightCP_API.h. nullptr for a version this build does not have.
extern "C" __declspec(dllexport) const MagelightCP::Api1* MagelightCP_GetApi(std::uint32_t version) {
    return version == 1 ? mlcp::api::Exported() : nullptr;
}

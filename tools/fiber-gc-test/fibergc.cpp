// fibergc - the page fiber's gate (invariant 1): Magelight's frame model offline, with src/PageStackCore.h's VmLock.
// A 1 MB "game" thread with a stack guarantee (CrashLogger's 64 KB) loads the Ultralight runtime itself, as
// PreloadRuntime does on the game's main thread, then runs every Ultralight call on an 8 MB fiber, one job per
// switch, and its own work on its own stack between jobs, as the game does between frames. Each round runs
// MgStackTest's tests (stack-tests.js) with garbage between them, then a stretch with no View at all (every view
// hibernated), then a View again. The thread leaves the fiber holding the page VM's lock as the host does:
//
//   --lock session  taken once the VM exists, never released (the host)
//   --lock frame    released at each fiber entry, taken again before each exit
//   --lock none     never: a collector helper thread then scans the thread off the fiber with the fiber's bounds
//                   and crashes the process; build.bat requires that it still does
//
// --thread is the page thread's model instead (pageThread, invariant 27): every job, Renderer::Create included, runs
// on a thread of its own with an 8 MB stack (--fibermb sizes it) and a 64 KB guarantee, while the game thread, which
// loaded the runtime, goes on with its own work until the job is done. Between jobs the page thread waits on its own
// stack, whose bounds are WebKit's, so --lock none must pass there; the lock modes apply on the page thread.
//
// Options: --gpu <dir> (MagelightGPU.dll from there on a D3D11 device; accelerated views), --rounds N (3),
// --used KB (600 of the game's 1 MB in use at each switch), --guarantee KB (64), --fibermb N (8), --bench N (frame
// jobs timed after the rounds), --thread. Exit 0 when every test passed; a crash exits with its exception code,
// after a line naming it.
//   fibergc.exe <sdkDir> <stack-tests.js> [options]

#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <intrin.h>
#include <malloc.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <AppCore/Platform.h>
#include <Ultralight/Ultralight.h>

#include "PageStackCore.h"

using namespace ultralight;
namespace PS = Magelight::PageStack;

namespace
{
    enum class LockMode { None, Session, Frame };
    LockMode    g_lock = LockMode::Session;
    std::string g_gpuDir;
    int         g_rounds = 3;
    int         g_usedKb = 600;
    int         g_guaranteeKb = 64;
    int         g_fiberMb = 8;
    int         g_bench = 0;
    bool        g_thread = false;
    std::string g_sdk;
    std::string g_script;

    double QpcUs()
    {
        static const LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart) * 1e6 / static_cast<double>(f.QuadPart);
    }

    DWORD g_gameTid = 0;

    // ---- a crash names itself ------------------------------------------------------------------------------------
    std::string ModuleOffset(const void* addr)
    {
        HMODULE m = nullptr;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(addr), &m) && m) {
            GetModuleFileNameA(m, path, MAX_PATH);
            const char* name = std::strrchr(path, '\\');
            char out[MAX_PATH + 32];
            std::snprintf(out, sizeof out, "%s+0x%llX", name ? name + 1 : path,
                          static_cast<unsigned long long>(reinterpret_cast<const char*>(addr) - reinterpret_cast<const char*>(m)));
            return out;
        }
        char out[32];
        std::snprintf(out, sizeof out, "%p", addr);
        return out;
    }

    LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep)
    {
        const DWORD code = ep->ExceptionRecord->ExceptionCode;
        PWSTR desc = nullptr;
        std::string who;
        if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &desc)) && desc) {
            char b[128];
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, b, sizeof b, nullptr, nullptr);
            who = b;
            LocalFree(desc);
        }
        std::printf("CRASH 0x%08lX at %s in thread %lu%s%s%s\n", code, ModuleOffset(ep->ExceptionRecord->ExceptionAddress).c_str(),
                    GetCurrentThreadId(), GetCurrentThreadId() == g_gameTid ? " (the game thread)" : " (not the game thread)",
                    who.empty() ? "" : ", ", who.c_str());
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), code);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // ---- GPU (the game's device; MagelightGPU.dll as the host loads it) -------------------------------------------
    using MgCreateFn = void* (*)(ID3D11Device*, ID3D11DeviceContext*, void (*)(const char*));
    using MgGetDrvFn = void* (*)(void*);
    using MgHasFn = int (*)(void*);
    using MgDrawFn = void (*)(void*);
    ID3D11Device*        g_dev = nullptr;
    ID3D11DeviceContext* g_ctx = nullptr;
    void*                g_mg = nullptr;
    MgHasFn              g_mgHas = nullptr;
    MgDrawFn             g_mgDraw = nullptr;
    void GpuLog(const char*) {}

    bool InitGpu()
    {
        D3D_FEATURE_LEVEL fl;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx))) {
            std::printf("no D3D11 device\n");
            return false;
        }
        const std::string dll = g_gpuDir + "\\MagelightGPU.dll";
        HMODULE gpu = LoadLibraryA(dll.c_str());
        if (!gpu) { std::printf("cannot load %s (%lu)\n", dll.c_str(), GetLastError()); return false; }
        auto create = reinterpret_cast<MgCreateFn>(GetProcAddress(gpu, "MgGpu_Create"));
        auto drv = reinterpret_cast<MgGetDrvFn>(GetProcAddress(gpu, "MgGpu_GetGPUDriver"));
        g_mgHas = reinterpret_cast<MgHasFn>(GetProcAddress(gpu, "MgGpu_HasCommandsPending"));
        g_mgDraw = reinterpret_cast<MgDrawFn>(GetProcAddress(gpu, "MgGpu_DrawCommandList"));
        if (!create || !drv || !g_mgHas || !g_mgDraw) { std::printf("MagelightGPU.dll exports\n"); return false; }
        g_mg = create(g_dev, g_ctx, &GpuLog);
        if (!g_mg) return false;
        Platform::instance().set_gpu_driver(static_cast<GPUDriver*>(drv(g_mg)));
        return true;
    }

    // ---- the page side (every call on the fiber) -------------------------------------------------------------------
    PS::Exports      g_exports;
    PS::VmLock       g_vmLock;
    RefPtr<Renderer> g_renderer;
    RefPtr<View>     g_view;
    bool             g_ready = false;
    bool             g_vmAtCreate = false;
    int              g_count = 0;
    std::string      g_line;

    class Loader : public LoadListener
    {
    public:
        void OnDOMReady(View*, uint64_t, bool isMainFrame, const String&) override
        {
            if (isMainFrame) g_ready = true;
        }
    } g_loader;

    std::string Js(const std::string& js)
    {
        String ex;
        const String r = g_view->EvaluateScript(String(js.c_str()), &ex);
        std::string out = r.utf8().data();
        if (!ex.empty()) out += std::string("  (exception: ") + ex.utf8().data() + ")";
        return out;
    }

    void Frames(int n)
    {
        for (int i = 0; i < n; ++i) {
            g_renderer->Update();
            g_renderer->RefreshDisplay(0);
            g_renderer->Render();
            if (g_mg && g_mgHas(g_mg)) g_mgDraw(g_mg);
        }
    }

    bool MakeView()
    {
        ViewConfig vc;
        vc.is_accelerated = g_mg != nullptr;
        g_view = g_renderer->CreateView(320, 240, vc, nullptr);
        g_view->set_load_listener(&g_loader);
        g_ready = false;
        g_view->LoadHTML("<html><body style='margin:0;background:#3366cc'>fibergc</body></html>");
        for (int i = 0; i < 1000 && !g_ready; ++i) {
            Frames(1);
            Sleep(5);
        }
        if (!g_ready) { std::printf("the page never loaded\n"); return false; }
        const std::string threw = Js(g_script + "\n;''");
        if (!threw.empty()) { std::printf("stack-tests.js: %s\n", threw.c_str()); return false; }
        g_count = std::atoi(Js("String(MgStackTests.tests.length)").c_str());
        return g_count > 0;
    }

    bool InitUl()
    {
        Config cfg;
        if (g_mg || !g_gpuDir.empty()) cfg.face_winding = FaceWinding::Clockwise;
        Platform::instance().set_config(cfg);
        Platform::instance().set_font_loader(GetPlatformFontLoader());
        Platform::instance().set_file_system(GetPlatformFileSystem(String(g_sdk.c_str())));
        Platform::instance().set_logger(GetDefaultLogger("fibergc_ul.log"));
        if (!g_gpuDir.empty() && !InitGpu()) return false;
        g_renderer = Renderer::Create();
        if (!g_renderer) return false;
        g_vmAtCreate = *g_exports.commonVm != nullptr;
        if (!MakeView()) return false;
        void* viewVm = g_view->JavaScriptVM();
        std::printf("VM: %s after Renderer::Create; View::JavaScriptVM %p, WebCore's common VM %p (%s)\n",
                    g_vmAtCreate ? "made" : "not made yet", viewVm, *g_exports.commonVm,
                    viewVm == *g_exports.commonVm ? "the same" : "DIFFERENT");
        return viewVm == *g_exports.commonVm;
    }

    // Garbage for the collector between tests: the heap grows while the thread is away on its own stack.
    void Churn()
    {
        Js("(function(){window.__keep=window.__keep||[];for(var i=0;i<20000;i++){window.__keep.push({a:i,b:'s'+i,"
           "c:[i,i+1,i+2]});}if(window.__keep.length>200000)window.__keep=[];return '';})()");
    }

    enum class Job { Init, Start, Poll, Churn, Frame, DropView, MakeView, Nop, Shutdown };
    Job  g_job = Job::Init;
    int  g_testIndex = 0;
    int  g_jobResult = 0;   // 0 done, 1 stack overflow, 2 failed
    double g_jobUs = 0;

    int DoJob()
    {
        switch (g_job) {
        case Job::Init: return InitUl() ? 0 : 2;
        case Job::Start:
            g_line.clear();
            Js("window.__mgr='';MgStackTests.start(MgStackTests.tests[" + std::to_string(g_testIndex) +
               "],function(r){window.__mgr=MgStackTests.line(r);});''");
            return 0;
        case Job::Poll:
            Frames(1);
            g_line = Js("window.__mgr");
            return 0;
        case Job::Churn: Churn(); Frames(3); return 0;
        case Job::Frame: Frames(1); return 0;
        case Job::DropView: g_view = nullptr; return 0;
        case Job::MakeView: return MakeView() ? 0 : 2;
        case Job::Nop: return 0;
        case Job::Shutdown:
            g_vmLock.Release(g_exports);
            g_view = nullptr;
            g_renderer = nullptr;
            return 0;
        }
        return 2;
    }

    int GuardedJob()
    {
        const double t0 = QpcUs();
        int r = 0;
        __try {
            r = DoJob();
        } __except (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
            r = 1;
        }
        g_jobUs = QpcUs() - t0;
        return r;
    }

    // ---- the fiber: as Magelight's UlFiberMain ----------------------------------------------------------------------
    LPVOID g_gameFiber = nullptr;
    LPVOID g_pageFiber = nullptr;
    VOID CALLBACK PageFiber(LPVOID)
    {
        ULONG g = 64 * 1024;
        SetThreadStackGuarantee(&g);
        for (;;) {
            if (g_lock == LockMode::Frame) g_vmLock.Release(g_exports);
            g_jobResult = GuardedJob();
            if (g_lock != LockMode::None && g_job != Job::Shutdown) g_vmLock.Hold(g_exports);
            SwitchToFiber(g_gameFiber);
        }
    }

    // The game: about `kb` of its stack in use, then one job on the fiber.
    __declspec(noinline) int Deep(int kb)
    {
        if (kb > 0) {
            volatile char* p = static_cast<volatile char*>(_alloca(16 * 1024));
            p[0] = 1;
            p[16 * 1024 - 1] = 1;
            const int r = Deep(kb - 16);
            p[1] = p[0];
            return r;
        }
        SwitchToFiber(g_pageFiber);
        return g_jobResult;
    }

    // ---- the page thread: as Magelight's PageThreadMain ------------------------------------------------------------
    HANDLE g_go = nullptr;     // auto-reset: the game hands the page thread a job
    HANDLE g_done = nullptr;   // auto-reset: the job is done

    DWORD WINAPI PageThread(void*)
    {
        SetThreadDescription(GetCurrentThread(), L"fibergc page thread");
        ULONG g = 64 * 1024;
        SetThreadStackGuarantee(&g);
        for (;;) {
            WaitForSingleObject(g_go, INFINITE);
            const Job job = g_job;
            // Here, at the top of the thread's own stack, the lock modes as on the fiber.
            if (g_lock == LockMode::Frame) g_vmLock.Release(g_exports);
            g_jobResult = GuardedJob();
            if (g_lock != LockMode::None && job != Job::Shutdown) g_vmLock.Hold(g_exports);
            SetEvent(g_done);
            if (job == Job::Shutdown) return 0;
        }
    }

    // The game thread hands the job over and goes on with its own work until it is done, as the host's game thread
    // goes on with its frame.
    int RunOnPageThread()
    {
        SetEvent(g_go);
        while (WaitForSingleObject(g_done, 0) == WAIT_TIMEOUT) {
            volatile unsigned x = 0;
            for (int i = 0; i < 20000; ++i) x += i;
        }
        return g_jobResult;
    }

    int Run(Job j, int test = 0)
    {
        g_job = j;
        g_testIndex = test;
        const int r = g_thread ? RunOnPageThread() : Deep(g_usedKb);
        if (r == 1) {
            std::printf("STACK OVERFLOW on the page %s (job %d, test %d)\n", g_thread ? "thread" : "fiber",
                        static_cast<int>(j), test);
            std::fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 3);
        }
        return r;
    }

    // The game between frames: its own work, on its own stack, for a moment.
    void GameWork(int ms)
    {
        volatile unsigned x = 0;
        const DWORD until = GetTickCount() + static_cast<DWORD>(ms);
        while (GetTickCount() < until)
            for (int i = 0; i < 100000; ++i) x += i;
    }

    void Stats(const char* what, std::vector<double>& v)
    {
        if (v.empty()) return;
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (double x : v) sum += x;
        const auto pct = [&](double p) { return v[std::min(v.size() - 1, static_cast<size_t>(p * v.size()))]; };
        std::printf("  %s: n=%zu mean %.1f us, median %.1f, p90 %.1f, p99 %.1f\n", what, v.size(), sum / v.size(),
                    pct(0.5), pct(0.9), pct(0.99));
    }

    DWORD WINAPI GameThread(void*)
    {
        g_gameTid = GetCurrentThreadId();
        if (g_guaranteeKb > 0) {
            ULONG g = static_cast<ULONG>(g_guaranteeKb) * 1024;
            SetThreadStackGuarantee(&g);
        }
        // The runtime loads on this thread, before any fiber, as Magelight's PreloadRuntime does (delay-loaded).
        for (const char* dll : { "UltralightCore.dll", "WebCore.dll", "Ultralight.dll", "AppCore.dll" }) {
            if (!LoadLibraryA(dll)) { std::printf("cannot load %s (%lu)\n", dll, GetLastError()); return 2; }
        }
        g_exports = PS::Resolve(GetModuleHandleA("WebCore.dll"));
        if (!g_exports.LockReady()) { std::printf("WebCore does not export %s\n", g_exports.missingLock); return 4; }
        if (g_thread) {
            g_go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!g_go || !g_done || !CreateThread(nullptr, static_cast<SIZE_T>(g_fiberMb) << 20, PageThread, nullptr,
                                                  STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr)) {
                std::printf("no page thread\n");
                return 2;
            }
        } else {
            g_gameFiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
            g_pageFiber = CreateFiberEx(256 * 1024, static_cast<SIZE_T>(g_fiberMb) << 20, FIBER_FLAG_FLOAT_SWITCH, PageFiber, nullptr);
            if (!g_gameFiber || !g_pageFiber) { std::printf("no fiber\n"); return 2; }
        }
        if (Run(Job::Init)) { std::printf("init failed\n"); return 2; }
        const char* lockName = g_lock == LockMode::Session ? "session" : g_lock == LockMode::Frame ? "frame" : "none";
        if (g_thread)
            std::printf("lock %s, %s, pages on a thread of their own (%d MB stack, 64 KB guarantee), the game's guarantee "
                        "%d KB, %d round(s)\n", lockName, g_mg ? "GPU (MagelightGPU.dll)" : "CPU", g_fiberMb, g_guaranteeKb,
                        g_rounds);
        else
            std::printf("lock %s, %s, %d KB of the game's 1 MB used, guarantee %d KB, %d MB fiber, %d round(s)\n", lockName,
                        g_mg ? "GPU (MagelightGPU.dll)" : "CPU", g_usedKb, g_guaranteeKb, g_fiberMb, g_rounds);
        std::fflush(stdout);
        int failures = 0;
        for (int round = 0; round < g_rounds; ++round) {
            for (int i = 0; i < g_count; ++i) {
                Run(Job::Start, i);
                // An asynchronous test ends in a later frame; the thread does its own work in between.
                for (int f = 0; f < 400; ++f) {
                    Run(Job::Poll);
                    if (!g_line.empty()) break;
                    GameWork(2);
                }
                const bool ok = g_line.rfind("ok", 0) == 0;
                if (g_line.empty()) g_line = "FAIL  test " + std::to_string(i) + " never finished";
                if (round == 0 || !ok) std::printf("%s\n", g_line.c_str());
                if (!ok) ++failures;
                std::fflush(stdout);
                for (int f = 0; f < 4; ++f) {
                    GameWork(15);
                    Run(Job::Churn);
                }
            }
            // Every view hibernated: no View, a heap full of garbage, frames still running.
            Run(Job::Churn);
            Run(Job::DropView);
            for (int f = 0; f < 120; ++f) {
                GameWork(8);
                Run(Job::Frame);
            }
            if (Run(Job::MakeView)) { std::printf("FAIL  the view could not be made again\n"); ++failures; break; }
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
            std::printf("  round %d done: %d failure(s) so far, VM lock %s, private %llu MB\n", round + 1, failures,
                        g_vmLock.Held() ? "held" : "not held", static_cast<unsigned long long>(pmc.PrivateUsage >> 20));
            std::fflush(stdout);
        }
        if (g_bench > 0) {
            std::vector<double> seen, inside;
            for (int i = 0; i < g_bench; ++i) {
                Sleep(1);
                const double t0 = QpcUs();
                Run(Job::Frame);
                seen.push_back(QpcUs() - t0);
                inside.push_back(g_jobUs);
            }
            if (!g_thread) Stats("frame job (Update, RefreshDisplay, Render) as the game sees it", seen);
            Stats(g_thread ? "frame job on the page thread" : "frame job on the fiber", inside);
        }
        std::printf("done: %d round(s), %d failure(s)\n", g_rounds, failures);
        std::fflush(stdout);
        Run(Job::Shutdown);
        // Ultralight's teardown at process exit fails fast with the renderer gone from under it.
        TerminateProcess(GetCurrentProcess(), failures ? 1 : 0);
        return failures ? 1 : 0;
    }
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: fibergc <sdkDir> <stack-tests.js> [--lock session|frame|none] [--gpu dir] [--rounds N]\n"
                    "               [--used KB] [--guarantee KB] [--fibermb N] [--bench N] [--thread]\n");
        return 2;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(&OnCrash);
    g_sdk = argv[1];
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--lock") {
            const std::string m = next();
            g_lock = m == "none" ? LockMode::None : m == "frame" ? LockMode::Frame : LockMode::Session;
        } else if (a == "--gpu") g_gpuDir = next();
        else if (a == "--rounds") g_rounds = std::atoi(next().c_str());
        else if (a == "--used") g_usedKb = std::atoi(next().c_str());
        else if (a == "--guarantee") g_guaranteeKb = std::atoi(next().c_str());
        else if (a == "--fibermb") g_fiberMb = std::atoi(next().c_str());
        else if (a == "--bench") g_bench = std::atoi(next().c_str());
        else if (a == "--thread") g_thread = true;
        else { std::printf("unknown option %s\n", a.c_str()); return 2; }
    }
    std::ifstream in(argv[2], std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    g_script = ss.str();
    if (g_script.empty()) { std::printf("could not read %s\n", argv[2]); return 2; }
    // The game's main thread: a 1 MB reserve (SkyrimSE.exe's header).
    HANDLE t = CreateThread(nullptr, 1u << 20, GameThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t) return 2;
    WaitForSingleObject(t, INFINITE);
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    return static_cast<int>(code);
}

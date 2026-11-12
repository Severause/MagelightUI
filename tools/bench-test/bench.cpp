// bench - what a page costs per frame through MagelightGPU.dll, for comparing two Ultralight SDK builds. One run loads
// one page on the in-frame path (Ultralight and the driver on one thread, the driver on the immediate context), paces
// frames at 60 a second, and times each part of a frame the host runs: the page's tick script, Renderer::Update,
// RefreshDisplay + Render, the driver's draw with the target's resolve (CPU), and the GPU time of that draw
// (timestamp queries). Only frames that drew are counted. Build it once per SDK (build.bat) and compare the RESULT
// lines (compare.py). Numbers are comparable only between runs on one PC with nothing else using the GPU.
//
//   bench.exe <assetsDir> <url> --gpu <dir> --name <label> --sdk <label> [--w N] [--h N] [--scale F] [--frames N] [--warmup N]
//             [--msaa N] [--threads N] [--tickfile file.js] [--mouse] [--probe "<js>"]
//
// <assetsDir> holds resources\ (the SDK's) and the pages; <url> is file:///... inside it. The tick is the page's
// window.__bench(n), called once a frame after the page is ready; --tickfile defines it for a page that has none.

#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <AppCore/Platform.h>
#include <Ultralight/Ultralight.h>

#include "MagelightGpuApi.h"

using namespace ultralight;

namespace
{
    std::string g_assets, g_url, g_gpuDir, g_name = "page", g_sdk = "?", g_tickFile, g_probe;
    int         g_w = 1920, g_h = 1080, g_frames = 600, g_warmup = 60, g_msaa = 4, g_threads = 0;
    double      g_scale = 1.0;
    bool        g_mouse = false;

    using MgCreateFn = void* (*)(ID3D11Device*, ID3D11DeviceContext*, void (*)(const char*));
    using MgGetDrvFn = void* (*)(void*);
    using MgHasFn = int (*)(void*);
    using MgDrawFn = void (*)(void*);
    using MgSrvFn = void* (*)(void*, std::uint32_t);
    using MgSamplesFn = int (*)(void*, int);
    using MgStatsFn = int (*)(void*, MgGpuStats*);
    using MgInfoFn = const MgGpuInfo* (*)();

    ID3D11Device*        g_dev = nullptr;
    ID3D11DeviceContext* g_imm = nullptr;

    void GpuLog(const char* m) { std::printf("  MgGpu: %s\n", m); std::fflush(stdout); }

    LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep)
    {
        std::printf("CRASH 0x%08lX at %p\n", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), ep->ExceptionRecord->ExceptionCode);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    std::string ReadFile(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    double g_qpf = 1.0;
    double Now()   // milliseconds
    {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart) * 1000.0 / g_qpf;
    }

    bool g_ready = false;
    class Loader : public LoadListener
    {
    public:
        void OnDOMReady(View*, uint64_t, bool isMainFrame, const String&) override
        {
            if (isMainFrame) g_ready = true;
        }
    } g_loader;

    int g_consoleErrors = 0;
    class Console : public ViewListener
    {
    public:
        void OnAddConsoleMessage(View*, const ConsoleMessage& m) override
        {
            if (m.level() != kMessageLevel_Error) return;
            if (++g_consoleErrors <= 3) std::printf("  page error: %s\n", String(m.message()).utf8().data());
        }
    } g_console;

    struct Series {
        std::vector<double> v;
        void   Add(double x) { v.push_back(x); }
        double At(double q)
        {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size())))];
        }
        double Median() { return At(0.5); }
        double P95() { return At(0.95); }
        double Worst() { return At(1.0); }
    };

    // The GPU time between two points of the immediate context's stream: valid while nothing else uses the device.
    struct GpuTimer {
        ID3D11Query* disjoint = nullptr;
        ID3D11Query* a = nullptr;
        ID3D11Query* b = nullptr;
        bool Init()
        {
            D3D11_QUERY_DESC d{};
            d.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            if (FAILED(g_dev->CreateQuery(&d, &disjoint))) return false;
            d.Query = D3D11_QUERY_TIMESTAMP;
            return SUCCEEDED(g_dev->CreateQuery(&d, &a)) && SUCCEEDED(g_dev->CreateQuery(&d, &b));
        }
        void Begin()
        {
            g_imm->Begin(disjoint);
            g_imm->End(a);
        }
        // Blocks until the GPU has run everything up to here; -1 when the interval cannot be trusted.
        double End()
        {
            g_imm->End(b);
            g_imm->End(disjoint);
            g_imm->Flush();
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            UINT64 ta = 0, tb = 0;
            while (g_imm->GetData(disjoint, &dj, sizeof dj, 0) == S_FALSE) SwitchToThread();
            while (g_imm->GetData(a, &ta, sizeof ta, 0) == S_FALSE) SwitchToThread();
            while (g_imm->GetData(b, &tb, sizeof tb, 0) == S_FALSE) SwitchToThread();
            if (dj.Disjoint || !dj.Frequency || tb < ta) return -1.0;
            return static_cast<double>(tb - ta) * 1000.0 / static_cast<double>(dj.Frequency);
        }
    };
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(&OnCrash);
    if (argc < 3) {
        std::printf("usage: bench <assetsDir> <url> --gpu dir --name label [--w N] [--h N] [--scale F] [--frames N] "
                    "[--warmup N] [--msaa N] [--threads N] [--tickfile file.js] [--mouse] [--probe js]\n");
        return 2;
    }
    g_assets = argv[1];
    g_url = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--gpu") g_gpuDir = next();
        else if (a == "--name") g_name = next();
        else if (a == "--sdk") g_sdk = next();
        else if (a == "--w") g_w = std::atoi(next().c_str());
        else if (a == "--h") g_h = std::atoi(next().c_str());
        else if (a == "--scale") g_scale = std::atof(next().c_str());
        else if (a == "--frames") g_frames = std::atoi(next().c_str());
        else if (a == "--warmup") g_warmup = std::atoi(next().c_str());
        else if (a == "--msaa") g_msaa = std::atoi(next().c_str());
        else if (a == "--threads") g_threads = std::atoi(next().c_str());
        else if (a == "--tickfile") g_tickFile = next();
        else if (a == "--mouse") g_mouse = true;
        else if (a == "--probe") g_probe = next();
        else { std::printf("unknown option %s\n", a.c_str()); return 2; }
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = static_cast<double>(f.QuadPart);
    timeBeginPeriod(1);

    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &g_dev, &fl, &g_imm))) {
        std::printf("no D3D11 device\n");
        return 2;
    }
    const std::string dll = g_gpuDir + "\\MagelightGPU.dll";
    HMODULE gpu = LoadLibraryA(dll.c_str());
    if (!gpu) { std::printf("cannot load %s (%lu)\n", dll.c_str(), GetLastError()); return 2; }
    const auto info = reinterpret_cast<MgInfoFn>(GetProcAddress(gpu, "MgGpu_GetInfo"));
    const MgGpuInfo* gi = info ? info() : nullptr;
    // A driver built for another SDK reads every command at the wrong offsets. Two betas of one version share
    // ULTRALIGHT_VERSION, so pairing the driver with its SDK is the caller's job (--sdk only labels the result).
    if (!gi || gi->contract != MGGPU_CONTRACT || gi->gpuStateSize != sizeof(GPUState) || gi->commandSize != sizeof(Command)) {
        std::printf("%s does not match this bench's driver contract or command layout\n", dll.c_str());
        return 2;
    }
    const auto mgCreate = reinterpret_cast<MgCreateFn>(GetProcAddress(gpu, "MgGpu_Create"));
    const auto mgDrv = reinterpret_cast<MgGetDrvFn>(GetProcAddress(gpu, "MgGpu_GetGPUDriver"));
    const auto mgHas = reinterpret_cast<MgHasFn>(GetProcAddress(gpu, "MgGpu_HasCommandsPending"));
    const auto mgDraw = reinterpret_cast<MgDrawFn>(GetProcAddress(gpu, "MgGpu_DrawCommandList"));
    const auto mgSrv = reinterpret_cast<MgSrvFn>(GetProcAddress(gpu, "MgGpu_GetTextureSRV"));
    const auto mgSamples = reinterpret_cast<MgSamplesFn>(GetProcAddress(gpu, "MgGpu_SetSampleCount"));
    const auto mgStats = reinterpret_cast<MgStatsFn>(GetProcAddress(gpu, "MgGpu_GetStats"));
    if (!mgCreate || !mgDrv || !mgHas || !mgDraw || !mgSrv || !mgSamples || !mgStats) {
        std::printf("MagelightGPU.dll lacks an export\n");
        return 2;
    }

    Config cfg;
    cfg.num_renderer_threads = static_cast<uint32_t>(g_threads);
    Platform::instance().set_config(cfg);
    Platform::instance().set_font_loader(GetPlatformFontLoader());
    Platform::instance().set_file_system(GetPlatformFileSystem(String(g_assets.c_str())));
    Platform::instance().set_logger(GetDefaultLogger("bench_ul.log"));
    void* mg = mgCreate(g_dev, g_imm, &GpuLog);
    if (!mg) { std::printf("MgGpu_Create refused the context\n"); return 2; }
    const int msaa = mgSamples(mg, g_msaa);
    Platform::instance().set_gpu_driver(static_cast<GPUDriver*>(mgDrv(mg)));
    RefPtr<Renderer> renderer = Renderer::Create();
    if (!renderer) { std::printf("no renderer\n"); return 2; }
    GpuTimer timer;
    if (!timer.Init()) { std::printf("no timestamp queries\n"); return 2; }

    ViewConfig vc;
    vc.is_accelerated = true;
    vc.is_transparent = true;
    vc.initial_focus = false;
    vc.initial_device_scale = g_scale;
    RefPtr<View> view = renderer->CreateView(static_cast<uint32_t>(g_w), static_cast<uint32_t>(g_h), vc, nullptr);
    view->set_load_listener(&g_loader);
    view->set_view_listener(&g_console);

    const auto frame = [&](double& update, double& render, double& draw, double& gpuMs) {
        const double t0 = Now();
        renderer->Update();
        const double t1 = Now();
        renderer->RefreshDisplay(0);
        renderer->Render();
        const double t2 = Now();
        update = t1 - t0;
        render = t2 - t1;
        draw = 0.0;
        gpuMs = 0.0;
        if (!mgHas(mg)) return false;
        timer.Begin();
        mgDraw(mg);
        mgSrv(mg, view->render_target().texture_id);   // the resolve the host's composite asks for
        draw = Now() - t2;
        gpuMs = timer.End();
        return true;
    };

    // ---- load: unpaced, to the first frame that drew after DOM ready ------------------------------------------------
    const double loadStart = Now();
    view->LoadURL(String(g_url.c_str()));
    double readyMs = -1.0, paintMs = -1.0;
    while (paintMs < 0.0) {
        double u, r, d, g;
        const bool drew = frame(u, r, d, g);
        const double t = Now() - loadStart;
        if (g_ready && readyMs < 0.0) readyMs = t;
        if (g_ready && drew) paintMs = t;
        if (t > 60000.0) { std::printf("the page never painted (ready %d)\n", g_ready ? 1 : 0); return 2; }
        Sleep(1);
    }
    if (!g_tickFile.empty()) {
        const std::string js = ReadFile(g_tickFile);
        if (js.empty()) { std::printf("could not read %s\n", g_tickFile.c_str()); return 2; }
        view->EvaluateScript(String(js.c_str()));
    }

    // ---- steady state: paced at 60 a second -----------------------------------------------------------------------
    Series script, update, render, draw, cpu, gpuT;
    int drawn = 0, counted = 0, gpuBad = 0;
    double cpuTotal = 0.0;   // every counted frame, drawn or not: the page's whole cost to the thread
    const double period = 1000.0 / 60.0;
    double next = Now();
    for (int n = 0; n < g_warmup + g_frames; ++n) {
        while (Now() < next) Sleep(Now() + 1.5 < next ? 1 : 0);
        next += period;
        const double s0 = Now();
        if (g_mouse) {
            // A slow sweep over the page: hover states and whatever tracks the pointer.
            const int x = static_cast<int>((0.5 + 0.45 * std::sin(n * 0.031)) * g_w / g_scale);
            const int y = static_cast<int>((0.5 + 0.40 * std::sin(n * 0.047 + 1.0)) * g_h / g_scale);
            MouseEvent ev;
            ev.type = MouseEvent::kType_MouseMoved;
            ev.x = x;
            ev.y = y;
            ev.button = MouseEvent::kButton_None;
            view->FireMouseEvent(ev);
        }
        char call[64];
        std::snprintf(call, sizeof call, "window.__bench&&window.__bench(%d)", n);
        view->EvaluateScript(String(call));
        const double sMs = Now() - s0;
        double u, r, d, g;
        const bool drew = frame(u, r, d, g);
        if (n < g_warmup) continue;
        ++counted;
        cpuTotal += sMs + u + r + d;
        if (!drew) continue;
        ++drawn;
        script.Add(sMs);
        update.Add(u);
        render.Add(r);
        draw.Add(d);
        cpu.Add(sMs + u + r + d);
        if (g >= 0.0) gpuT.Add(g);
        else ++gpuBad;
    }

    if (!g_probe.empty()) {
        const String out = view->EvaluateScript(String(g_probe.c_str()));
        std::printf("  probe: %s\n", out.utf8().data());
    }
    MgGpuStats st{};
    st.size = sizeof(st);
    mgStats(mg, &st);
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);

    std::printf("RESULT name=%s sdk=%s w=%d h=%d scale=%.2f msaa=%d threads=%d ready_ms=%.1f paint_ms=%.1f frames=%d drawn=%d "
                "cpu_total=%.1f cpu_med=%.3f cpu_p95=%.3f cpu_worst=%.3f script_med=%.3f update_med=%.3f render_med=%.3f draw_med=%.3f "
                "gpu_med=%.3f gpu_p95=%.3f gpu_bad=%d private_mb=%.0f tex_mb=%.1f target_mb=%.1f msaa_mb=%.1f flushes=%llu "
                "page_errors=%d\n",
                g_name.c_str(), g_sdk.c_str(), g_w, g_h, g_scale, msaa, g_threads, readyMs, paintMs, counted, drawn,
                cpuTotal, cpu.Median(), cpu.P95(), cpu.Worst(), script.Median(), update.Median(), render.Median(), draw.Median(),
                gpuT.Median(), gpuT.P95(), gpuBad, static_cast<double>(pmc.PrivateUsage) / 1048576.0,
                static_cast<double>(st.textureBytes) / 1048576.0, static_cast<double>(st.targetBytes) / 1048576.0,
                static_cast<double>(st.msaaBytes) / 1048576.0, static_cast<unsigned long long>(st.flushes), g_consoleErrors);
    std::fflush(stdout);
    // Ultralight's objects are left as the host leaves them at exit.
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

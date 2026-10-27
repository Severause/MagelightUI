// deferred_test - MagelightGPU.dll on a deferred context (the page thread, invariant 27) against the immediate
// context (the in-frame path), pixel for pixel. One run renders one page and dumps the View's resolved target:
//
//   --mode immediate  Ultralight and the driver on the main thread, the driver on the immediate context (in-frame).
//   --mode thread     Ultralight on a page thread of its own (8 MB stack) that owns Renderer::Create; the driver on a
//                     deferred context there; each frame's FinishCommandList goes to the main thread, which executes
//                     it on the immediate context and keeps an AddRef on the frame's target SRV (the host's model).
//
// --case release: at frame 60 the View is released and made again on the same page while the main thread still
// holds the old target's SRV and copies from it for a few frames (a texture Ultralight freed while a frame that drew
// it waits). --warp runs on WARP, which prints its DriverCommandLists (0 exercises the emulated path).
//
//   deferred_test.exe <sdkDir> <page.html> --gpu <dir> --mode immediate|thread --out <file.raw> [--msaa N] [--warp]
//                     [--case release] [--frames N]
//   deferred_test.exe --compare a.raw b.raw     exit 0 when every byte matches

#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <AppCore/Platform.h>
#include <Ultralight/Ultralight.h>

using namespace ultralight;

namespace
{
    std::string g_sdk, g_page, g_gpuDir, g_out, g_mode = "immediate", g_case;
    int         g_msaa = 1;
    int         g_frames = 180;
    bool        g_warp = false;

    using MgCreateFn = void* (*)(ID3D11Device*, ID3D11DeviceContext*, void (*)(const char*));
    using MgGetDrvFn = void* (*)(void*);
    using MgHasFn = int (*)(void*);
    using MgDrawFn = void (*)(void*);
    using MgSrvFn = void* (*)(void*, std::uint32_t);
    using MgSamplesFn = int (*)(void*, int);
    MgCreateFn  g_mgCreate = nullptr;
    MgGetDrvFn  g_mgDrv = nullptr;
    MgHasFn     g_mgHas = nullptr;
    MgDrawFn    g_mgDraw = nullptr;
    MgSrvFn     g_mgSrv = nullptr;
    MgSamplesFn g_mgSamples = nullptr;
    void*       g_mg = nullptr;

    ID3D11Device*        g_dev = nullptr;
    ID3D11DeviceContext* g_imm = nullptr;
    ID3D11DeviceContext* g_def = nullptr;   // thread mode: the page thread's context

    void GpuLog(const char* m) { std::printf("  MgGpu: %s\n", m); std::fflush(stdout); }

    LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep)
    {
        std::printf("CRASH 0x%08lX at %p in thread %lu\n", ep->ExceptionRecord->ExceptionCode,
                    ep->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
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

    bool LoadDriver()
    {
        const std::string dll = g_gpuDir + "\\MagelightGPU.dll";
        HMODULE gpu = LoadLibraryA(dll.c_str());
        if (!gpu) { std::printf("cannot load %s (%lu)\n", dll.c_str(), GetLastError()); return false; }
        g_mgCreate = reinterpret_cast<MgCreateFn>(GetProcAddress(gpu, "MgGpu_Create"));
        g_mgDrv = reinterpret_cast<MgGetDrvFn>(GetProcAddress(gpu, "MgGpu_GetGPUDriver"));
        g_mgHas = reinterpret_cast<MgHasFn>(GetProcAddress(gpu, "MgGpu_HasCommandsPending"));
        g_mgDraw = reinterpret_cast<MgDrawFn>(GetProcAddress(gpu, "MgGpu_DrawCommandList"));
        g_mgSrv = reinterpret_cast<MgSrvFn>(GetProcAddress(gpu, "MgGpu_GetTextureSRV"));
        g_mgSamples = reinterpret_cast<MgSamplesFn>(GetProcAddress(gpu, "MgGpu_SetSampleCount"));
        if (!g_mgCreate || !g_mgDrv || !g_mgHas || !g_mgDraw || !g_mgSrv || !g_mgSamples) {
            std::printf("MagelightGPU.dll lacks an export\n");
            return false;
        }
        return true;
    }

    // ---- the page side: every Ultralight call on one thread ----------------------------------------------------------
    RefPtr<Renderer> g_renderer;
    RefPtr<View>     g_view;
    bool             g_ready = false;

    class Loader : public LoadListener
    {
    public:
        void OnDOMReady(View*, uint64_t, bool isMainFrame, const String&) override
        {
            if (isMainFrame) g_ready = true;
        }
    } g_loader;

    void MakeView()
    {
        ViewConfig vc;
        vc.is_accelerated = true;
        vc.is_transparent = true;
        vc.initial_focus = false;
        g_view = g_renderer->CreateView(1280, 720, vc, nullptr);
        g_view->set_load_listener(&g_loader);
        g_ready = false;
        g_view->LoadHTML(String(ReadFile(g_page).c_str()));
    }

    bool InitDriver(ID3D11DeviceContext* ctx)
    {
        g_mg = g_mgCreate(g_dev, ctx, &GpuLog);
        if (!g_mg) { std::printf("MgGpu_Create refused the context\n"); return false; }
        const int got = g_mgSamples(g_mg, g_msaa);
        std::printf("  MSAA asked %d, got %d\n", g_msaa, got);
        return true;
    }

    bool InitPage(ID3D11DeviceContext* ctx)
    {
        Platform::instance().set_config(Config{});
        Platform::instance().set_font_loader(GetPlatformFontLoader());
        Platform::instance().set_file_system(GetPlatformFileSystem(String(g_sdk.c_str())));
        Platform::instance().set_logger(GetDefaultLogger("deferred_test_ul.log"));
        if (!InitDriver(ctx)) return false;
        Platform::instance().set_gpu_driver(static_cast<GPUDriver*>(g_mgDrv(g_mg)));
        g_renderer = Renderer::Create();
        if (!g_renderer) return false;
        MakeView();
        return true;
    }

    // One page frame. Returns the View's target SRV, AddRef'ed (the host's snapshot), or nullptr.
    ID3D11ShaderResourceView* PageFrame(int frame)
    {
        if (g_case == "release" && frame == 60) {
            g_view = nullptr;   // Ultralight frees the target (in a later Render) while the main thread holds its SRV
            MakeView();
        }
        // Glyphs and shapes the first paint did not have: Ultralight adds them to textures it already made, which is
        // the driver's UpdateTexture with a dirty box (the upload the emulated command lists place wrongly unless the
        // driver adjusts the source pointer). Every frame from here on draws them.
        if (frame == 40 || frame == 100)
            g_view->EvaluateScript(String(
                "document.body.insertAdjacentHTML('beforeend', '<div style=\"position:absolute;left:60px;top:"
                "' + (560 + document.querySelectorAll(\".late\").length * 70) + 'px;width:880px\" class=\"late\">"
                "<span style=\"font-size:31px;font-weight:bold\">Ωμέγα Ψυχή Жизнь Ğğ Łł Ŵŵ</span> "
                "<span style=\"font-size:19px;font-style:italic\">qwfpgjluy ;[]{}#~@ 9182736450</span>"
                "<span style=\"display:inline-block;width:60px;height:40px;margin-left:12px;border-radius:13px 4px;"
                "background:rgba(20,120,60,.6);box-shadow:0 0 9px #000\"></span></div>'); ''"));
        g_renderer->Update();
        g_renderer->RefreshDisplay(0);
        g_renderer->Render();
        if (g_mgHas(g_mg)) g_mgDraw(g_mg);
        const RenderTarget rt = g_view->render_target();
        auto* srv = static_cast<ID3D11ShaderResourceView*>(g_mgSrv(g_mg, rt.texture_id));   // resolves MSAA
        if (srv) srv->AddRef();
        return srv;
    }

    // --case upload: the driver alone, no View. A texture made through the GPUDriver interface, then a box away from its
    // origin updated (the dirty box Ultralight passes when it adds to a texture it already made). Pages rarely make
    // one off the first row, so this is the case that proves the deferred upload lands where it should.
    constexpr std::uint32_t kUpW = 256, kUpH = 128;
    constexpr int kBoxL = 37, kBoxT = 21, kBoxR = 101, kBoxB = 77;
    constexpr std::uint32_t kUpBack = 0xFF2040C0u, kUpBox = 0xFF30D050u;   // BGRA in memory: two opaque colours

    ID3D11ShaderResourceView* UploadCase()
    {
        auto* drv = static_cast<GPUDriver*>(g_mgDrv(g_mg));
        RefPtr<Bitmap> bmp = Bitmap::Create(kUpW, kUpH, BitmapFormat::BGRA8_UNORM_SRGB);
        const auto fill = [&](bool box) {
            auto* p = static_cast<std::uint8_t*>(bmp->LockPixels());
            for (std::uint32_t y = 0; y < kUpH; ++y)
                for (std::uint32_t x = 0; x < kUpW; ++x) {
                    const bool in = box && x >= kBoxL && x < kBoxR && y >= kBoxT && y < kBoxB;
                    reinterpret_cast<std::uint32_t*>(p + static_cast<size_t>(y) * bmp->row_bytes())[x] = in ? kUpBox : kUpBack;
                }
            bmp->UnlockPixels();
        };
        fill(false);
        const std::uint32_t id = drv->NextTextureId();
        drv->CreateTexture(id, bmp, 0);
        fill(true);
        drv->UpdateTexture(id, bmp, IntRect{ kBoxL, kBoxT, kBoxR, kBoxB });
        auto* srv = static_cast<ID3D11ShaderResourceView*>(g_mgSrv(g_mg, id));
        if (srv) srv->AddRef();
        return srv;
    }

    // The upload case's dump against what was uploaded: the box in its colour, everything else in the other.
    int CheckUpload()
    {
        const std::string d = ReadFile(g_out);
        if (d.size() != 8 + static_cast<size_t>(kUpW) * kUpH * 4) { std::printf("upload: wrong dump size\n"); return 1; }
        int wrong = 0;
        for (std::uint32_t y = 0; y < kUpH; ++y)
            for (std::uint32_t x = 0; x < kUpW; ++x) {
                std::uint32_t v = 0;
                std::memcpy(&v, d.data() + 8 + (static_cast<size_t>(y) * kUpW + x) * 4, 4);
                const bool in = x >= kBoxL && x < kBoxR && y >= kBoxT && y < kBoxB;
                if (v != (in ? kUpBox : kUpBack)) ++wrong;
            }
        if (wrong) { std::printf("upload: %d texel(s) wrong\n", wrong); return 1; }
        std::printf("upload: the box landed where it was sent\n");
        return 0;
    }

    // ---- the main thread's side ------------------------------------------------------------------------------------
    // A GPU read of a texture the page side may have released: what the composite's draw is to the host.
    void TouchTarget(ID3D11ShaderResourceView* srv)
    {
        ID3D11Resource* res = nullptr;
        srv->GetResource(&res);
        ID3D11Texture2D* tex = nullptr;
        res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = 0;
        d.MiscFlags = 0;
        ID3D11Texture2D* copy = nullptr;
        if (SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &copy))) {
            g_imm->CopyResource(copy, tex);
            copy->Release();
        }
        tex->Release();
        res->Release();
    }

    bool Dump(ID3D11ShaderResourceView* srv)
    {
        ID3D11Resource* res = nullptr;
        srv->GetResource(&res);
        ID3D11Texture2D* tex = nullptr;
        res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
        res->Release();
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &staging))) { tex->Release(); return false; }
        g_imm->CopyResource(staging, tex);
        tex->Release();
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(g_imm->Map(staging, 0, D3D11_MAP_READ, 0, &m))) { staging->Release(); return false; }
        std::ofstream out(g_out, std::ios::binary);
        const std::uint32_t wh[2] = { d.Width, d.Height };
        out.write(reinterpret_cast<const char*>(wh), sizeof wh);
        for (UINT y = 0; y < d.Height; ++y)
            out.write(static_cast<const char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, static_cast<std::streamsize>(d.Width) * 4);
        g_imm->Unmap(staging, 0);
        staging->Release();
        std::printf("  dumped %ux%u -> %s\n", d.Width, d.Height, g_out.c_str());
        return true;
    }

    int RunImmediate()
    {
        if (g_case == "upload") {
            if (!InitDriver(g_imm)) return 2;
            ID3D11ShaderResourceView* srv = UploadCase();
            return srv && Dump(srv) ? CheckUpload() : 2;
        }
        if (!InitPage(g_imm)) return 2;
        ID3D11ShaderResourceView* held = nullptr;
        for (int f = 0; f < g_frames; ++f) {
            ID3D11ShaderResourceView* srv = PageFrame(f);
            if (held && g_case == "release" && f >= 60 && f < 70) TouchTarget(held);
            if (srv) { if (held) held->Release(); held = srv; }
            Sleep(2);
        }
        if (!g_ready) { std::printf("the page never loaded\n"); return 2; }
        return held && Dump(held) ? 0 : 2;
    }

    // Thread mode: the handoff the host makes, reduced to one slot (the main thread waits for each frame here, so the
    // dumps of the two modes come from the same frame number).
    struct Packet { ID3D11CommandList* list = nullptr; ID3D11ShaderResourceView* srv = nullptr; bool ready = false; };
    std::mutex              g_mx;
    std::condition_variable g_cv;
    Packet                  g_packet;
    int                     g_go = -1;   // the frame the page thread may run
    bool                    g_pageFailed = false;

    DWORD WINAPI PageThread(void*)
    {
        SetThreadDescription(GetCurrentThread(), L"deferred_test page thread");
        ULONG g = 64 * 1024;
        SetThreadStackGuarantee(&g);
        const bool ok = InitPage(g_def);
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_pageFailed = !ok;
        }
        g_cv.notify_all();
        if (!ok) return 2;
        for (int f = 0; f < g_frames; ++f) {
            {
                std::unique_lock<std::mutex> lk(g_mx);
                g_cv.wait(lk, [&] { return g_go >= f; });
            }
            ID3D11ShaderResourceView* srv = PageFrame(f);
            ID3D11CommandList* list = nullptr;
            if (FAILED(g_def->FinishCommandList(FALSE, &list))) { std::printf("FinishCommandList failed\n"); return 2; }
            {
                std::lock_guard<std::mutex> lk(g_mx);
                g_packet = { list, srv, true };
            }
            g_cv.notify_all();
        }
        return 0;
    }

    DWORD WINAPI UploadThread(void*)
    {
        if (!InitDriver(g_def)) return 2;
        g_packet.srv = UploadCase();
        return SUCCEEDED(g_def->FinishCommandList(FALSE, &g_packet.list)) ? 0 : 2;
    }

    int RunThread()
    {
        if (FAILED(g_dev->CreateDeferredContext(0, &g_def))) { std::printf("no deferred context\n"); return 2; }
        if (g_case == "upload") {
            HANDLE u = CreateThread(nullptr, 0, UploadThread, nullptr, 0, nullptr);
            if (!u) return 2;
            WaitForSingleObject(u, INFINITE);
            DWORD code = 2;
            GetExitCodeThread(u, &code);
            if (code || !g_packet.list || !g_packet.srv) return 2;
            g_imm->ExecuteCommandList(g_packet.list, FALSE);
            return Dump(g_packet.srv) ? CheckUpload() : 2;
        }
        HANDLE t = CreateThread(nullptr, static_cast<SIZE_T>(8) << 20, PageThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!t) return 2;
        ID3D11ShaderResourceView* held = nullptr;
        for (int f = 0; f < g_frames; ++f) {
            Packet p;
            {
                std::unique_lock<std::mutex> lk(g_mx);
                g_go = f;
                g_cv.notify_all();
                g_cv.wait(lk, [&] { return g_packet.ready || g_pageFailed; });
                if (g_pageFailed) { std::printf("the page thread failed to start\n"); return 2; }
                p = g_packet;
                g_packet = {};
            }
            g_imm->ExecuteCommandList(p.list, FALSE);
            p.list->Release();
            if (held && g_case == "release" && f >= 60 && f < 70) TouchTarget(held);
            if (p.srv) { if (held) held->Release(); held = p.srv; }
            Sleep(2);
        }
        WaitForSingleObject(t, INFINITE);
        if (!g_ready) { std::printf("the page never loaded\n"); return 2; }
        return held && Dump(held) ? 0 : 2;
    }

    int Compare(const std::string& a, const std::string& b)
    {
        const std::string x = ReadFile(a), y = ReadFile(b);
        if (x.size() < 8 || y.size() < 8) { std::printf("compare: missing dump\n"); return 2; }
        if (x.size() != y.size() || std::memcmp(x.data(), y.data(), 8) != 0) {
            std::printf("compare: sizes differ\n");
            return 1;
        }
        std::uint32_t w = 0;
        std::memcpy(&w, x.data(), 4);
        size_t drawn = 0;
        for (size_t i = 8 + 3; i < x.size(); i += 4)
            if (x[i]) ++drawn;
        if (!drawn) { std::printf("compare: nothing was drawn\n"); return 1; }
        size_t diff = 0, first = SIZE_MAX;
        for (size_t i = 8; i < x.size(); ++i)
            if (x[i] != y[i]) { ++diff; if (first == SIZE_MAX) first = i; }
        if (!diff) { std::printf("compare: identical\n"); return 0; }
        const size_t px = (first - 8) / 4;
        std::printf("compare: %zu byte(s) differ, first at pixel (%zu, %zu)\n", diff, px % w, px / w);
        return 1;
    }
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(&OnCrash);
    if (argc == 4 && std::string(argv[1]) == "--compare") return Compare(argv[2], argv[3]);
    if (argc < 3) {
        std::printf("usage: deferred_test <sdkDir> <page.html> --gpu dir --mode immediate|thread --out file.raw "
                    "[--msaa N] [--warp] [--case release] [--frames N]\n       deferred_test --compare a.raw b.raw\n");
        return 2;
    }
    g_sdk = argv[1];
    g_page = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--gpu") g_gpuDir = next();
        else if (a == "--mode") g_mode = next();
        else if (a == "--out") g_out = next();
        else if (a == "--msaa") g_msaa = std::atoi(next().c_str());
        else if (a == "--warp") g_warp = true;
        else if (a == "--case") g_case = next();
        else if (a == "--frames") g_frames = std::atoi(next().c_str());
        else { std::printf("unknown option %s\n", a.c_str()); return 2; }
    }
    if (ReadFile(g_page).empty()) { std::printf("could not read %s\n", g_page.c_str()); return 2; }
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, g_warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &g_dev, &fl, &g_imm))) {
        std::printf("no D3D11 device\n");
        return 2;
    }
    D3D11_FEATURE_DATA_THREADING th{};
    g_dev->CheckFeatureSupport(D3D11_FEATURE_THREADING, &th, sizeof th);
    std::printf("%s, %s, DriverCommandLists %d, msaa %d%s: %s\n", g_mode.c_str(), g_warp ? "WARP" : "hardware",
                th.DriverCommandLists, g_msaa, g_case.empty() ? "" : (", case " + g_case).c_str(), g_page.c_str());
    std::fflush(stdout);
    if (!LoadDriver()) return 2;
    const int r = g_mode == "thread" ? RunThread() : RunImmediate();
    std::fflush(stdout);
    // Ultralight's objects belong to the page thread: leave them, as the host does at exit.
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(r));
    return r;
}

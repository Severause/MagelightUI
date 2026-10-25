// visprobe - what View::set_visible does on Ultralight 2.0, for Magelight's hide / show / prepaint (SyncViewVisibility):
//   A  a visible page hidden and shown: no page code inside set_visible, nothing painted or animated while hidden,
//      timers / MessageChannel / promises still run, the first frame after a show paints what changed while hidden
//   B  a page loaded hidden, then prepainted as the host does it (shown between Update and RefreshDisplay, Render,
//      hidden again, in one frame), then shown
//   C  a page loaded hidden with no prepaint (the cost the prepaint moves), and a hidden View's next document
//   D  ViewConfig::enable_hidden_timer_throttling
//   E  Render() with every view hidden: no command list (FrameWork renders like this once a second with nothing shown)
//   F  where in the frame a show can land: before Update (the host's first sync), between Update and RefreshDisplay
//      (its second) and after RefreshDisplay (never: no rAF runs in that Render), for a page fed while hidden directly,
//      through requestAnimationFrame, two rAF deep and MessageChannel-then-rAF; a prepaint at the last two; a hide at
//      the last two; where the visibilitychange handler runs after each
//   G  resize, scroll, matchMedia change, ResizeObserver and IntersectionObserver: held while hidden (direct reads and a
//      fresh matchMedia(q) stay current, a kept MediaQueryList's matches does not), then run once, coalesced, inside the
//      Render of a show or of a prepaint, before its paint; and none of them runs inside Resize or set_device_scale,
//      on a visible or a hidden View, with each provoked (ApplyPendingResizes makes those calls under s_viewsMutex)
//   H  OnPageSettled, what the settle repaint (TakePrepaintView) relies on: a page loaded hidden settles only after a
//      paint, after its prepaint, inside Update; a resize does not settle it again; a fragment change waits for the
//      next paint and then settles again, with no DOM ready
// Usage: visprobe <sdkDir> [cpu|gpu] [gpuDllDir]
//   cpu (default): CPU views, paints read from the BitmapSurface (dirty bounds, a pixel).
//   gpu: GPU views on Magelight's own driver (MagelightGPU.dll from gpuDllDir: the build dir's unpatched copy), paints
//        read from the driver's command flag and a pixel read back from the render target.
// Build and run both: build.bat [out dir]. Exit code = the number of failed checks. Re-run at every SDK bump.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <AppCore/Platform.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/platform/Surface.h>
#include <JavaScriptCore/JavaScript.h>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "../fiber-gc-test/FiberRun.h"
#pragma comment(lib, "d3d11.lib")
#pragma warning(disable : 4996)   // RenderOnly is deprecated in 2.0; probed on purpose

using namespace ultralight;
using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

// ---- GPU backend (Magelight's driver) ------------------------------------------------------------------------------
typedef void (*MgGpuLogFn)(const char*);
typedef void* (*MgCreateFn)(ID3D11Device*, ID3D11DeviceContext*, MgGpuLogFn);
typedef void* (*MgGetDrvFn)(void*);
typedef int (*MgHasFn)(void*);
typedef void (*MgDrawFn)(void*);
typedef void* (*MgSrvFn)(void*, unsigned);
typedef int (*MgSamplesFn)(void*, int);
static bool g_gpu = false;
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static void* g_mg = nullptr;
static MgHasFn mgHas = nullptr;
static MgDrawFn mgDraw = nullptr;
static MgSrvFn mgSrv = nullptr;
static ID3D11Query* g_evq = nullptr;
static void GpuLog(const char* m) { std::printf("  [gpu] %s\n", m); }
static void GpuWait()
{
    g_ctx->End(g_evq);
    BOOL done = FALSE;
    while (g_ctx->GetData(g_evq, &done, sizeof(done), 0) != S_OK || !done) std::this_thread::yield();
}

// ---- the page ------------------------------------------------------------------------------------------------------
static std::string Page(int heavyRows)
{
    std::string rows;
    for (int i = 0; i < heavyRows; ++i)
        rows += "<div class=r><b>Row " + std::to_string(i) +
                "</b> <span>Iron sword of the morning, a heavy blade with a long name</span><i>" + std::to_string(i * 7) +
                "</i></div>";
    return std::string(R"(<!doctype html><html><head><style>
html,body{margin:0;background:transparent;font:14px sans-serif;color:#eee}
#mark{position:absolute;left:0;top:0;width:64px;height:64px;background:rgb(255,0,0)}
#anim{position:absolute;left:100px;top:0;width:40px;height:40px;background:#0f0;animation:mv 1s linear infinite}
@keyframes mv{from{transform:translateX(0)}to{transform:translateX(200px)}}
#tr{position:absolute;left:400px;top:0;width:40px;height:40px;background:#00f;transition:opacity 300ms linear}
.r{margin:2px 8px;padding:4px 8px;border-radius:6px;border:1px solid #556;box-shadow:0 2px 6px rgba(0,0,0,.5);
   background:linear-gradient(90deg,rgba(40,40,60,.9),rgba(80,60,40,.9))}
.r i{float:right;opacity:.7}
</style></head><body><div id=mark></div><div id=anim></div><div id=tr></div><div style="padding-top:80px">)") +
           rows + R"(</div><script>
window.__raf=0;window.__iv=0;window.__to=0;window.__vc=0;window.__vlog=[];window.__trend=0;window.__ld='';
window.__atScript=document.visibilityState;window.__name=location.hash.slice(1);
(function r(){__raf++;requestAnimationFrame(r);})();
setInterval(function(){__iv++;},16);
(function t(){__to++;setTimeout(t,16);})();
document.addEventListener('visibilitychange',function(){__vc++;__vlog.push(document.visibilityState+':'+document.hidden);
  if(window.nativeNote)nativeNote((window.__name||'?')+':'+document.visibilityState);});
window.addEventListener('load',function(){__ld='load:'+document.visibilityState;});
document.getElementById('tr').addEventListener('transitionend',function(){__trend++;});
var mc=new MessageChannel();window.__mcGot=-1;mc.port1.onmessage=function(){__mcGot=performance.now();};
window.mcPing=function(){__mcGot=-1;mc.port2.postMessage(1);return performance.now();};
window.__prGot=-1;window.prPing=function(){__prGot=-1;Promise.resolve().then(function(){__prGot=1;});};
window.__otAt=-1;window.otPing=function(ms){var t0=performance.now();__otAt=-1;setTimeout(function(){__otAt=performance.now()-t0;},ms);};
window.animT=function(){var a=document.getAnimations().filter(function(x){return x.animationName==='mv';})[0];return a?Math.round(a.currentTime):-1;};
</script></body></html>)";
}

// Section F's page: a red mark the probe recolours the ways a consumer feeds a hidden page.
static const char* kFeedPage = R"(<!doctype html><html><head><style>
html,body{margin:0;background:transparent}
#mark{position:absolute;left:0;top:0;width:64px;height:64px;background:rgb(255,0,0)}
</style></head><body><div id=mark></div><script>
window.__raf=0;window.__vc=0;window.__loop=0;window.__vlog=[];
function put(c){document.getElementById('mark').style.background=c;}
window.feedDom=function(c){put(c);};
window.feedRaf=function(c){requestAnimationFrame(function(){__raf++;put(c);});};
window.feedRaf2=function(c){requestAnimationFrame(function(){requestAnimationFrame(function(){__raf++;put(c);});});};
window.feedMcRaf=function(c){var ch=new MessageChannel();
  ch.port1.onmessage=function(){requestAnimationFrame(function(){__raf++;put(c);});};ch.port2.postMessage(0);};
window.loop=function(){(function r(){__loop++;requestAnimationFrame(r);})();};
document.addEventListener('visibilitychange',function(){__vc++;__vlog.push(document.visibilityState);});
</script></body></html>)";

// Section G's page: counts the window's resize and scroll events, a media query's change events and a ResizeObserver
// and an IntersectionObserver's callbacks. The resize handler recolours the fixed box at once (green at 500 px wide or
// more, blue below), so a paint shows whether it ran before it. reads() asks a fresh matchMedia, kept() the list the
// page made at load: a page that keeps one (a useMediaQuery hook) reads that. Two more observers follow the viewport
// alone, for the calls a resize makes with no script: a ResizeObserver on #vw (half the viewport's width) and an
// IntersectionObserver on #edge (fixed at x 440, in view only while the viewport is wider than that).
static const char* kStepsPage = R"(<!doctype html><html><head><style>
html,body{margin:0;background:transparent}
#box{position:fixed;left:0;top:0;width:100px;height:50px;background:rgb(255,0,0)}
#tall{height:3000px}
#io{width:10px;height:10px}
#vw{position:absolute;left:0;top:60px;width:50vw;height:1px}
#edge{position:fixed;left:440px;top:60px;width:10px;height:10px}
</style></head><body><div id=box></div><div id=tall></div><div id=io></div><div id=vw></div><div id=edge></div><script>
window.c={ro:0,io:0,rs:0,mq:0,sc:0,rv:0,iv:0};
var box=document.getElementById('box');
new ResizeObserver(function(){c.ro++;}).observe(box);
new IntersectionObserver(function(){c.io++;}).observe(document.getElementById('io'));
new ResizeObserver(function(){c.rv++;}).observe(document.getElementById('vw'));
new IntersectionObserver(function(){c.iv++;}).observe(document.getElementById('edge'));
window.addEventListener('resize',function(){c.rs++;box.style.background=innerWidth>=500?'rgb(0,255,0)':'rgb(0,0,255)';});
window.wide=matchMedia('(min-width:500px)');wide.addEventListener('change',function(){c.mq++;});
window.addEventListener('scroll',function(){c.sc++;});
window.counts=function(){return c.ro+','+c.io+','+c.rs+','+c.mq+','+c.sc+','+c.rv+','+c.iv;};
window.reads=function(){return innerWidth+','+matchMedia('(min-width:500px)').matches+','+Math.round(box.getBoundingClientRect().width)+','+
  Math.round(scrollY);};
window.kept=function(){return String(wide.matches);};
</script></body></html>)";

// ---- harness -------------------------------------------------------------------------------------------------------
static RefPtr<Renderer> g_r;
static int g_nativeNotes = 0;
static std::string g_lastNote;
static std::string g_noteLog;

static JSValueRef NativeNote(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef argv[], JSValueRef*)
{
    ++g_nativeNotes;
    if (argc > 0) {
        JSStringRef s = JSValueToStringCopy(ctx, argv[0], nullptr);
        char buf[64] = {};
        JSStringGetUTF8CString(s, buf, sizeof buf);
        JSStringRelease(s);
        g_lastNote = buf;
        g_noteLog += std::string(buf) + ' ';
    }
    return JSValueMakeUndefined(ctx);
}

// The Renderer call the pump is in (Frame, Prepaint), for where a listener fires.
static const char* g_phase = "no pump call";

struct Load : LoadListener {
    bool dom = false, finished = false, windowObj = false;
    Clock::time_point domAt{};
    View* bindFor = nullptr;
    void OnWindowObjectReady(View* caller, uint64_t, bool main, const String&) override
    {
        if (!main) return;
        windowObj = true;
        auto scope = caller->LockJSContext();
        JSContextRef ctx = *scope;
        JSStringRef name = JSStringCreateWithUTF8CString("nativeNote");
        JSObjectRef fn = JSObjectMakeFunctionWithCallback(ctx, name, &NativeNote);
        JSObjectSetProperty(ctx, JSContextGetGlobalObject(ctx), name, fn, 0, nullptr);
        JSStringRelease(name);
    }
    void OnDOMReady(View*, uint64_t, bool main, const String&) override { if (main) { dom = true; domAt = Clock::now(); } }
    void OnFinishLoading(View*, uint64_t, bool main, const String&) override { if (main) finished = true; }
    bool settled = false;
    int settles = 0;
    const char* settledIn = nullptr;   // g_phase at the last settle
    void OnPageSettled(View*, const String&) override { settled = true; ++settles; settledIn = g_phase; }
};

struct Probe {
    const char* name;
    RefPtr<View> v;
    Load load;
};

static std::string Js(View* v, const char* s)
{
    String ex;
    String out = v->EvaluateScript(s, &ex);
    std::string r = out.utf8().data();
    if (!ex.empty()) r += " (exc: " + std::string(ex.utf8().data()) + ")";
    return r;
}
static long JsInt(View* v, const char* s) { return std::atol(Js(v, s).c_str()); }

// What the last Render did to a view: CPU = the surface's dirty bounds (cleared here); GPU = the driver had commands.
struct PaintInfo { bool painted = false; int w = 0, h = 0; };
static PaintInfo TakeCpuPaint(View* v)
{
    PaintInfo p;
    if (Surface* s = v->surface()) {
        const IntRect b = s->dirty_bounds();
        if (!b.IsEmpty()) { p.painted = true; p.w = b.width(); p.h = b.height(); }
        s->ClearDirtyBounds();
    }
    return p;
}
// BGRA at (x,y): CPU surface, or the GPU render target read back.
static unsigned Pixel(View* v, int x, int y)
{
    if (!g_gpu) {
        Surface* s = v->surface();
        if (!s) return 0;
        auto* px = static_cast<unsigned char*>(s->LockPixels());
        unsigned c = *reinterpret_cast<unsigned*>(px + y * s->row_bytes() + x * 4);
        s->UnlockPixels();
        return c;
    }
    const RenderTarget rt = v->render_target();
    auto* srv = static_cast<ID3D11ShaderResourceView*>(mgSrv(g_mg, rt.texture_id));
    if (!srv) return 0xDEADBEEF;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    res->QueryInterface(&tex);
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0; sd.MipLevels = 1; sd.ArraySize = 1;
    ID3D11Texture2D* st = nullptr;
    g_dev->CreateTexture2D(&sd, nullptr, &st);
    g_ctx->CopyResource(st, tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    unsigned c = 0xDEADBEEF;
    if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        const int px = static_cast<int>(rt.uv_coords.left * d.Width) + x;
        const int py = static_cast<int>(rt.uv_coords.top * d.Height) + y;
        c = *reinterpret_cast<unsigned*>(static_cast<unsigned char*>(m.pData) + py * m.RowPitch + px * 4);
        g_ctx->Unmap(st, 0);
    }
    st->Release(); tex->Release(); res->Release();
    return c;
}

static std::vector<Probe*> g_all;
struct FrameStat { double renderMs = 0; bool gpuCmds = false; };
// One frame of the Magelight pump: Update, RefreshDisplay, Render, then the driver's command list.
static FrameStat Frame(bool render = true, bool sleep = true)
{
    FrameStat f;
    g_phase = "Update";
    g_r->Update();
    g_phase = "RefreshDisplay";
    g_r->RefreshDisplay(0);
    g_phase = "no pump call";
    if (render) {
        const auto t0 = Clock::now();
        g_phase = "Render";
        g_r->Render();
        g_phase = "no pump call";
        if (g_gpu) {
            f.gpuCmds = mgHas(g_mg) != 0;
            if (f.gpuCmds) mgDraw(g_mg);
            GpuWait();
        }
        f.renderMs = Ms(t0, Clock::now());
    }
    FiberRun::BetweenFrames();
    if (sleep) std::this_thread::sleep_for(std::chrono::milliseconds(16));
    return f;
}

static int g_fail = 0;
static void Check(bool ok, const char* what, const std::string& detail = {})
{
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what, detail.empty() ? "" : "  -> ", detail.c_str());
    if (!ok) ++g_fail;
}
static void Info(const char* what, const std::string& detail) { std::printf("info  %s  -> %s\n", what, detail.c_str()); }
static std::string S(long v) { return std::to_string(v); }
static std::string F(double v) { char b[32]; std::snprintf(b, sizeof b, "%.2f", v); return b; }

static Probe* Make(const char* name, int w, int h, bool hiddenBeforeLoad, bool throttle, int rows)
{
    auto* p = new Probe{ name };
    ViewConfig vc;
    vc.is_accelerated = g_gpu;
    vc.is_transparent = true;
    vc.enable_hidden_timer_throttling = throttle;
    p->v = g_r->CreateView(w, h, vc, nullptr);
    p->v->set_load_listener(&p->load);
    if (hiddenBeforeLoad) p->v->set_visible(false);
    const std::string html = Page(rows);
    p->v->LoadHTML(html.c_str(), (std::string("file:///probe.html#") + name).c_str());
    g_all.push_back(p);
    return p;
}
static Probe* MakeWith(const char* name, int w, int h, const char* html)
{
    auto* p = new Probe{ name };
    ViewConfig vc;
    vc.is_accelerated = g_gpu;
    vc.is_transparent = true;
    p->v = g_r->CreateView(w, h, vc, nullptr);
    p->v->set_load_listener(&p->load);
    p->v->LoadHTML(html, (std::string("file:///feed.html#") + name).c_str());
    g_all.push_back(p);
    return p;
}
static void Settle(Probe* p, int maxFrames = 400)
{
    for (int i = 0; i < maxFrames && !p->load.finished; ++i) Frame();
}

// Pump n frames; returns how many painted this view (CPU dirty bounds / GPU commands while it is the only visible one).
static int PumpCountPaints(Probe* p, int n)
{
    int painted = 0;
    for (int i = 0; i < n; ++i) {
        const FrameStat f = Frame();
        if (g_gpu) { if (f.gpuCmds) ++painted; }
        else if (TakeCpuPaint(p->v.get()).painted) ++painted;
    }
    return painted;
}

static int ToolMain(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: visprobe <sdkDir> [cpu|gpu] [gpuDllDir]\n"); return 2; }
    const std::string sdk = argv[1];
    g_gpu = argc > 2 && std::string(argv[2]) == "gpu";
    Config cfg;
    Platform::instance().set_config(cfg);
    Platform::instance().set_font_loader(GetPlatformFontLoader());
    Platform::instance().set_file_system(GetPlatformFileSystem(String(sdk.c_str())));
    if (g_gpu) {
        D3D_FEATURE_LEVEL fl;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx))) { std::printf("no device\n"); return 3; }
        D3D11_QUERY_DESC qd{ D3D11_QUERY_EVENT, 0 };
        g_dev->CreateQuery(&qd, &g_evq);
        std::string dll = (argc > 3 ? std::string(argv[3]) + "\\" : std::string()) + "MagelightGPU.dll";
        HMODULE m = LoadLibraryA(dll.c_str());
        if (!m) { std::printf("cannot load %s\n", dll.c_str()); return 3; }
        auto mgCreate = (MgCreateFn)GetProcAddress(m, "MgGpu_Create");
        auto mgDrv = (MgGetDrvFn)GetProcAddress(m, "MgGpu_GetGPUDriver");
        mgHas = (MgHasFn)GetProcAddress(m, "MgGpu_HasCommandsPending");
        mgDraw = (MgDrawFn)GetProcAddress(m, "MgGpu_DrawCommandList");
        mgSrv = (MgSrvFn)GetProcAddress(m, "MgGpu_GetTextureSRV");
        g_mg = mgCreate(g_dev, g_ctx, &GpuLog);
        if (auto s = (MgSamplesFn)GetProcAddress(m, "MgGpu_SetSampleCount")) std::printf("MSAA %dx\n", s(g_mg, 4));
        Platform::instance().set_gpu_driver(static_cast<GPUDriver*>(mgDrv(g_mg)));
    }
    g_r = Renderer::Create();
    std::printf("mode %s, Ultralight %s\n", g_gpu ? "gpu (MagelightGPU.dll)" : "cpu", ULTRALIGHT_VERSION);

    const int W = 1600, H = 900, ROWS = 400;

    // ── A: a visible page, then hidden, then shown ─────────────────────────────────────────────────────────────
    std::printf("\n== A: visible -> set_visible(false) -> set_visible(true)\n");
    Probe* a = Make("A", W, H, false, false, ROWS);
    // First paint cost: the frame that paints the loaded page.
    double firstPaint = 0;
    for (int i = 0; i < 400 && !a->load.finished; ++i) { const FrameStat f = Frame(); if (f.renderMs > firstPaint) firstPaint = f.renderMs; }
    if (!g_gpu) TakeCpuPaint(a->v.get());
    for (int i = 0; i < 60; ++i) Frame();
    if (!g_gpu) TakeCpuPaint(a->v.get());
    Info("A worst Render while loading (ms)", F(firstPaint));
    Check(Js(a->v.get(), "__atScript") == "visible", "A: a default View loads visible", Js(a->v.get(), "__atScript"));
    // Static view, forced repaint, for the cost of a repaint with nothing changed. Stop the animation first so the
    // only paint is the forced one.
    Js(a->v.get(), "document.getElementById('anim').style.animationPlayState='paused'");
    for (int i = 0; i < 10; ++i) Frame();
    if (!g_gpu) TakeCpuPaint(a->v.get());
    Check(!a->v->needs_paint(), "A: static visible view: needs_paint false", S(a->v->needs_paint()));
    a->v->set_needs_paint(true);
    {
        const FrameStat f = Frame();
        const PaintInfo pi = g_gpu ? PaintInfo{ f.gpuCmds } : TakeCpuPaint(a->v.get());
        Info("A forced repaint (set_needs_paint) of a static visible view", "painted=" + S(pi.painted) + " area=" + S(pi.w) + "x" + S(pi.h) +
             " render=" + F(f.renderMs) + " ms");
    }
    Js(a->v.get(), "document.getElementById('anim').style.animationPlayState='running'");
    for (int i = 0; i < 30; ++i) Frame();
    if (!g_gpu) TakeCpuPaint(a->v.get());

    auto framesUntil = [&](View* v, const char* kick, const char* test) {
        Js(v, kick);
        for (int i = 1; i <= 30; ++i) { Frame(); if (Js(v, test) == "true") return i; }
        return -1;
    };
    // Where does the page's visibilitychange handler run? Count the native notes after each call of one frame.
    auto stepNotes = [&](View* v, bool show, const char* tag) {
        g_nativeNotes = 0; g_noteLog.clear();
        v->set_visible(show);
        const int inCall = g_nativeNotes;
        const std::string st = Js(v, "document.visibilityState+','+document.hidden");
        const int afterEval = g_nativeNotes;
        g_r->Update();
        const int afterUpdate = g_nativeNotes;
        g_r->RefreshDisplay(0);
        const int afterRefresh = g_nativeNotes;
        g_r->Render();
        if (g_gpu) { if (mgHas(g_mg)) mgDraw(g_mg); GpuWait(); }
        if (!g_gpu) TakeCpuPaint(v);
        const int afterRender = g_nativeNotes;
        Info(tag, std::string("visibilityState right after the call=") + st + "; handler runs counted: in set_visible=" + S(inCall) +
             " +EvaluateScript=" + S(afterEval) + " +Update=" + S(afterUpdate) + " +RefreshDisplay=" + S(afterRefresh) +
             " +Render=" + S(afterRender) + " [" + g_noteLog + "]");
        return inCall;
    };
    Info("A MessageChannel while VISIBLE: frames until delivered", S(framesUntil(a->v.get(), "mcPing()", "__mcGot>0")));
    {   // Which call runs a VISIBLE page's rAF callbacks (page JS inside that call)?
        std::string row;
        for (int k = 0; k < 4; ++k) {
            const long r0 = JsInt(a->v.get(), "__raf");
            g_r->Update();
            const long r1 = JsInt(a->v.get(), "__raf");
            g_r->RefreshDisplay(0);
            const long r2 = JsInt(a->v.get(), "__raf");
            g_r->Render();
            if (g_gpu) { if (mgHas(g_mg)) mgDraw(g_mg); GpuWait(); }
            const long r3 = JsInt(a->v.get(), "__raf");
            row += "U+" + S(r1 - r0) + " R+" + S(r2 - r1) + " Rn+" + S(r3 - r2) + "; ";
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        Info("A visible rAF callbacks by call (Update / RefreshDisplay / Render), 4 frames", row);
    }
    if (!g_gpu) TakeCpuPaint(a->v.get());
    const long raf0 = JsInt(a->v.get(), "__raf"), iv0 = JsInt(a->v.get(), "__iv"), to0 = JsInt(a->v.get(), "__to");
    const long an0 = JsInt(a->v.get(), "animT()");
    const int notesInCall = stepNotes(a->v.get(), false, "A set_visible(false)");
    Check(notesInCall == 0, "A: set_visible(false) runs no page handler inside the call (visibilitychange is queued)", S(notesInCall));
    Check(!a->v->is_visible(), "A: is_visible() false at once");
    Check(!a->v->needs_paint(), "A: needs_paint() false while hidden");
    Check(Js(a->v.get(), "document.visibilityState+','+document.hidden") == "hidden,true", "A: document.visibilityState hidden at once",
          Js(a->v.get(), "document.visibilityState+','+document.hidden"));
    const auto hideT0 = Clock::now();
    const int hiddenPaints = PumpCountPaints(a, 120);
    const double hiddenMs = Ms(hideT0, Clock::now());
    const long raf1 = JsInt(a->v.get(), "__raf"), iv1 = JsInt(a->v.get(), "__iv"), to1 = JsInt(a->v.get(), "__to");
    const long an1 = JsInt(a->v.get(), "animT()");
    Check(hiddenPaints == 0, "A: Render() paints nothing for a hidden view (120 frames)", S(hiddenPaints));
    Check(raf1 == raf0, "A: requestAnimationFrame stops while hidden", S(raf0) + " -> " + S(raf1));
    Check(an1 - an0 < 50, "A: CSS animation does not advance while hidden (currentTime read)", S(an0) + " -> " + S(an1));
    Check(iv1 - iv0 > 60, "A: setInterval(16) keeps running hidden (no throttling)", S(iv1 - iv0) + " ticks in " + F(hiddenMs) + " ms");
    Check(to1 - to0 > 60, "A: chained setTimeout(16) keeps running hidden", S(to1 - to0) + " ticks");
    // Work a consumer does to a hidden page.
    {
        const int mcf = framesUntil(a->v.get(), "mcPing()", "__mcGot>0");
        Check(mcf > 0, "A: MessageChannel delivered while hidden (React's scheduler)", "frames=" + S(mcf));
    }
    Check(framesUntil(a->v.get(), "prPing()", "__prGot==1") == 1, "A: promise microtask runs by the next Update while hidden");
    Js(a->v.get(), "otPing(300)");
    for (int i = 0; i < 40; ++i) Frame();
    Info("A one-shot setTimeout(300) while hidden fired after (ms)", Js(a->v.get(), "__otAt"));
    Js(a->v.get(), "document.getElementById('tr').style.opacity='0.2'");
    for (int i = 0; i < 40; ++i) Frame();
    Info("A transitionend while hidden (300 ms transition, 640 ms pumped)", Js(a->v.get(), "__trend"));
    // A DOM change while hidden (the consumer pushes data): no paint, needs_paint false.
    Js(a->v.get(), "document.getElementById('mark').style.background='rgb(0,0,255)'");
    {
        int p = PumpCountPaints(a, 5);
        Check(p == 0 && !a->v->needs_paint(), "A: DOM change while hidden: no paint, needs_paint false", "paints=" + S(p));
    }
    a->v->set_needs_paint(true);
    Check(!a->v->needs_paint(), "A: set_needs_paint(true) on a hidden view reads back false", S(a->v->needs_paint()));
    {
        int p = PumpCountPaints(a, 2);
        Check(p == 0, "A: ... and Render() still paints nothing", S(p));
    }
    // The deprecated RenderOnly on a hidden view.
    {
        Frame(false, false);
        View* arr[1] = { a->v.get() };
        const auto t0 = Clock::now();
        g_r->RenderOnly(arr, 1);
        bool cmds = false;
        if (g_gpu) { cmds = mgHas(g_mg) != 0; if (cmds) mgDraw(g_mg); GpuWait(); }
        const PaintInfo pi = g_gpu ? PaintInfo{ cmds } : TakeCpuPaint(a->v.get());
        Info("A RenderOnly({hidden view})", "painted=" + S(pi.painted) + " in " + F(Ms(t0, Clock::now())) + " ms; pixel(mark)=" +
             [&] { char b[16]; std::snprintf(b, sizeof b, "%08X", Pixel(a->v.get(), 10, 10)); return std::string(b); }());
    }
    // Show again: one frame, stepped, for where the handler runs and what the frame paints.
    {
        const long anBefore = JsInt(a->v.get(), "animT()");
        g_nativeNotes = 0; g_noteLog.clear();
        a->v->set_visible(true);
        const int inCall = g_nativeNotes;
        const bool np = a->v->needs_paint();
        g_r->Update();
        const int afterUpdate = g_nativeNotes;
        g_r->RefreshDisplay(0);
        const int afterRefresh = g_nativeNotes;
        const auto t0 = Clock::now();
        g_r->Render();
        bool cmds = false;
        if (g_gpu) { cmds = mgHas(g_mg) != 0; if (cmds) mgDraw(g_mg); GpuWait(); }
        const double ms = Ms(t0, Clock::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        Info("A set_visible(true)", "handler runs: in set_visible=" + S(inCall) + " +Update=" + S(afterUpdate) + " +RefreshDisplay=" +
             S(afterRefresh) + " +Render=" + S(g_nativeNotes) + " [" + g_noteLog + "]");
        Check(inCall == 0, "A: set_visible(true) runs no page handler inside the call", S(inCall));
        Check(np, "A: needs_paint() true right after set_visible(true)");
        const PaintInfo pi = g_gpu ? PaintInfo{ cmds } : TakeCpuPaint(a->v.get());
        const unsigned px = Pixel(a->v.get(), 10, 10);
        char b[16]; std::snprintf(b, sizeof b, "%08X", px);
        Check(pi.painted, "A: the first frame after show paints", "area=" + S(pi.w) + "x" + S(pi.h) + " render=" + F(ms) + " ms");
        Check((px & 0x00FFFFFF) == 0x0000FF, "A: ... and shows the DOM change made while hidden (mark blue, BGRA FF0000FF)", b);
        Info("A CSS animation time across the hide (before show -> 1st frame after)", S(anBefore) + " -> " + Js(a->v.get(), "animT()"));
    }
    {
        const long r0 = JsInt(a->v.get(), "__raf");
        for (int i = 0; i < 10; ++i) Frame();
        Check(JsInt(a->v.get(), "__raf") > r0, "A: rAF resumes after show", S(r0) + " -> " + Js(a->v.get(), "__raf"));
        Info("A transitionend after show", Js(a->v.get(), "__trend"));
    }
    // A hide/show with NOTHING changed: what the show costs.
    Js(a->v.get(), "document.getElementById('anim').style.animationPlayState='paused'");
    for (int i = 0; i < 10; ++i) Frame();
    if (!g_gpu) TakeCpuPaint(a->v.get());
    a->v->set_visible(false);
    for (int i = 0; i < 10; ++i) Frame();
    a->v->set_visible(true);
    {
        const FrameStat f = Frame();
        const PaintInfo pi = g_gpu ? PaintInfo{ f.gpuCmds } : TakeCpuPaint(a->v.get());
        char pb[16]; std::snprintf(pb, sizeof pb, "%08X", Pixel(a->v.get(), 10, 10));
        Info("A show after an unchanged hide", "painted=" + S(pi.painted) + " area=" + S(pi.w) + "x" + S(pi.h) + " render=" + F(f.renderMs) + " ms, pixel(mark) " + pb);
    }
    Info("A visibilitychange log", Js(a->v.get(), "JSON.stringify(__vlog)"));
    a->v->set_visible(false);

    // ── B: loaded hidden (set_visible(false) before LoadHTML), then prepainted, then shown ────────────────────────
    std::printf("\n== B: hidden before its load -> prepaint (show, Render, hide) -> show\n");
    for (int i = 0; i < 5; ++i) Frame();   // A's queued hide event lands first
    g_nativeNotes = 0; g_noteLog.clear();
    Probe* b = Make("B", W, H, true, false, ROWS);
    Settle(b);
    Check(b->load.dom && b->load.finished, "B: a View hidden before LoadHTML reaches DOMReady and FinishLoading",
          "dom=" + S(b->load.dom) + " fin=" + S(b->load.finished));
    Check(Js(b->v.get(), "__atScript") == "hidden" && Js(b->v.get(), "__ld") == "load:hidden",
          "B: the page sees visibilityState hidden from its first script; load fires", Js(b->v.get(), "__atScript") + " / " + Js(b->v.get(), "__ld"));
    Check(g_noteLog.find("B:") == std::string::npos, "B: no visibilitychange for a page that loads hidden", "[" + g_noteLog + "]");
    {
        for (int i = 0; i < 60; ++i) Frame();
        Check(JsInt(b->v.get(), "__raf") == 1, "B: rAF never ran while loaded hidden (1 = the first synchronous call)", Js(b->v.get(), "__raf"));
        Check(JsInt(b->v.get(), "__iv") > 30, "B: its timers run", Js(b->v.get(), "__iv"));
        const unsigned px = Pixel(b->v.get(), 10, 10);
        char bb[16]; std::snprintf(bb, sizeof bb, "%08X", px);
        Info("B pixel(mark) before any paint (0 = never painted)", bb);
        Info("B OnPageSettled after load + 60 hidden frames", S(b->load.settled));
    }
    // Prepaint the way the host does it: shown between Update and RefreshDisplay, Render, hidden - in one frame.
    {
        g_nativeNotes = 0; g_noteLog.clear();
        const long raf0b = JsInt(b->v.get(), "__raf");
        g_r->Update();
        b->v->set_visible(true);
        g_r->RefreshDisplay(0);
        const long rafShown = JsInt(b->v.get(), "__raf");
        const auto t0 = Clock::now();
        g_r->Render();
        bool cmds = false;
        if (g_gpu) { cmds = mgHas(g_mg) != 0; if (cmds) mgDraw(g_mg); GpuWait(); }
        const double ms = Ms(t0, Clock::now());
        const long rafRendered = JsInt(b->v.get(), "__raf");
        b->v->set_visible(false);
        Info("B rAF across the prepaint: before / after set_visible(true) + RefreshDisplay / after Render", S(raf0b) + " / " + S(rafShown) + " / " + S(rafRendered));
        const PaintInfo pi = g_gpu ? PaintInfo{ cmds } : TakeCpuPaint(b->v.get());
        const unsigned px = Pixel(b->v.get(), 10, 10);
        char bb[16]; std::snprintf(bb, sizeof bb, "%08X", px);
        Check(pi.painted && (px & 0x00FFFFFF) == 0xFF0000, "B: show + Render + hide in one frame paints the hidden page",
              "painted=" + S(pi.painted) + " area=" + S(pi.w) + "x" + S(pi.h) + " " + F(ms) + " ms, pixel " + bb);
        Info("B page events of that prepaint", "native notes=" + S(g_nativeNotes) + " vlog=" + Js(b->v.get(), "JSON.stringify(__vlog)") +
             " raf " + S(raf0b) + " -> " + Js(b->v.get(), "__raf"));
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    {
        const int p = PumpCountPaints(b, 60);
        const unsigned px = Pixel(b->v.get(), 10, 10);
        char bb[16]; std::snprintf(bb, sizeof bb, "%08X", px);
        Check(p == 0 && (px & 0x00FFFFFF) == 0xFF0000, "B: hidden after the prepaint: no paints, the painted pixels stay", S(p) + " paints, pixel " + bb);
        Info("B OnPageSettled after the prepaint + 60 hidden frames", S(b->load.settled));
        Info("B raf / vlog / native notes after 60 hidden frames", Js(b->v.get(), "__raf") + " / " + Js(b->v.get(), "JSON.stringify(__vlog)") + " / [" + g_noteLog + "]");
    }
    // The real first show after a prepaint: what does it paint?
    {
        b->v->set_visible(true);
        const FrameStat f = Frame();
        const PaintInfo pi = g_gpu ? PaintInfo{ f.gpuCmds } : TakeCpuPaint(b->v.get());
        Info("B first show after the prepaint", "painted=" + S(pi.painted) + " area=" + S(pi.w) + "x" + S(pi.h) + " render=" + F(f.renderMs) + " ms");
        for (int i = 0; i < 30; ++i) Frame();
        Info("B OnPageSettled after 30 shown frames (the page animates)", S(b->load.settled));
        b->v->set_visible(false);
        Frame();
    }

    // ── C: loaded hidden, NO prepaint, first show (the cost the prepaint moves) ─────────────────────────────────
    std::printf("\n== C: hidden before its load, no prepaint -> first show\n");
    Probe* c = Make("C", W, H, true, false, ROWS);
    Settle(c);
    for (int i = 0; i < 30; ++i) Frame();
    {
        c->v->set_visible(true);
        const FrameStat f = Frame();
        const PaintInfo pi = g_gpu ? PaintInfo{ f.gpuCmds } : TakeCpuPaint(c->v.get());
        Info("C first show of a never-painted page", "painted=" + S(pi.painted) + " area=" + S(pi.w) + "x" + S(pi.h) + " render=" + F(f.renderMs) + " ms");
        const FrameStat f2 = Frame();
        Info("C second frame", "render=" + F(f2.renderMs) + " ms");
        c->v->set_visible(false);
        Frame();
    }

    // ── C2: a hidden View navigated again (ReloadView / NavigateView on a hidden view) ──────────────────────────
    {
        c->load.dom = c->load.finished = false;
        g_noteLog.clear();
        const std::string html = Page(20);
        if (!g_gpu) TakeCpuPaint(c->v.get());
        c->v->LoadHTML(html.c_str(), "file:///probe-next.html#C2");
        Settle(c);
        for (int i = 0; i < 30; ++i) Frame();
        Check(c->load.dom && Js(c->v.get(), "__atScript") == "hidden" && JsInt(c->v.get(), "__raf") == 1,
              "C2: a hidden View's next document loads hidden (DOMReady, visibilityState hidden, no rAF)",
              "dom=" + S(c->load.dom) + " " + Js(c->v.get(), "__atScript") + " raf=" + Js(c->v.get(), "__raf") + " [" + g_noteLog + "]");
        const int p = PumpCountPaints(c, 10);
        Check(p == 0, "C2: ... and paints nothing", S(p));
    }

    // ── D: enable_hidden_timer_throttling ──────────────────────────────────────────────────────────────────────
    std::printf("\n== D: ViewConfig::enable_hidden_timer_throttling = true\n");
    Probe* d = Make("D", 400, 300, false, true, 4);
    Settle(d);
    for (int i = 0; i < 30; ++i) Frame();
    d->v->set_visible(false);
    {
        const long iv0d = JsInt(d->v.get(), "__iv"), to0d = JsInt(d->v.get(), "__to");
        const auto t0 = Clock::now();
        for (int i = 0; i < 120; ++i) Frame();
        const double ms = Ms(t0, Clock::now());
        Info("D setInterval(16) ticks while hidden, throttled", S(JsInt(d->v.get(), "__iv") - iv0d) + " in " + F(ms) + " ms");
        Info("D chained setTimeout(16) ticks while hidden, throttled", S(JsInt(d->v.get(), "__to") - to0d) + " in " + F(ms) + " ms");
        Js(d->v.get(), "otPing(300)");
        for (int i = 0; i < 90; ++i) Frame();
        Info("D one-shot setTimeout(300) while hidden, throttled, fired after (ms)", Js(d->v.get(), "__otAt"));
        Js(d->v.get(), "otPing(0)");
        for (int i = 0; i < 90; ++i) Frame();
        Info("D one-shot setTimeout(0) while hidden, throttled, fired after (ms)", Js(d->v.get(), "__otAt"));
        const int mcf = framesUntil(d->v.get(), "mcPing()", "__mcGot>0");
        Check(mcf > 0 && mcf <= 3, "D: MessageChannel still delivered within a few frames, throttled", "frames=" + S(mcf));
    }
    d->v->set_visible(true);
    {
        const long iv0d = JsInt(d->v.get(), "__iv");
        for (int i = 0; i < 60; ++i) Frame();
        Info("D setInterval(16) ticks after show (60 frames)", S(JsInt(d->v.get(), "__iv") - iv0d));
    }

    // ── E: the render pass with every view hidden ──────────────────────────────────────────────────────────────
    std::printf("\n== E: Render() with every view hidden\n");
    d->v->set_visible(false);
    {
        double worst = 0;
        int cmds = 0;
        for (int i = 0; i < 60; ++i) { const FrameStat f = Frame(); worst = std::max(worst, f.renderMs); cmds += f.gpuCmds; }
        Info("E Render() with 4 hidden views: worst ms", F(worst));
        // FrameWork's idle Render (nothing shown, once a second, so released Views free their GPU resources).
        Check(cmds == 0, "E: Render() with every view hidden sends no command list (60 frames)", S(cmds));
    }

    // ── F: the host's frame order ──────────────────────────────────────────────────────────────────────────────
    // Where in the frame a show lands: before Update (early), between Update and RefreshDisplay (mid) or between
    // RefreshDisplay and Render (late). Each case: the mark is red and painted, the view is hidden, the page is fed
    // a blue mark, then the view is shown at one of the three places; the pixel is read after that frame's Render
    // and after the next frame.
    std::printf("\n== F: the host's frame: a show before Update (early), before RefreshDisplay (mid) or before Render (late)\n");
    Probe* fp = MakeWith("F", 400, 300, kFeedPage);
    Settle(fp);
    for (int i = 0; i < 10; ++i) Frame();
    auto renderNow = [&] {
        if (!g_gpu) TakeCpuPaint(fp->v.get());   // only this Render's paint counts
        g_r->Render();
        bool cmds = false;
        if (g_gpu) { cmds = mgHas(g_mg) != 0; if (cmds) mgDraw(g_mg); GpuWait(); }
        return g_gpu ? cmds : TakeCpuPaint(fp->v.get()).painted;
    };
    auto mark = [&] { return Pixel(fp->v.get(), 10, 10) & 0x00FFFFFF; };
    auto hex = [](unsigned c) { char b[16]; std::snprintf(b, sizeof b, "%06X", c); return std::string(b); };
    enum class At { Early, Mid, Late };
    // One frame with `show` applied at `at`: Update, RefreshDisplay, Render. Returns whether that Render painted.
    auto frameWith = [&](At at, bool show) {
        if (at == At::Early) fp->v->set_visible(show);
        g_r->Update();
        if (at == At::Mid) fp->v->set_visible(show);
        g_r->RefreshDisplay(0);
        if (at == At::Late) fp->v->set_visible(show);
        return renderNow();
    };
    auto resetRed = [&] {
        fp->v->set_visible(true);
        for (int i = 0; i < 4; ++i) Frame();   // any rAF a previous case left runs out first
        Js(fp->v.get(), "put('rgb(255,0,0)')");
        for (int i = 0; i < 4; ++i) Frame();
        return mark() == 0xFF0000;
    };
    struct Shown { bool first = false, second = false; };
    auto feedCase = [&](const char* tag, const char* feed, At at, int hiddenFrames) {
        const bool redBefore = resetRed();
        fp->v->set_visible(false);
        for (int i = 0; i < 4; ++i) Frame();
        const long raf0 = JsInt(fp->v.get(), "__raf"), vc0 = JsInt(fp->v.get(), "__vc");
        Js(fp->v.get(), (std::string(feed) + "('rgb(0,0,255)')").c_str());
        for (int i = 0; i < hiddenFrames; ++i) Frame();
        const long rafHidden = JsInt(fp->v.get(), "__raf");
        const bool painted = frameWith(at, true);
        Shown r;
        const unsigned m1 = mark();
        r.first = m1 == 0x0000FF;
        const long rafFirst = JsInt(fp->v.get(), "__raf"), vcFirst = JsInt(fp->v.get(), "__vc");
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        Frame();
        const unsigned m2 = mark();
        r.second = m2 == 0x0000FF;
        Info(tag, std::string("red before=") + S(redBefore) + "; rAF callbacks while hidden=" + S(rafHidden - raf0) +
             ", in the show's frame=" + S(rafFirst - rafHidden) + "; show frame painted=" + S(painted) + " mark=" +
             hex(m1) + ", next frame mark=" + hex(m2) + "; visibilitychange by the show's Render=" + S(vcFirst - vc0) +
             ", by the next frame=" + S(JsInt(fp->v.get(), "__vc") - vc0));
        return r;
    };
    const char* atName[] = { "early", "mid", "late" };
    Shown dom[3], domSame[3], raf[3], rafSame[3], mc[3], raf2[3];
    for (int i = 0; i < 3; ++i) {
        const At at = static_cast<At>(i);
        auto tag = [&](const char* what) { return std::string("F ") + what + ", " + atName[i] + " show"; };
        dom[i] = feedCase(tag("DOM write fed while hidden").c_str(), "feedDom", at, 10);
        domSame[i] = feedCase(tag("DOM write fed in the show's frame (a bridge drain)").c_str(), "feedDom", at, 0);
        raf[i] = feedCase(tag("rAF-deferred write fed while hidden").c_str(), "feedRaf", at, 10);
        rafSame[i] = feedCase(tag("rAF-deferred write fed in the show's frame").c_str(), "feedRaf", at, 0);
        mc[i] = feedCase(tag("MessageChannel then rAF fed while hidden").c_str(), "feedMcRaf", at, 10);
        raf2[i] = feedCase(tag("two-deep rAF fed while hidden").c_str(), "feedRaf2", at, 10);
    }
    for (int i = 0; i < 3; ++i)
        Check(dom[i].first && domSame[i].first,
              (std::string("F: a DOM write made while hidden is on screen in the show's own frame, ") + atName[i] + " show").c_str());
    for (int i = 0; i < 2; ++i)
        Check(raf[i].first && rafSame[i].first && mc[i].first,
              (std::string("F: a write deferred to requestAnimationFrame while hidden runs in the show's Render and is "
                           "painted by it, ") + atName[i] + " show").c_str());
    Check(!raf[2].first && !rafSame[2].first && !mc[2].first,
          "F: a show after RefreshDisplay runs no rAF in its Render: the old texture is on screen for a frame or more "
          "(why the host syncs before RefreshDisplay)");
    for (int i = 0; i < 2; ++i)
        Check(!raf2[i].first && raf2[i].second,
              (std::string("F: a write two rAF deep lands one frame after the show (as in a browser), ") + atName[i] + " show").c_str());
    // A prepaint: show, Render, hide in one frame, of a page fed through rAF while hidden. Is the rAF-deferred write
    // in the painted texture?
    for (int i = 1; i < 3; ++i) {
        const At at = static_cast<At>(i);
        resetRed();
        fp->v->set_visible(false);
        for (int k = 0; k < 4; ++k) Frame();
        Js(fp->v.get(), "feedRaf('rgb(0,0,255)')");
        for (int k = 0; k < 10; ++k) Frame();
        const long rafP0 = JsInt(fp->v.get(), "__raf");
        Js(fp->v.get(), "__vlog=[]");
        frameWith(at, true);
        fp->v->set_visible(false);
        const unsigned m = mark();
        for (int k = 0; k < 10; ++k) Frame();
        Info((std::string("F prepaint of a page fed through rAF while hidden, shown ") + atName[i]).c_str(),
             "rAF callbacks in the prepaint's frame=" + S(JsInt(fp->v.get(), "__raf") - rafP0) + ", painted mark=" + hex(m) +
             ", after 10 hidden frames=" + hex(mark()) + ", visibilitychange " + Js(fp->v.get(), "JSON.stringify(__vlog)"));
    }
    // A hide just before RefreshDisplay or Render: that Render must paint nothing for the view and run none of its rAF.
    Js(fp->v.get(), "loop()");
    for (int i = 1; i < 3; ++i) {
        const At at = static_cast<At>(i);
        const bool redBefore = resetRed();
        Js(fp->v.get(), "put('rgb(0,255,0)')");
        const long loop0 = JsInt(fp->v.get(), "__loop"), vc0 = JsInt(fp->v.get(), "__vc");
        const bool painted = frameWith(at, false);
        const long loop1 = JsInt(fp->v.get(), "__loop");
        const unsigned after = mark();
        Check(redBefore && !painted && loop1 == loop0 && after == 0xFF0000,
              (std::string("F: a hide ") + atName[i] + " in the frame: that Render paints nothing and runs no rAF for the view").c_str(),
              "red before=" + S(redBefore) + ", mark after=" + hex(after) + ", painted=" + S(painted) + ", rAF " + S(loop0) + " -> " + S(loop1));
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        Frame();
        Info((std::string("F visibilitychange of that hide by the next frame, ") + atName[i]).c_str(), S(JsInt(fp->v.get(), "__vc") - vc0));
    }
    fp->v->set_visible(false);

    // ── G: the other rendering steps ───────────────────────────────────────────────────────────────────────────
    // Each change below moves all five: the window's width (resize, and the media query across 500 px), the box's width
    // (ResizeObserver) and the scroll position (scroll, and the IntersectionObserver target at the bottom in or out).
    std::printf("\n== G: resize, scroll, matchMedia change, ResizeObserver and IntersectionObserver on a hidden page\n");
    Probe* gp = MakeWith("G", 400, 300, kStepsPage);
    Settle(gp);
    for (int i = 0; i < 10; ++i) Frame();
    View* gv = gp->v.get();
    // rv / iv: the viewport-following observers (#vw, #edge); `once` leaves them out, as a change need not move them.
    struct Steps { long ro = 0, io = 0, rs = 0, mq = 0, sc = 0, rv = 0, iv = 0; };
    auto steps = [&] {
        Steps s;
        std::sscanf(Js(gv, "counts()").c_str(), "%ld,%ld,%ld,%ld,%ld,%ld,%ld", &s.ro, &s.io, &s.rs, &s.mq, &s.sc, &s.rv, &s.iv);
        return s;
    };
    auto delta = [](const Steps& a, const Steps& b) {
        return Steps{ b.ro - a.ro, b.io - a.io, b.rs - a.rs, b.mq - a.mq, b.sc - a.sc, b.rv - a.rv, b.iv - a.iv };
    };
    auto str = [](const Steps& d) {
        return "ResizeObserver " + S(d.ro) + ", IntersectionObserver " + S(d.io) + ", resize " + S(d.rs) + ", matchMedia " +
               S(d.mq) + ", scroll " + S(d.sc) + ", viewport ResizeObserver " + S(d.rv) + ", viewport IntersectionObserver " +
               S(d.iv);
    };
    auto none = [](const Steps& d) {
        return d.ro == 0 && d.io == 0 && d.rs == 0 && d.mq == 0 && d.sc == 0 && d.rv == 0 && d.iv == 0;
    };
    auto once = [](const Steps& d) { return d.ro == 1 && d.io == 1 && d.rs == 1 && d.mq == 1 && d.sc == 1; };
    auto change = [&](int w, int boxW, int scroll) {
        gv->Resize(w, 300);
        Js(gv, ("document.getElementById('box').style.width='" + S(boxW) + "px';window.scrollTo(0," + S(scroll) + ")").c_str());
    };
    auto renderG = [&] {
        g_r->Render();
        if (g_gpu) { if (mgHas(g_mg)) mgDraw(g_mg); GpuWait(); }
    };
    auto box = [&] { return Pixel(gv, 10, 10) & 0x00FFFFFF; };
    {   // ApplyPendingResizes calls Resize under s_viewsMutex, so no page script may run inside it (invariant 16). The
        // calls provoke what a resize can: scrolled to the bottom first, the last one makes the viewport taller than
        // the page, so scrollY clamps to 0 (scroll), #vw follows the width (viewport ResizeObserver) and #edge comes
        // into view at 450 px (viewport IntersectionObserver); the frames after must run each, or a zero in the calls
        // proves nothing. Ends at 450x300, scale 1, at the top, below the media query's 500 px, as the control expects.
        Js(gv, "window.scrollTo(0,3000)");
        for (int i = 0; i < 10; ++i) Frame();
        const Steps r0 = steps();
        gv->Resize(500, 300);
        gv->Resize(400, 300, 1.25);
        gv->set_device_scale(1.0);
        gv->Resize(450, 3100, 1.0);
        const Steps r1 = steps();
        for (int i = 0; i < 10; ++i) Frame();
        const Steps r2 = steps();
        const Steps after = delta(r1, r2);
        Check(none(delta(r0, r1)) && after.rs > 0 && after.sc > 0 && after.rv > 0 && after.iv > 0,
              "G: Resize(w, h), Resize(w, h, scale) and set_device_scale run no page script inside the call; the frames "
              "after run the resize, scroll (a clamp) and both viewport observers' handlers",
              "in the calls: " + str(delta(r0, r1)) + "; the next 10 frames: " + str(after));
        gv->Resize(450, 300);
        for (int i = 0; i < 10; ++i) Frame();
    }
    {   // The control: visible, one change, stepped through one frame.
        const Steps s0 = steps();
        change(600, 200, 3000);
        const Steps sCall = steps();
        g_r->Update();
        const Steps sU = steps();
        g_r->RefreshDisplay(0);
        const Steps sR = steps();
        renderG();
        const Steps sN = steps();
        for (int i = 0; i < 10; ++i) Frame();
        const Steps s1 = steps();
        Info("G visible: run by Resize + the script / Update / RefreshDisplay / Render",
             str(delta(s0, sCall)) + " / " + str(delta(sCall, sU)) + " / " + str(delta(sU, sR)) + " / " + str(delta(sR, sN)));
        const Steps dv = delta(s0, s1);
        Check(dv.ro > 0 && dv.io > 0 && dv.rs > 0 && dv.mq > 0 && dv.sc > 0, "G: visible, a change runs all five", str(dv));
    }
    {   // Hidden, two changes, then shown where the host's first sync shows (before Update).
        gv->set_visible(false);
        for (int i = 0; i < 5; ++i) Frame();
        const Steps h0 = steps();
        // ApplyPendingResizes mostly resizes hidden views: the same calls on one (what they hold is in the show below).
        gv->Resize(500, 300);
        gv->Resize(400, 300, 1.25);
        gv->set_device_scale(1.0);
        gv->Resize(450, 300, 1.0);
        const Steps hc = steps();
        Check(none(delta(h0, hc)), "G: hidden, the same calls run no page script inside the call either", str(delta(h0, hc)));
        change(700, 300, 1500);
        for (int i = 0; i < 10; ++i) Frame();
        change(400, 100, 0);
        for (int i = 0; i < 10; ++i) Frame();
        const std::string keptH = Js(gv, "kept()");
        const std::string read = Js(gv, "reads()");
        const Steps h1 = steps();
        Check(none(delta(h0, h1)), "G: hidden, none of them runs (the calls, two changes, 20 frames)", str(delta(h0, h1)));
        Check(read == "400,false,100,0", "G: hidden, what a script reads itself is current (innerWidth, a fresh "
              "matchMedia(q).matches across 500 px, a box's size, scrollY)", read);
        gv->set_visible(true);
        g_r->Update();
        g_r->RefreshDisplay(0);
        const Steps hR = steps();
        const std::string keptR = Js(gv, "kept()");
        renderG();
        const Steps hN = steps();
        const unsigned px = box();
        const std::string keptN = Js(gv, "kept()");
        Check(keptH == "true" && keptR == "true" && keptN == "false",
              "G: a kept MediaQueryList's matches keeps its last reported value until its change event, in the show's Render",
              "hidden: " + keptH + "; shown, before Render: " + keptR + "; after: " + keptN);
        for (int i = 0; i < 10; ++i) Frame();
        const Steps h2 = steps();
        Check(none(delta(h0, hR)) && once(delta(h0, hN)), "G: shown, each runs once, coalesced, inside the show's Render",
              "before Render: " + str(delta(h0, hR)) + "; after: " + str(delta(h0, hN)));
        Check(px == 0x0000FF, "G: ... and what the resize handler wrote is painted by that Render (box blue, 0000FF)",
              [&] { char b[16]; std::snprintf(b, sizeof b, "%06X", px); return std::string(b); }());
        Check(once(delta(h0, h2)), "G: ... and nothing more after it (the changes between are lost)", str(delta(h0, h2)));
    }
    {   // Hidden, one change, then prepainted as the host does it (shown between Update and RefreshDisplay, Render, hidden).
        gv->set_visible(false);
        for (int i = 0; i < 5; ++i) Frame();
        const Steps p0 = steps();
        change(600, 200, 3000);
        for (int i = 0; i < 10; ++i) Frame();
        const Steps p1 = steps();
        const std::string keptH = Js(gv, "kept()");
        g_r->Update();
        gv->set_visible(true);
        g_r->RefreshDisplay(0);
        renderG();
        gv->set_visible(false);
        const Steps p2 = steps();
        const unsigned px = box();
        const std::string keptN = Js(gv, "kept()");
        for (int i = 0; i < 10; ++i) Frame();
        const Steps p3 = steps();
        Check(none(delta(p0, p1)) && once(delta(p0, p2)) && none(delta(p2, p3)) && px == 0x00FF00,
              "G: a prepaint runs each once inside its Render and paints the resize handler's write (box green, 00FF00)",
              "hidden: " + str(delta(p0, p1)) + "; prepaint: " + str(delta(p1, p2)) + "; hidden after: " + str(delta(p2, p3)) +
              [&] { char b[24]; std::snprintf(b, sizeof b, "; box %06X", px); return std::string(b); }());
        Check(keptH == "false" && keptN == "true", "G: ... and a kept MediaQueryList's matches changes in the prepaint's Render",
              "hidden: " + keptH + "; after the prepaint: " + keptN);
    }

    // ── H: OnPageSettled on a page loaded hidden ───────────────────────────────────────────────────────────────
    // What the settle repaint rests on: a page loaded hidden settles only once it has been painted (its prepaint), and
    // inside Update (OnPageSettled takes s_viewsMutex with try_lock, invariant 16); a resize does not settle it again
    // (ApplyPendingResizes folds a settle repaint still due into its next prepaint); a fragment change settles it again
    // after the next paint with no DOM ready, so a page that sets location.hash in rAF would settle after every
    // repaint (why the host latches that repaint per load, settleRepainted).
    std::printf("\n== H: OnPageSettled on a page loaded hidden: the prepaint, a resize, a fragment change\n");
    auto* hp = new Probe{ "H" };
    {
        ViewConfig vc;
        vc.is_accelerated = g_gpu;
        vc.is_transparent = true;
        hp->v = g_r->CreateView(400, 300, vc, nullptr);
        hp->v->set_load_listener(&hp->load);
        hp->v->set_visible(false);
        hp->v->LoadHTML(kFeedPage, "file:///settle.html");
        g_all.push_back(hp);
    }
    View* hv = hp->v.get();
    auto hstate = [&] {
        return "settles=" + S(hp->load.settles) + " in " + (hp->load.settledIn ? hp->load.settledIn : "-") +
               ", is_settled=" + S(hv->is_settled()) + ", DOM ready=" + S(hp->load.dom);
    };
    // A prepaint as FrameWork makes it (TakePrepaintView): shown between Update and RefreshDisplay, Render, hidden.
    auto prepaint = [&] {
        g_phase = "Update";
        g_r->Update();
        g_phase = "no pump call";
        hv->set_visible(true);
        g_phase = "RefreshDisplay";
        g_r->RefreshDisplay(0);
        g_phase = "Render";
        g_r->Render();
        g_phase = "no pump call";
        if (g_gpu) { if (mgHas(g_mg)) mgDraw(g_mg); GpuWait(); }
        hv->set_visible(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    };
    Settle(hp);
    for (int i = 0; i < 60; ++i) Frame();
    Check(hp->load.dom && hp->load.settles == 0 && !hv->is_settled(),
          "H: a page loaded hidden does not settle before it is painted (60 frames after it loaded)", hstate());
    prepaint();
    for (int i = 0; i < 60 && !hp->load.settles; ++i) Frame();
    Check(hp->load.settles == 1 && hp->load.settledIn && std::string(hp->load.settledIn) == "Update" && hv->is_settled(),
          "H: it settles after its prepaint, inside Renderer::Update", hstate());
    for (int i = 0; i < 30; ++i) Frame();
    {
        const int s0 = hp->load.settles;
        hv->Resize(450, 300);
        for (int i = 0; i < 10; ++i) Frame();
        prepaint();
        for (int i = 0; i < 60; ++i) Frame();
        Check(hp->load.settles == s0 && hv->is_settled(), "H: a resize and a paint after it: no new settle, still settled",
              hstate());
    }
    {
        const int s0 = hp->load.settles;
        hp->load.dom = false;
        Js(hv, "location.hash='#next'");
        for (int i = 0; i < 60; ++i) Frame();
        const std::string hidden = hstate();
        const bool quiet = hp->load.settles == s0 && !hv->is_settled();
        prepaint();
        for (int i = 0; i < 60 && hp->load.settles == s0; ++i) Frame();
        Check(quiet && hp->load.settles == s0 + 1 && !hp->load.dom && Js(hv, "location.hash") == "#next",
              "H: a fragment change unsettles the page until its next paint, then it settles again, with no DOM ready",
              "hidden 60 frames: " + hidden + "; after a prepaint: " + hstate());
    }

    std::printf("\n%d failure(s)\n", g_fail);
    for (Probe* p : g_all) p->v = nullptr;
    g_r = nullptr;
    return g_fail ? 1 : 0;
}

// MG_PAGE_FIBER=1: on the page fiber, as Magelight runs pages (../fiber-gc-test/FiberRun.h).
int main(int argc, char** argv) { return FiberRun::Run(argc, argv, &ToolMain); }

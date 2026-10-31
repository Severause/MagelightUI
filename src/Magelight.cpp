// Magelight UI — spike core. See Magelight.h for the threading model.

#include "Magelight.h"
#include "MagelightPresenter.h"
#include "MagelightVR.h"
#include "MagelightApi4.h"
#include "MagelightSound.h"
#include "MagelightManifest.h"
#include "MagelightDevWatch.h"
#include "MagelightCursorArt.h"
#include "MagelightFonts.h"
#include "MagelightSysInfo.h"
#include "HotkeyNames.h"
#include "TooltipCore.h"
#include "GamepadScript.h"
#include "ImeCore.h"
#include "MemoryReportCore.h"
#include "PageStackCore.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "MagelightGameTask.h"   // after CommonLib, which must precede windows.h
#include "PageThreadCore.h"      // the same

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <intrin.h>
#include <MinHook.h>
#include <imm.h>   // IME (0.27.0)
#include <windowsx.h>
#include <tlhelp32.h>
#include <chrono>
#include <thread>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <JavaScriptCore/JavaScript.h>
#include "JsBridgeCore.h"
#include "PathTextCore.h"   // after windows.h

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <Ultralight/ConsoleMessage.h>
#include <Ultralight/ImageSource.h>
#include <Ultralight/RenderTarget.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/platform/Config.h>
#include <Ultralight/platform/Logger.h>
#include <Ultralight/platform/Platform.h>
#include <Ultralight/platform/Surface.h>
#include <Ultralight/platform/GPUDriver.h>
#include <Platform/fonts/FontLoaderWin.h>   // the SDK's Windows font loader, compiled into this DLL (CMakeLists.txt)
#include "InputCore.h"
#include "EditStateCore.h"

#include "../gpu/MagelightGpuApi.h"   // the C ABI description only (MgGpuInfo); never driver internals (invariant 4)

namespace Magelight {

    // ── Host settings (Magelight.json; all optional) ────────────────────────
    // Location: My Games/Skyrim Special Edition/SKSE/Magelight.json (user
    // data, install-independent — beside the log and the cache), falling back
    // to the runtime dir for hand-installed setups.
    // {
    //   "toggleKey": 201,     // DirectInput scancode or key name ("F3") for the UI-mode toggle; 0 = none
    //   "demoViews": false,   // create the badge + playground demo views
    //   "imageProbe": false   // create the ImageSource probe page (views/probe)
    //   "devMode": false      // hot reload every v4 page folder, JS error overlay (reload/inspector keys unbound for now)
    //   "fontHinting": "normal", "fontGamma": 1.8   // text rasterization: smooth|normal|monochrome|none, gamma 1.0-3.0
    //   "msaa": 4             // GPU path: MSAA samples for page targets Ultralight antialiases (1 = off, 2, 4, 8)
    //   "memoryLogMinutes": 10   // 1.5.0: minutes between the periodic memory lines (0 = none, max 1440)
    //   "ulFiberMB": 8        // 1.3.0: the stack pages run on, in MB (8..64; less is raised to 8); 0 = the game's stack
    //   "pageThread": true    // 1.7.0: pages run on a thread of their own (flat only); false = in the game's frame
    //                         // (and no main-menu pages, 1.7.1)
    //   "keyFallback": true   // 1.7.0: keys that reach DirectInput but not the window go to the page anyway (flat only)
    //   "vr": { "enabled": true, "submitViews": true, "mirror": true, "alpha": "straight",
    //            "beam": true, "beamAlpha": 0.55, "cursorScale": 0.012, "cursorDot": true,   // laser-end pointer: a point (0.26.12); false = the arrow art
    //            "suppressRuntimeLaser": true, "aimUseTip": true, "aimPitchDeg": -35,
    //            "runtimeKeyboard": false, "keyboard": true,
    //            "panelWidth": 1600, "panelHeight": 900,   // fullscreen views in VR (NOT the mirror's aspect)
    //            "panelFollow": false,   // true = panels glide back to centre when you look away
    //            "panelDistanceM": 1.2, "panelWidthM": 1.6, "panelHeightOffsetM": -0.1,  // the page layer's quad
    //            "followAngleDeg": 45, "followMinDeg": 80, "followDistM": 0.5 }
    //                                                  // lazy-follow deadzone: margin past the panel EDGE,
    //                                                  // with an absolute floor in degrees
    //                                                  // Skyrim VR presenter (docs/VR_PRESENTER.md)
    //   "logLevel": "info"    // trace | debug | info | warn | error — Magelight.log verbosity
    //   "consoleLog": "warnings"   // 0.31.0: none | errors | warnings | all — page console lines written to the
    //                              // log (unset: "all" in devMode, else "warnings")
    //   "stallWatchdog": true, "stallThresholdMs": 1500   // 0.28.3: log the stalled thread's stack when no
    //                                                     // frame presents this long (250-60000)
    //   "exceptionTelemetry": true   // log the first severe exception at each address, with its stack (false =
    //                                // no exception handler at all; see VectoredLogger)
    //   "blockPolledKeys": true   // 0.31.10: other plugins' GetAsyncKeyState / GetKeyState read keys as up while a
    //                             // page holds UI mode (see InstallPolledKeyFilter)
    //   "skipGameCursor": true    // 1.2.0: the game's cursor is not drawn while a page holds UI mode, flat only
    //                             // (see InstallGameCursorSkip)
    //   "clipCursor": true        // 1.5.1: the Windows cursor stays inside the game window while a page holds the
    //                             // mouse, flat only (see ConfineCursor)
    //   "pinGameCursor": true     // 1.5.1: the game's Cursor Menu is not closed while a page holds UI mode
    //                             // (see InstallCursorMenuPin)
    //   "loadStagger": true, "loadBudgetMs": 8   // 0.31.0: hidden views start their first load one per frame
    //                                            // (see MaterializeViews); false = all in one frame (1-100)
    //   "cursorFile": "", "cursorHeight": 24, "cursorHotspotX": 0, "cursorHotspotY": 0   // the host cursor
    //   "cursorForce": false, "modCursors": true   // 0.31.0: the player's say over mod cursors (Per-view cursors)
    //   "hotkeys": { "ModId/viewName": "F7", "Other/hud": 0 }   // rebind (or 0 = disable) any mod's hotkey
    //   "gamepad": { "enabled": true, "deadZone": 0.25, "exitDeadZone": 0.18, "cursorSpeed": 900,
    //                "repeatDelay": 0.35, "repeatInterval": 0.09, "glyphs": "xbox", "trace": false }
    //                         // 1.6.0: the gamepad layer for views that opt in, flat only (GamepadCore.h);
    //                         // enabled false (or "gamepad": false) turns it off for every view; ranges as
    //                         // Gamepad::Clamped; glyphs: xbox | playstation | generic;
    //                         // trace logs the input batches once a second and each pad button
    // }
    // Toggle key default: PAGE UP (201). Moved off Home in 0.3.1: Home sits
    // in hotkey territory other UI mods use, and two systems doing their
    // cursor/controls/window dance in the same instant is a crash recipe we
    // don't need to be part of. demoViews defaults FALSE now that a real
    // consumer (SeverActions) rides the host — the always-on badge and the
    // Page Up playground are dev surfaces, not something end users should
    // see; flip the flag to field-test the host by itself.
    static std::atomic<std::uint32_t> s_toggleKey{ 201 };
    static std::atomic<bool> s_devMode{ false };   // Magelight.json "devMode": hot reload + JS error overlay
    // Magelight.json "forceCpu": skip the GPU driver and run Ultralight's CPU
    // rasterizer (the surface path) — a diagnostic A/B and a user escape
    // hatch when the accelerated path misrenders on some load order.
    static std::atomic<bool> s_forceCpu{ false };
    static bool s_demoViews = false;
    // Text rasterization (Magelight.json "fontHinting": "smooth" | "normal" | "monochrome" | "none", "fontGamma":
    // 1.0-3.0), so text can be compared in one session. Ultralight 2.0's font profile supplies both (gamma 2.2 in its
    // Default profile): "normal" hinting and an unset fontGamma keep the profile's, anything else pins it.
    static std::string s_fontHinting = "normal";
    static double      s_fontGamma   = 1.8;     // read only when s_fontGammaSet
    static bool        s_fontGammaSet = false;
    // Magelight.json "msaa": the GPU driver's MSAA samples (1 off, 2, 4, 8) for the render targets Ultralight asks
    // to antialias (kGPUTextureFlag_Antialiased). Ultralight 2.0 draws paths analytically (Photon) and asked for
    // none on the pages measured, so the count is an offer; the memory line says whether any target took it. Each
    // sample of a target that does adds a copy of it in video memory (2560x1440: ~15 MB a sample).
    static int         s_msaa        = 4;
    static int         s_msaaInEffect = 1;   // what the driver took (CreateUltralight); 1 = off
    // Magelight.json "ulFiberMB" (1.3.0): the stack the frame work runs on (see UlFiberMain). 0 = the game's own.
    static int         s_ulFiberMb   = 8;
    // The smallest page stack: on one under about 6 MB, JavaScriptCore's script limits (its default
    // maxPerThreadStackUsage is 5 MB) fall to the fiber's end, so its error path commits into the 64 KB guarantee
    // UlFiberMain sets and a page's first uncaught error is 0xC00000FD. tools/fiber-gc-test checks 8 MB.
    static constexpr int kMinFiberMb = 8;
    // Magelight.json "keyFallback" (1.7.0): see LostKeys in the keyboard poll.
    static bool s_keyFallback = true;
    // Magelight.json "pageThread" (1.7.0, default on, flat only): Ultralight on a thread of its own (PageThreadMain),
    // the game thread only running its recorded GPU work and drawing its views. LoadHostSettings makes it Wanted; render
    // init asks the device (StartPageThread), then it is On or Off for the session. Its stack is ulFiberMB (8 when that
    // is 0). The in-frame path has a bug the thread avoids: pages behind a frame-generation swapchain paint rounded,
    // hover-only rows black.
    static std::atomic<PageThread::Mode> s_pageThreadMode{ PageThread::Mode::Off };
    static bool PageThreadOn() { return s_pageThreadMode.load() == PageThread::Mode::On; }
    // Main-menu pages (1.7.1, page thread only). A view is one once a mod asks for UI mode on it at the main menu, or
    // calls SetViewMainMenu; that also starts Ultralight there. All of it runs on the main thread's Presents, which take
    // over from the engine's own thread after the Main Menu opens; s_mainPresenting says they have. The carrier menu
    // stays closed there, so frames draw at Present, or in the UI pass in engine mode. MenuSink keeps s_mainMenuOpen;
    // s_viewsReleased turns on at the first in-game frame, when the other views load.
    static std::atomic<bool> s_mainMenuOpen{ false };
    static std::atomic<bool> s_anyMainMenuPage{ false };   // written under s_viewsMutex
    static std::atomic<bool> s_viewsReleased{ false };
    static std::atomic<bool> s_mainPresenting{ false };
    // Cursor art: the flat cursor is drawn in code (MagelightCursorArt.h) and follows the page's CSS cursor.
    // "cursorFile" (relative to the runtime dir, or absolute; default none) replaces it with a still image,
    // "cursorHeight" is the drawn height in px at 1080p (scaled with resolution) and "cursorHotspotX/Y"
    // (normalized 0..1, the pixel that IS the pointer) place a cursorFile image. A missing or unreadable file
    // keeps the drawn cursor.
    static std::string s_cursorFile;
    // 24 at 1080p is ~1.2x the Windows arrow at 100% scaling (~20px) and stays so at any resolution (48 at
    // 4K, against ~40 at Windows' 200%). VR sizes its pointer from the panel (vr.cursorScale).
    static float       s_cursorHeight = 24.0f;
    static float       s_cursorHotX = 0.0f, s_cursorHotY = 0.0f;
    static bool        s_cursorCustom = false;   // custom art loaded (render thread)
    static int         s_cursorImgW = 0, s_cursorImgH = 0;
    // Mod cursors (0.31.0, see "Per-view cursors"). "cursorForce": true = the host cursor (the cursorFile image,
    // else the drawn art) everywhere, over every mod cursor and over pages that hide the pointer. "modCursors":
    // false = mod images are ignored; a page or view that draws its own pointer still hides the host cursor.
    static std::atomic<bool> s_cursorForce{ false };
    static std::atomic<bool> s_modCursors{ true };
    // "imageProbe": true — the ImageSource probe (views/probe): a host-repainted
    // texture placed eight ways in a page. Dev-only; see TickImageProbe.
    static bool s_imageProbe = false;
    // (LoadHostSettings is defined after s_runtimeDir, below.)

    // Milestone-3 playground: hover styling, click counters, a text field, and
    // a scrollable list — one widget per input path (mouse move/down/up, key,
    // char, wheel). The clock keeps proving the JS engine ticks.
    // Loaded with LoadHTML, so LoadViewLocked writes its CSP in (InjectCsp) as the file system does for a page.
    static constexpr const char* kBannerHTML = R"HTML(
<!DOCTYPE html>
<html><head><style>
  html, body { margin:0; background:transparent; overflow:hidden;
               font-family:Georgia,serif; color:#3a2d17; }
  .card {
    box-sizing:border-box; width:100%; height:100%;
    background:linear-gradient(160deg,#efe3c8,#e2d2ae);
    border:2px solid #8a6d3b; border-radius:10px;
    padding:16px 20px; box-shadow:inset 0 0 40px rgba(120,90,40,.25);
  }
  h1 { margin:0 0 2px; font-size:24px; }
  .sub { font-style:italic; color:#6b5527; font-size:13px; margin-bottom:10px; }
  .row { display:flex; gap:10px; align-items:center; margin-top:10px; }
  button {
    font:inherit; font-size:14px; padding:6px 14px; cursor:pointer;
    background:#e8d9b5; color:#3a2d17; border:1px solid #8a6d3b; border-radius:6px;
  }
  button:hover { background:#8a6d3b; color:#f4ecd8; }
  button:active { transform:translateY(1px); }
  input[type=text] {
    font:inherit; font-size:14px; flex:1; padding:6px 10px;
    background:#f7efdb; border:1px solid #8a6d3b; border-radius:6px; color:#3a2d17;
  }
  #list { margin-top:10px; height:110px; overflow-y:auto; border:1px solid #b39a67;
          border-radius:6px; background:rgba(255,250,235,.55); padding:4px 10px; font-size:13px; }
  #list div { padding:2px 0; border-bottom:1px dotted #c9b184; }
  .foot { display:flex; justify-content:space-between; margin-top:10px; font-size:12px;
          color:#6b5527; }
  .ok { color:#3f6d2a; font-weight:bold; }
  #clock { font-variant-numeric:tabular-nums; }
</style></head>
<body><div class="card">
  <h1>&#10022; Magelight UI &mdash; input playground</h1>
  <div class="sub">GPU-rendered by Ultralight, input routed by our own host. ESC or Page Up closes.</div>
  <div class="row">
    <button id="btn">Click me</button>
    <span id="clicks">0 clicks</span>
    <span id="hover" style="margin-left:auto">hover: &mdash;</span>
  </div>
  <div class="row">
    <input id="txt" type="text" placeholder="type here to test the keyboard&hellip;">
  </div>
  <div class="row"><span id="echo" style="font-size:13px;color:#6b5527">echo: &mdash;</span></div>
  <div id="list"></div>
  <div class="foot">
    <span>Renderer: <span class="ok">GPU</span> &middot; JS: <span class="ok" id="js">warming&hellip;</span></span>
    <span id="clock">--:--:--</span>
  </div>
</div>
<script>
  var clicks = 0;
  var btn = document.getElementById('btn');
  btn.addEventListener('click', function(){
    clicks++;
    document.getElementById('clicks').textContent = clicks + ' click' + (clicks===1?'':'s');
  });
  btn.addEventListener('mouseenter', function(){ document.getElementById('hover').textContent = 'hover: button'; });
  btn.addEventListener('mouseleave', function(){ document.getElementById('hover').textContent = 'hover: —'; });
  document.getElementById('txt').addEventListener('input', function(e){
    document.getElementById('echo').textContent = 'echo: ' + e.target.value;
  });
  var list = document.getElementById('list');
  for (var i = 1; i <= 40; i++) {
    var d = document.createElement('div');
    d.textContent = 'Scroll test row ' + i + ' — wheel over this box';
    list.appendChild(d);
  }
  document.getElementById('js').textContent = 'executing';
  setInterval(function(){
    document.getElementById('clock').textContent = new Date().toLocaleTimeString();
  }, 1000);
</script>
</body></html>
)HTML";

    // ── State ───────────────────────────────────────────────────────────────
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

    static std::atomic<bool> s_runtimeReady{ false };
    static std::atomic<bool> s_hookInstalled{ false };
    // Set at kPostLoadGame/kNewGame. Ultralight is NOT created until the world
    // is up: Skyrim presents main-menu frames from a DIFFERENT thread than
    // in-game frames (field logs 0.3.4/0.3.5 — two distinct present TIDs per
    // session), and WebKit binds its "main thread" at Renderer::Create, then
    // RELEASE_ASSERTs isMainThread() in focus/event dispatch and thread-pool
    // startup. Its CRASH() is a fastfail: no exception dispatch, no VEH, no
    // crashlog — the entire milestone-3 CTD family. Creating on the in-game
    // render thread, and skipping frames from any other thread, is the fix.
    static std::atomic<bool> s_worldReady{ false };
    static DWORD             s_ulThreadId = 0;    // thread Ultralight was created on
    static DWORD             s_mainThreadId = 0;  // captured at plugin load (game main thread)
    // Stall watchdog (0.28.3): the present hook stamps every frame; a thread
    // notices when the stamps stop and samples the stuck thread's stack. See
    // StallWatchdog below. Magelight.json "stallWatchdog": false disables.
    static std::atomic<long long> s_lastPresentMs{ 0 };
    static std::atomic<DWORD>     s_lastPresentTid{ 0 };
    static std::atomic<bool>      s_stallWatchdog{ true };
    // Magelight.json "exceptionTelemetry": false takes the fault logger (VectoredLogger) out of the chain.
    static std::atomic<bool>      s_exceptionTelemetry{ true };
    static bool                   s_blockPolledKeys = true;   // Magelight.json; read before InstallPolledKeyFilter
    static bool                   s_skipGameCursor = true;    // Magelight.json "skipGameCursor"; read before InstallGameCursorSkip
    static std::atomic<bool>      s_clipCursorCfg{ true };    // Magelight.json "clipCursor"
    static bool                   s_pinGameCursor = true;     // Magelight.json "pinGameCursor"; read before InstallCursorMenuPin
    static std::atomic<int>       s_stallThresholdMs{ 1500 };
    // Staggered view loading (0.31.0, MaterializeViews): "loadStagger" and "loadBudgetMs" in Magelight.json.
    static std::atomic<bool>      s_loadStagger{ true };
    static std::atomic<int>       s_loadBudgetMs{ 8 };
    // Magelight.json "memoryLogMinutes" (1.5.0): minutes between the periodic memory lines (TickMemoryReport).
    static std::atomic<int>       s_memLogMinutes{ 10 };
    // Magelight.json "gamepad" (1.6.0): the gamepad layer's tuning (GamepadCore.h), read before the first input batch.
    static Gamepad::Tuning        s_gamepadTuning;
    static std::atomic<bool> s_renderDead{ false };  // latched on any init failure
    static PresentFn         s_origPresent = nullptr;

    // Why the overlay died, for the RenderDead event, RequestUIMode's refusal and the log. Written once and
    // never allocated (it is filled from inside an SEH filter): s_renderDeadReasonState goes 0 -> 1 (writing)
    // -> 2 (readable, release), and readers check for 2 (acquire).
    static char             s_renderDeadReason[192] = {};
    static std::atomic<int> s_renderDeadReasonState{ 0 };

    static void NoteRenderFailure(const char* what, const char* detail = nullptr)
    {
        int expected = 0;
        if (!s_renderDeadReasonState.compare_exchange_strong(expected, 1)) return;
        std::size_t n = 0;
        const auto append = [&n](const char* t) {
            for (; t && *t && n + 1 < sizeof(s_renderDeadReason); ++t) s_renderDeadReason[n++] = *t;
        };
        append(what);
        if (detail && *detail) { append(": "); append(detail); }
        s_renderDeadReason[n] = '\0';
        s_renderDeadReasonState.store(2, std::memory_order_release);
    }

    static const char* RenderDeadReasonText()
    {
        return s_renderDeadReasonState.load(std::memory_order_acquire) == 2 ? s_renderDeadReason : "";
    }

    // Swapchain trust. A game swapchain that is not a full dxgi D3D11 swapchain (Community Shaders' frame
    // generation proxy implements only IDXGISwapChain and sits on Direct3D 12; a wrapper whose GetDevice fails)
    // marks the chain untrusted: no Present1 patch, no late composite, and a present target that never over-
    // releases (DrawOverlay). Latched: once set it is never cleared.
    static std::atomic<bool> s_untrustedChain{ false };
    static std::atomic<bool> s_deviceFromEngine{ false };   // s_device came from the engine, not the swapchain
    static std::atomic<bool> s_lateComposite{ false };       // composite inside dxgi's own Present (InstallLatePresent)
    static std::atomic<bool> s_deadNoticePending{ false };   // the render-death HUD notice waits for the HUD
    static std::atomic<bool> s_uiPassBroken{ false };        // the UI pass had nothing to draw on: Present draws (latched)

    // NVIDIA Smooth Motion (driver frame generation) loads this present layer into the game.
    static bool SmoothMotionLoaded() { return GetModuleHandleW(L"NvPresent64.dll") != nullptr; }

    // Engine mode: Magelight renders with the ENGINE's device and context (RendererData) and draws from the engine's
    // end-of-frame call (BSGraphics::Renderer::End + 9, PrismaUI's and the ImGui overlays' site) into the target the
    // engine left bound, never at Present. Behind Streamline the swapchain hands out wrapper objects; a draw that mixes
    // them with the engine's removes the device under Smooth Motion, and a draw at Present comes after Smooth Motion
    // grabs the frame (pages flicker). On for composite "engine", and for "auto" while Smooth Motion is loaded.
    static bool              s_engineMode = false;
    static std::atomic<bool> s_engineComposite{ false };   // the end-of-frame hook is installed and drawing
    // Behind a swapchain wrapper (a d3d11.dll or dxgi.dll proxy such as ENB's or ReShade's) a draw at Present may
    // never reach the screen, so while the carrier menu is closed (only click-through views up, which never open
    // it) the end-of-frame hook draws the UI-pass quads instead. Set once, with the UI-pass composite on a
    // wrapper, never in engine mode.
    static std::atomic<bool> s_endOfFrameFallback{ false };
    static std::atomic<bool> s_carrierOpen{ false };   // a MagelightOverlayMenu exists (its ctor / dtor)

    // Render-thread-owned (created lazily inside the hook; never touched
    // elsewhere after that).
    static bool                           s_renderInit = false;
    static std::atomic<bool>              s_rendererUp{ false };   // EnsureRenderInit succeeded (any-thread read)
    static ultralight::RefPtr<ultralight::Renderer> s_ulRenderer;
    static ID3D11Device*                  s_device = nullptr;   // from the swapchain's GetDevice; never Release (a wrapper may not AddRef)
    static ID3D11DeviceContext*           s_context = nullptr;  // owned ref
    static ID3D11VertexShader*            s_vs = nullptr;
    static ID3D11PixelShader*             s_ps = nullptr;
    static ID3D11PixelShader*             s_psStraight = nullptr;   // VR copy pass (premultiplied -> straight)
    static ID3D11InputLayout*             s_layout = nullptr;
    static ID3D11Buffer*                  s_vb = nullptr;
    static ID3D11Buffer*                  s_cb = nullptr;   // PS b0: the cutout rect
    static ID3D11SamplerState*            s_sampler = nullptr;
    static ID3D11BlendState*              s_blend = nullptr;
    static ID3D11BlendState*              s_blendStraight = nullptr;   // VR copy pass: straight-alpha 'over'
    static ID3D11Texture2D*               s_cursorTex = nullptr;
    static ID3D11ShaderResourceView*      s_cursorSrv = nullptr;
    static ID3D11Texture2D*               s_dotTex = nullptr;      // VR laser-end pointer (0.26.12)
    static ID3D11ShaderResourceView*      s_dotSrv = nullptr;

    // Cursor position mirrored from MenuCursor by the input sink (game side);
    // the render thread reads it to draw our own cursor sprite. The vanilla
    // cursor is drawn in the game's UI pass — BEFORE Present — so it can never
    // appear above our overlay; we draw our own on top instead.
    static std::atomic<int> s_cursorPosX{ 0 };
    static std::atomic<int> s_cursorPosY{ 0 };
    // The cursor a page asks for (MlView::pageCursor, from ViewListener::OnChangeCursor) picks the drawn cursor's
    // state; the left button's state (input sink, game thread) its press. None = CSS cursor: none (the page draws
    // its own pointer), Custom = cursor: url(...) with an image that loaded (the view's own arrow image, if any).
    enum class PageCursor : int { Arrow = 0, Clickable = 1, Text = 2, None = 3, Custom = 4 };
    static std::atomic<bool> s_mouseDown{ false };
    // Backbuffer size: the game thread writes it at Present; the input sink's cursor clamp and the page work read it.
    static std::atomic<int> s_backbufferW{ 0 }, s_backbufferH{ 0 };
    // The same size as one value (width high, height low), for the page thread: two loads of the pair above can
    // straddle a resize and read a size that never existed.
    static std::atomic<std::uint64_t> s_backbufferWH{ 0 };

    // ── IME (0.27.0): native composition for CJK input ──────────────────
    // The window's IME context is ATTACHED only while a field in the UI-mode
    // view takes typed text (the EditorListener's report, decided in
    // DrainEditableStates; never a password field) and DETACHED otherwise, so
    // gameplay keys never raise a composition. The pre-edit is drawn inline
    // in the field through the view's Editor (DrainInputQueue's edit ops), and
    // keys the IME consumed (VK_PROCESSKEY) never reach the page. No OS IME
    // window shows (none draws over the game): every change goes to the page
    // core on '__ime' (ImeCore.h), which draws the candidate list at the
    // composition unless the page draws its own. A page needs no code for any
    // of it. VR is exempt: the VR keyboards own text there.
    // wparam: 1 attach, 0 detach, 2 reposition the candidates, 3 cancel the IME's copy of a composition the page kept.
    static constexpr UINT kImeCtlMsg = WM_APP + 0x4F46;
    // Lost keys (Input::LostKeys): the message that gives the game window keyboard focus back, and the one that carries
    // a key the poll passed on (wparam vk, lparam as WM_KEYDOWN / WM_KEYUP have it). A message of our own, not a posted
    // WM_KEYDOWN: TranslateMessage would make its text from the thread's key state, which is stale here.
    static constexpr UINT kKeyFocusMsg = WM_APP + 0x4F4A;
    static constexpr UINT kPassedKeyMsg = WM_APP + 0x4F4B;
    static bool s_handlingPassedKey = false;   // window thread
    // The modifiers and lock keys a passed key is read with (Input::NotePassedKey): the live state has moved on by the
    // time it goes out, and the thread's key state is stale here. The window thread writes it; the poll reads NumLock.
    static std::atomic<std::uint16_t> s_passedBits{ 0 };
    static std::atomic<std::uint64_t> s_windowKeyPresses{ 0 };   // first presses through the window proc
    static std::atomic<std::uint32_t> s_keySession{ 0 };         // bumped on UI-mode entry
    static std::atomic<bool> s_imeAttached{ false };
    static std::atomic<bool> s_imeComposing{ false };
    // Between IMN_OPENCANDIDATE and IMN_CLOSECANDIDATE (window thread): the list is read only while it is open, and a
    // detach closes one still open.
    static std::atomic<bool> s_imeCandidatesOpen{ false };
    // Window thread: GetMessageTime when the IME last ended a composition or closed its list (0: never), so an Escape
    // translated into that end is still the IME's (Ime::EscapeGuard); whether the game window has keyboard focus,
    // since a composition the IME ends while it has none is kept as typed (WM_IME_ENDCOMPOSITION).
    static LONG s_imeEndMsgTime = 0;
    static Ime::EndTracker s_imeEnd;   // window thread: whether the IME's last composition ended with a commit
    static bool s_imeWindowFocused = true;
    static Ime::EscapeGuard s_imeEscape;
    // GetTickCount64 of the IME's last message about its composition or list (window thread), and whether the UI-mode
    // view's Editor holds a composition (render thread, once a frame: NoteEditorComposition): Ime::ComposingForEscape.
    static std::atomic<std::uint64_t> s_imeLastMs{ 0 };
    static std::atomic<bool> s_imeEditorComposing{ false };
    // Escape diagnostics: lines left this UI-mode session (reset on entry), and in all.
    static std::atomic<int> s_escLogSession{ 0 }, s_escLogTotal{ 0 };
    static std::atomic<int>  s_imeCaretX{ -1 }, s_imeCaretY{ -1 }, s_imeCaretH{ 0 };   // client px; x<0 = unknown

    static std::filesystem::path s_runtimeDir;  // Data/SKSE/Plugins/Magelight
    // Magelight.json "presentHook": "auto" (late composite behind an earlier Present hook on a plain dxgi
    // swapchain, or for Smooth Motion without engine mode; never on VR), "late" (try it wherever dxgi's own Present
    // can be reached), "vtable" (never). See InstallLatePresent.
    static std::string s_presentHookMode = "auto";
    // Magelight.json "composite": where views are drawn onto the frame. "present" draws them over the back buffer
    // at Present; "engine" from the engine's end of frame with its own device, after every menu (flat only; what
    // "auto" uses under Smooth Motion, see s_engineMode); "ui" draws them in the game's UI pass (MagelightOverlayMenu::PostDisplay), so a mod that keeps
    // the game's UI apart from the scene (Skyrim Upscaler's HUD Fix) keeps ours with it. "auto" picks "ui" when
    // Skyrim Upscaler is loaded or the game swapchain is not dxgi's own: NVIDIA Streamline's (sl.interposer.dll:
    // Skyrim Upscaler, Community Shaders' and Open Shaders' upscaling, whose frame generation drops what is drawn
    // at Present) or another wrapper (a d3d11.dll or dxgi.dll proxy, which may present a buffer other than the one
    // we draw into), unless it is untrusted (frame generation on Direct3D 12: Present draws into the engine's own
    // framebuffer there, DrawOverlay). Never on VR. See UiPassActive.
    static std::string s_compositeMode = "auto";
    // Magelight.json "freezeWorld" (0.31.0, default true): false turns SetViewFreezeWorld off for every mod
    // (QueryCapability("freezeworld") answers 0). "freezeWorldSkipCapture" (default true) keeps MagelightOverlay out
    // of the frame the engine captures as the frozen background, so a translucent page cannot ghost over itself.
    static std::atomic<bool> s_freezeWorldCfg{ true };
    static std::atomic<bool> s_freezeSkipCapture{ true };
    // Magelight.json "consoleLog" (0.31.0): which page console messages are written to Magelight.log. A page logs
    // whatever it likes (one wrote its config, an API key included), so by default only warnings and errors go in.
    // The ConsoleMessage event reaches the owning mod whatever this says. -1 = unset: All in devMode, else Warnings.
    enum ConsoleLog : int { kConsoleLogNone = 0, kConsoleLogErrors = 1, kConsoleLogWarnings = 2, kConsoleLogAll = 3 };
    static std::atomic<int> s_consoleLogCfg{ -1 };
    static constexpr std::size_t   kConsoleLineMaxBytes = 2048;   // a logged message is cut here (UTF-8 safe)
    static constexpr std::uint32_t kConsoleLinesPerSec = 20;      // per view; the rest are counted, not logged

    // Read Magelight.json from the runtime dir (see the settings block at the
    // top of the file for the schema). Missing/corrupt file = defaults.
    static void LoadHostSettings()
    {
        // Before the file: the default holds with no file, and with a corrupt one.
        if (!REL::Module::IsVR()) s_pageThreadMode.store(PageThread::Mode::Wanted);
        try {
            // User config lives in the SKSE user-data dir (My Games/.../SKSE/
            // Magelight.json — beside the log and the cache, independent of
            // how or where the mod was installed; a mod-folder copy is only
            // valid for the ONE mod folder that happens to be active). The
            // runtime dir is the fallback for hand-installed setups.
            std::vector<std::filesystem::path> candidates;
            if (auto dir = SKSE::log::log_directory()) candidates.push_back(*dir / L"Magelight.json");
            candidates.push_back(s_runtimeDir / L"Magelight.json");
            std::filesystem::path p;
            std::error_code ec;
            for (const auto& c : candidates)
                if (std::filesystem::exists(c, ec)) { p = c; break; }
            if (p.empty()) {
                SKSE::log::info("Magelight: no Magelight.json (looked in {} and {}) — defaults in effect",
                    SysInfo::ForLog(candidates.front()), SysInfo::ForLog(candidates.back()));
                return;
            }
            SKSE::log::info("Magelight: settings file {}", SysInfo::ForLog(p));
            std::ifstream f(p);
            nlohmann::json j;
            f >> j;
            if (!j.is_object()) return;
            if (auto it = j.find("toggleKey"); it != j.end()) {
                std::string err;
                const std::uint32_t code = HotkeyNames::Parse(*it, err);
                if (!err.empty()) {
                    SKSE::log::warn("Magelight: toggleKey ignored ({}) - keeping {}", err, s_toggleKey.load());
                } else {
                    if (const auto w = HotkeyNames::ScancodeWarning(code); !w.empty())
                        SKSE::log::warn("Magelight: toggleKey {}", w);
                    if (code == 0) SKSE::log::info("Magelight: toggleKey off - Escape and each mod's own keys still leave UI mode");
                    s_toggleKey.store(code);
                }
            }
            if (auto it = j.find("forceCpu"); it != j.end() && it->is_boolean())
                s_forceCpu.store(it->get<bool>());
            if (auto it = j.find("presentHook"); it != j.end() && it->is_string())
                s_presentHookMode = HotkeyNames::Lower(it->get<std::string>());
            if (auto it = j.find("composite"); it != j.end() && it->is_string())
                s_compositeMode = HotkeyNames::Lower(it->get<std::string>());
            if (auto it = j.find("freezeWorld"); it != j.end() && it->is_boolean())
                s_freezeWorldCfg.store(it->get<bool>());
            if (auto it = j.find("freezeWorldSkipCapture"); it != j.end() && it->is_boolean())
                s_freezeSkipCapture.store(it->get<bool>());
            if (auto it = j.find("devMode"); it != j.end() && it->is_boolean())
                s_devMode.store(it->get<bool>());
            if (auto it = j.find("consoleLog"); it != j.end() && it->is_string()) {
                const std::string c = HotkeyNames::Lower(it->get<std::string>());
                const int lvl = c == "none" ? kConsoleLogNone : c == "errors" ? kConsoleLogErrors
                              : c == "warnings" ? kConsoleLogWarnings : c == "all" ? kConsoleLogAll : -1;
                if (lvl < 0) SKSE::log::warn("Magelight: consoleLog '{}' ignored (none, errors, warnings or all)", c);
                else s_consoleLogCfg.store(lvl);
            }
            if (auto it = j.find("fontHinting"); it != j.end() && it->is_string())
                s_fontHinting = HotkeyNames::Lower(it->get<std::string>());
            if (auto it = j.find("fontGamma"); it != j.end() && it->is_number()) {
                s_fontGamma = std::clamp(it->get<double>(), 1.0, 3.0);
                s_fontGammaSet = true;
            }
            // Ultralight 2.0 has no requestAnimationFrame timer to set (see the frame-rate line CreateUltralight logs).
            if (j.contains("animationTimerDelay"))
                SKSE::log::warn("Magelight: animationTimerDelay is ignored: Ultralight 2.0 has no animation timer");
            // Any JSON number: some editors write 8.0 for 8, and a skipped key would silently keep 4x.
            if (auto it = j.find("msaa"); it != j.end() && it->is_number()) {
                const double asked = it->get<double>();
                s_msaa = asked >= 8 ? 8 : asked >= 4 ? 4 : asked >= 2 ? 2 : 1;
                if (asked != s_msaa) SKSE::log::warn("Magelight: msaa {} is not 1, 2, 4 or 8 - using {}", asked, s_msaa);
            }
            if (auto it = j.find("ulFiberMB"); it != j.end() && it->is_number()) {
                const double asked = it->get<double>();
                s_ulFiberMb = asked <= 0.0
                    ? 0 : static_cast<int>(std::clamp(asked, static_cast<double>(kMinFiberMb), 64.0));
                if (asked > 0.0 && asked < kMinFiberMb)
                    SKSE::log::warn("Magelight: ulFiberMB {} is below {}, the smallest page stack Magelight runs pages "
                                    "on (below about 6 MB a page's uncaught error can overflow it) - using {}", asked, kMinFiberMb, s_ulFiberMb);
                else if (asked > 0.0 && asked != s_ulFiberMb)
                    SKSE::log::warn("Magelight: ulFiberMB {} is outside {}..64 - using {}", asked, kMinFiberMb,
                                    s_ulFiberMb);
            }
            if (auto it = j.find("pageThread"); it != j.end() && it->is_boolean()) {
                if (!it->get<bool>()) {
                    s_pageThreadMode.store(PageThread::Mode::Off);
                    SKSE::log::info("Magelight: pageThread false - pages run in the game's frame");
                } else if (REL::Module::IsVR()) {
                    SKSE::log::info("Magelight: pageThread is ignored on Skyrim VR - pages run in the game's frame");
                }
            }
            if (auto it = j.find("keyFallback"); it != j.end() && it->is_boolean()) s_keyFallback = it->get<bool>();
            if (auto it = j.find("logLevel"); it != j.end() && it->is_string()) {
                const std::string l = HotkeyNames::Lower(it->get<std::string>());
                const auto lvl = l == "trace" ? spdlog::level::trace : l == "debug" ? spdlog::level::debug
                               : l == "warn" ? spdlog::level::warn : l == "error" ? spdlog::level::err
                               : spdlog::level::info;
                if (auto lg = spdlog::default_logger()) lg->set_level(lvl);
                SKSE::log::info("Magelight: log level {}", l);
            }
            if (auto it = j.find("hotkeys"); it != j.end() && it->is_object()) {
                for (auto hk = it->begin(); hk != it->end(); ++hk) {
                    std::string err;
                    const std::uint32_t code = HotkeyNames::Parse(hk.value(), err);
                    if (!err.empty()) { SKSE::log::warn("Magelight: hotkeys['{}'] ignored — {}", hk.key(), err); continue; }
                    if (const auto w = HotkeyNames::ScancodeWarning(code); !w.empty())
                        SKSE::log::warn("Magelight: hotkeys['{}'] {}", hk.key(), w);
                    Api4::SetHotkeyOverride(hk.key().c_str(), code);
                }
            }
            if (auto it = j.find("vr"); it != j.end() && it->is_object()) {
                VR::Settings vs;
                if (auto v = it->find("enabled"); v != it->end() && v->is_boolean()) vs.enabled = v->get<bool>();
                if (auto v = it->find("submitViews"); v != it->end() && v->is_boolean()) vs.submitViews = v->get<bool>();
                if (auto v = it->find("mirror"); v != it->end() && v->is_boolean()) vs.mirror = v->get<bool>();
                if (auto v = it->find("alpha"); v != it->end() && v->is_string())
                    vs.straightAlpha = HotkeyNames::Lower(v->get<std::string>()) != "premultiplied";
                if (auto v = it->find("beam"); v != it->end() && v->is_boolean()) vs.beam = v->get<bool>();
                if (auto v = it->find("beamAlpha"); v != it->end() && v->is_number()) vs.beamAlpha = v->get<float>();
                if (auto v = it->find("cursorScale"); v != it->end() && v->is_number()) vs.cursorScale = v->get<float>();
                if (auto v = it->find("cursorDot"); v != it->end() && v->is_boolean()) vs.cursorDot = v->get<bool>();
                if (auto v = it->find("panelWidth"); v != it->end() && v->is_number_integer())
                    vs.panelWidth = v->get<int>();
                if (auto v = it->find("panelHeight"); v != it->end() && v->is_number_integer())
                    vs.panelHeight = v->get<int>();
                if (auto v = it->find("panelFollow"); v != it->end() && v->is_boolean())
                    vs.panelFollow = v->get<bool>();
                if (auto v = it->find("panelDistanceM"); v != it->end() && v->is_number())
                    vs.panelDistanceM = v->get<float>();
                if (auto v = it->find("panelWidthM"); v != it->end() && v->is_number())
                    vs.panelWidthM = v->get<float>();
                if (auto v = it->find("panelHeightOffsetM"); v != it->end() && v->is_number())
                    vs.panelHeightOffsetM = v->get<float>();
                if (auto v = it->find("followAngleDeg"); v != it->end() && v->is_number())
                    vs.followAngleDeg = v->get<float>();
                if (auto v = it->find("followMinDeg"); v != it->end() && v->is_number())
                    vs.followMinDeg = v->get<float>();
                if (auto v = it->find("followDistM"); v != it->end() && v->is_number())
                    vs.followDistM = v->get<float>();
                if (auto v = it->find("suppressRuntimeLaser"); v != it->end() && v->is_boolean())
                    vs.suppressRuntimeLaser = v->get<bool>();
                if (auto v = it->find("aimUseTip"); v != it->end() && v->is_boolean()) vs.aimUseTip = v->get<bool>();
                if (auto v = it->find("aimPitchDeg"); v != it->end() && v->is_number()) vs.aimPitchDeg = v->get<float>();
                if (auto v = it->find("runtimeKeyboard"); v != it->end() && v->is_boolean())
                    vs.runtimeKeyboard = v->get<bool>();
                if (auto v = it->find("keyboard"); v != it->end() && v->is_boolean())
                    vs.ownKeyboard = v->get<bool>();
                VR::Configure(vs);
            }
            if (auto it = j.find("demoViews"); it != j.end() && it->is_boolean())
                s_demoViews = it->get<bool>();
            if (auto it = j.find("imageProbe"); it != j.end() && it->is_boolean())
                s_imageProbe = it->get<bool>();
            if (auto it = j.find("stallWatchdog"); it != j.end() && it->is_boolean())
                s_stallWatchdog.store(it->get<bool>());
            if (auto it = j.find("exceptionTelemetry"); it != j.end() && it->is_boolean())
                s_exceptionTelemetry.store(it->get<bool>());
            if (auto it = j.find("blockPolledKeys"); it != j.end() && it->is_boolean())
                s_blockPolledKeys = it->get<bool>();
            if (auto it = j.find("skipGameCursor"); it != j.end() && it->is_boolean())
                s_skipGameCursor = it->get<bool>();
            if (auto it = j.find("clipCursor"); it != j.end() && it->is_boolean())
                s_clipCursorCfg.store(it->get<bool>());
            if (auto it = j.find("pinGameCursor"); it != j.end() && it->is_boolean())
                s_pinGameCursor = it->get<bool>();
            if (auto it = j.find("stallThresholdMs"); it != j.end() && it->is_number())
                s_stallThresholdMs.store(std::clamp(it->get<int>(), 250, 60000));
            if (auto it = j.find("loadStagger"); it != j.end() && it->is_boolean())
                s_loadStagger.store(it->get<bool>());
            if (auto it = j.find("loadBudgetMs"); it != j.end() && it->is_number())
                s_loadBudgetMs.store(std::clamp(it->get<int>(), 1, 100));
            if (auto it = j.find("memoryLogMinutes"); it != j.end() && it->is_number())
                s_memLogMinutes.store(static_cast<int>(std::clamp(it->get<double>(), 0.0, 1440.0)));
            if (auto it = j.find("cursorFile"); it != j.end() && it->is_string())
                s_cursorFile = it->get<std::string>();
            if (auto it = j.find("cursorHeight"); it != j.end() && it->is_number())
                s_cursorHeight = std::clamp(it->get<float>(), 8.0f, 256.0f);
            if (auto it = j.find("cursorHotspotX"); it != j.end() && it->is_number())
                s_cursorHotX = std::clamp(it->get<float>(), 0.0f, 1.0f);
            if (auto it = j.find("cursorHotspotY"); it != j.end() && it->is_number())
                s_cursorHotY = std::clamp(it->get<float>(), 0.0f, 1.0f);
            if (auto it = j.find("cursorForce"); it != j.end() && it->is_boolean())
                s_cursorForce.store(it->get<bool>());
            if (auto it = j.find("modCursors"); it != j.end() && it->is_boolean())
                s_modCursors.store(it->get<bool>());
            if (auto it = j.find("gamepad"); it != j.end() && it->is_boolean()) {
                s_gamepadTuning.enabled = it->get<bool>();
            } else if (it != j.end() && it->is_object()) {
                Gamepad::Tuning t;
                if (auto v = it->find("enabled"); v != it->end() && v->is_boolean()) t.enabled = v->get<bool>();
                if (auto v = it->find("trace"); v != it->end() && v->is_boolean()) t.trace = v->get<bool>();
                if (auto v = it->find("deadZone"); v != it->end() && v->is_number()) t.deadZone = v->get<float>();
                if (auto v = it->find("exitDeadZone"); v != it->end() && v->is_number()) t.exitDeadZone = v->get<float>();
                if (auto v = it->find("cursorSpeed"); v != it->end() && v->is_number()) t.cursorSpeed = v->get<float>();
                if (auto v = it->find("repeatDelay"); v != it->end() && v->is_number()) t.repeatDelay = v->get<double>();
                if (auto v = it->find("repeatInterval"); v != it->end() && v->is_number())
                    t.repeatInterval = v->get<double>();
                if (auto v = it->find("glyphs"); v != it->end() && v->is_string()) {
                    const std::string g = v->get<std::string>();
                    t.glyphs = g == "playstation" ? Gamepad::Glyphs::PlayStation
                             : g == "generic"     ? Gamepad::Glyphs::Generic
                                                  : Gamepad::Glyphs::Xbox;
                }
                s_gamepadTuning = Gamepad::Clamped(t);
            }
            static constexpr const char* kConsoleLogNames[] = { "none", "errors", "warnings", "all" };
            SKSE::log::info("Magelight: settings loaded (toggleKey={}, demoViews={}, imageProbe={}, devMode={}, consoleLog={}, "
                            "loadStagger={}, loadBudgetMs={}, cursorForce={}, modCursors={}, msaa={}, ulFiberMB={}, "
                            "pageThread={}, exceptionTelemetry={}, memoryLogMinutes={}, clipCursor={}, gamepad={})",
                s_toggleKey.load(), s_demoViews, s_imageProbe, s_devMode.load(), kConsoleLogNames[ConsoleLogLevel()],
                s_loadStagger.load(), s_loadBudgetMs.load(), s_cursorForce.load(), s_modCursors.load(), s_msaa,
                s_ulFiberMb, s_pageThreadMode.load() != PageThread::Mode::Off,
                s_exceptionTelemetry.load(), s_memLogMinutes.load(), s_clipCursorCfg.load(), s_gamepadTuning.enabled);
            if (s_gamepadTuning.enabled) {
                static constexpr const char* kGlyphNames[] = { "xbox", "playstation", "generic" };
                const Gamepad::Tuning& t = s_gamepadTuning;
                SKSE::log::info("Magelight: gamepad layer: deadZone {:.2f}, exitDeadZone {:.2f}, cursorSpeed {:.0f}, "
                                "repeatDelay {:.2f}, repeatInterval {:.2f}, glyphs {}, trace {}",
                    t.deadZone, t.exitDeadZone, t.cursorSpeed, t.repeatDelay, t.repeatInterval,
                    kGlyphNames[static_cast<int>(t.glyphs)], t.trace);
            }
        } catch (...) {
            SKSE::log::warn("Magelight: Magelight.json unreadable — defaults in effect");
        }
    }

    // Once, by the first thing that needs it: InstallHook at Magelight's kDataLoaded, or a plugin asking about the
    // gamepad layer from its own kDataLoaded, which SKSE can deliver before Magelight's (QueryCapability("gamepad")
    // would answer from the defaults, and a bridge stepping aside on it would leave a player who turned it off with none).
    // Never reach it with Api4's s_mutex or s_viewsMutex held: LoadHostSettings takes s_mutex (SetHotkeyOverride).
    static std::once_flag s_hostSettingsOnce;
    static void EnsureHostSettings() { std::call_once(s_hostSettingsOnce, LoadHostSettings); }

    // ── GPU backend (MagelightGPU.dll — optional; CPU surface is the fallback)
    // Resolved via GetProcAddress after the runtime preload. See
    // gpu/MagelightGpuApi.h for the contract.
    using MgGpuLogFn      = void (*)(const char*);
    using MgGpuCreateFn   = void* (*)(ID3D11Device*, ID3D11DeviceContext*, MgGpuLogFn);
    using MgGpuDestroyFn  = void (*)(void*);
    using MgGpuGetDrvFn   = void* (*)(void*);
    using MgGpuHasFn      = int (*)(void*);
    using MgGpuDrawFn     = void (*)(void*);
    using MgGpuSrvFn      = void* (*)(void*, std::uint32_t);
    using MgGpuSamplesFn  = int (*)(void*, int);   // MSAA sample count
    // External textures (ImageSource).
    using MgGpuRegExtFn  = std::uint32_t (*)(void*, ID3D11ShaderResourceView*);
    using MgGpuSetExtFn   = int (*)(void*, std::uint32_t, ID3D11ShaderResourceView*);
    using MgGpuUnregExtFn = void (*)(void*, std::uint32_t);
    using MgGpuStatsFn    = int (*)(void*, MgGpuStats*);
    using MgGpuTakeListFn = ID3D11CommandList* (*)(void*);

    static MgGpuCreateFn  s_gpuCreate = nullptr;
    static MgGpuDestroyFn s_gpuDestroy = nullptr;
    static MgGpuGetDrvFn  s_gpuGetDriver = nullptr;
    static MgGpuHasFn     s_gpuHas = nullptr;
    static MgGpuDrawFn    s_gpuDraw = nullptr;
    static MgGpuSrvFn     s_gpuSrv = nullptr;
    static MgGpuSamplesFn s_gpuSamples = nullptr;
    static MgGpuRegExtFn   s_gpuRegExt = nullptr;
    static MgGpuSetExtFn   s_gpuSetExtSrv = nullptr;
    static MgGpuUnregExtFn s_gpuUnregExt = nullptr;
    static MgGpuStatsFn    s_gpuStats = nullptr;
    static MgGpuTakeListFn s_gpuTakeList = nullptr;   // contract 3: the parts a deferred recording split at flushes
    static void*          s_gpu = nullptr;       // backend handle (render thread)
    static std::atomic<bool> s_gpuActive{ false };   // accelerated path in use (set by Ultralight's thread at its start)

    // Why a backend's build does not match this host's, or "" when it does. A mod manager can pair this
    // Magelight.dll with another mod's MagelightGPU.dll, and one built for another Ultralight SDK reads every
    // command at the wrong offsets.
    static std::string GpuBuildMismatch(const MgGpuInfo* info)
    {
        if (!info) return "it has no version export (a Magelight before 0.31.6, or another build)";
        if (info->size < sizeof(MgGpuInfo))
            return std::format("its version record is {} bytes, this host reads {}", info->size, sizeof(MgGpuInfo));
        if (info->contract != MGGPU_CONTRACT)
            return std::format("it speaks GPU contract {}, this host {}", info->contract, MGGPU_CONTRACT);
        const char* ul = info->ultralightVersion ? info->ultralightVersion : "?";
        if (info->gpuStateSize != sizeof(ultralight::GPUState) || info->commandSize != sizeof(ultralight::Command)
            || std::strcmp(ul, ULTRALIGHT_VERSION) != 0)
            return std::format("it was built for Ultralight {} ({}-byte state, {}-byte commands), this host for {} "
                               "({}-byte state, {}-byte commands)", ul, info->gpuStateSize, info->commandSize,
                               ULTRALIGHT_VERSION, sizeof(ultralight::GPUState), sizeof(ultralight::Command));
        return {};
    }

    static const MgGpuInfo* QueryGpuInfo(HMODULE gpu)
    {
        using MgGpuInfoFn = const MgGpuInfo* (*)();
        const auto get = reinterpret_cast<MgGpuInfoFn>(GetProcAddress(gpu, "MgGpu_GetInfo"));
        return get ? get() : nullptr;
    }

    static void GpuLogBridge(const char* msg)
    {
        SKSE::log::error("Magelight: {}", msg ? msg : "?");
    }

    // ── View registry + JS <-> C++ bridge (per-view) ───────────────────────
    // Entries are created from ANY thread (CreateView queues intent); the
    // render thread materializes the Ultralight view once the renderer is up
    // and owns all `ul`/texture members after that. Everything else is
    // guarded by s_viewsMutex — contention is a couple of short lock holds
    // per frame plus API calls, so one mutex is plenty.
    // JS -> C++: one native dispatcher (__mlNative) is exposed to each page;
    // per-listener window shims forward (viewId, name, arg) to it — installed
    // at OnWindowObjectReady, BEFORE the page's own scripts run (the contract
    // SeverActions' frontend assumes).
    // C++ -> JS: InteropCall queues {fn, arg} on the view; the render thread
    // drains per frame and calls window.fn(arg) through JavaScriptCore with
    // the payload as a REAL string argument when the page has such a function.
    // Otherwise (every reserved host channel: no page may define a '__' name)
    // it runs window.magelight._dispatch(fn, arg) as the named script
    // magelight:dispatch, with both JSON-quoted (MlJsonQuote) into its source.
    // raw: fn is a script, a mod's (InvokeJS, run unnamed) or the devMode banner (quiet: an exception it throws is not
    // reported, since that report would queue another banner). The host's own pushes go on reserved channels as
    // InteropCalls, so they take that _dispatch path.
    struct OutboundCall { std::string fn; std::string arg; bool raw; bool quiet = false; };
    static std::string MlJsonQuote(const std::string& in);
    static std::string PathToFileUrl(const std::filesystem::path& p);
    struct PendingEval { std::string script; JsResultFnInternal fn; void* user; };
    // Per-view budget for console lines written to the log: kConsoleLinesPerSec a second, the rest counted.
    struct ConsoleRate {
        std::uint64_t windowStart = 0;
        std::uint32_t lines = 0;
        std::uint32_t suppressed = 0;
        // True when this line may be logged; `dropped` = the lines refused in the window that just closed.
        bool Admit(std::uint64_t now, std::uint32_t& dropped)
        {
            if (now - windowStart >= 1000) {
                dropped = suppressed;
                suppressed = 0;
                lines = 0;
                windowStart = now;
            }
            if (lines >= kConsoleLinesPerSec) { ++suppressed; return false; }
            ++lines;
            return true;
        }
    };
    // 0.31.9: per-view log lines for a page's hostTheme. A page may send its theme on every change of its own (a
    // colour picker, an animated fade), so the first accepted change is logged, then at most one a second, which
    // counts the changes it did not log; refusals are logged up to kThemeRefusalsLogged per view, and counted.
    static constexpr std::uint32_t kThemeRefusalsLogged = 32;
    struct ThemeLogRate {
        std::uint64_t lastLine = 0;   // GetTickCount64 of the last change line; 0 = none yet
        std::uint32_t held = 0;       // accepted changes since that line, not logged
        std::uint32_t refusals = 0;   // every refusal, logged or not
        // True when this change may be logged; `skipped` = the changes held back since the last line.
        bool Admit(std::uint64_t now, std::uint32_t& skipped)
        {
            if (lastLine && now - lastLine < 1000) { ++held; return false; }
            skipped = held;
            held = 0;
            lastLine = now ? now : 1;
            return true;
        }
    };

    struct MlView {
        ViewId id = 0;
        std::string htmlPath;   // the page's file: relative to the runtime dir, or drive-lettered; "" = inline banner
        // The file:/// URL the last host navigation loaded, its query and fragment included ("" = the page at htmlPath,
        // which a navigation also follows): what a wake or a rebuild loads again.
        std::string pageUrl;
        // The page's own folder (its mod folder under Data\Magelight\<Mod>, or
        // the runtime dir for host pages): one of the roots MlFileSystem
        // serves, and the key its network level is published under for the
        // page's CSP (RootNetLevel). WebKit asks for files with no View in
        // hand, so the file gate is the union of every root, not this one.
        std::filesystem::path root;
        int x = 0, y = 0;       // negative = anchored from right/bottom edge
        int w = 0, h = 0;
        bool visible = false;
        bool clickThrough = false;
        bool isInspector = false;   // a hosted Web Inspector: visible only while UI mode is active
        int pageCursor = 0;         // PageCursor: the CSS cursor under the pointer, as the page last reported it
        float cutX = 0, cutY = 0, cutW = 0, cutH = 0;   // compositor cutout, view pixels (w<=0 = none)
        std::uint32_t hibernateMs = 0;  // 0 = never; else release the View after this long hidden
        std::uint64_t hiddenSince = 0;  // GetTickCount64 at the last hide (0 = visible / never hidden)
        bool dormant = false;           // hibernated: no View until shown again
        bool domReady = false;          // main frame reached DOM ready since its last load
        // The document the View shows reached DOM ready. ReloadView and NavigateView clear domReady at once and leave
        // this, so domReady can be put back when the document stays: by ApplyPendingLifecycle for a navigation that
        // begins no load (a fragment one), by OnFailLoading for a host load that fails (hostLoad).
        bool docDomReady = false;
        // A host Navigate or Reload may replace the page (pending: from the call until it begins no load, or until a
        // DOM ready, a failure or a new document's window object), and the view as that page left it. A load that
        // fails keeps its page, so OnFailLoading puts these back, and domReady with them. A request made while one is
        // pending keeps the first one's record: its call stops the load before it, with no callback.
        struct HostLoad {
            bool pending = false;
            bool docDomReady = false;
            bool reloadedFlag = false;
            std::string htmlPath, pageUrl;
        };
        HostLoad hostLoad;
        bool loadNow = false;           // shown before its first load: skips the load queue (MaterializeViews)
        bool loadFailed = false;        // main frame failed to load since its last load (the load queue's settle test)
        bool loadOnShow = false;        // 0.31.0 SetViewLoadOnShow: no first load until the view is shown
        bool mainMenu = false;          // 1.7.1 loads and shows at the main menu: SetViewMainMenu, or UI mode asked there
        // 0.31.11 SetViewPrepaint: paint the page while hidden, kPrepaintSettleMs after paintableAt, and once more
        // when it settles (settleRepaint), so its first show composites a finished texture instead of painting the
        // whole page that frame.
        bool prepaint = false;
        bool prepainted = false;        // painted (shown, or prepainted) since paintableAt
        // GetTickCount64 of the last DOM ready, resize, or host Navigate or Reload that kept its page (one that began no
        // load, or whose load failed); 0 = none since the last load or host request.
        std::uint64_t paintableAt = 0;
        // OnPageSettled on a hidden prepaint view: TakePrepaintView paints it once more. Ultralight 2.0 decodes a
        // page's images only once it is painted, so the first prepaint has none and this paint is the one with them.
        bool settleRepaint = false;
        // That paint was taken: no more until the next DOM ready or host request that kept its page (as paintableAt;
        // a resize leaves it). A change of the URL's fragment is a new navigation that settles again with no DOM ready,
        // so a page whose requestAnimationFrame sets location.hash would otherwise settle again after every such
        // paint, without end.
        bool settleRepainted = false;
        bool fullscreen = false;    // w/h track the backbuffer (0,0 at CreateView)
        // Ultralight device scale: CSS px -> view px. The page lays out and
        // rasterizes at this scale (real DPI — sharp), instead of a consumer
        // CSS transform that resamples a 1x raster (soft). 1 = 100%.
        float deviceScale = 1.0f;
        // View::needs_paint() sampled BEFORE Renderer::Render() clears it —
        // "this view's pixels change this frame". The VR submitter uses it to
        // skip a copy + compositor upload the runtime does not need.
        bool repaintedThisFrame = true;
        bool boundsDirty = false;   // ApplyPendingResizes visits the view (a new size, or a new scale with scaleDirty)
        bool scaleDirty = false;    // ApplyPendingResizes applies deviceScale; set only with boundsDirty
        bool rebuildPending = false;   // 0.31.7 RebuildViewAtScale: render thread releases the View (ApplyPendingRebuilds)
        bool rebuilt = false;          // released by a rebuild, not by hibernation (LoadViewLocked's log line)
        float liveScale = 0.0f;        // the device scale the current View has (0 = no View); deviceScale is the wanted one
        bool shimsDirty = false;    // render thread re-installs bridge shims
        DomReadyFn onDomReady = nullptr;
        struct Listener { JsListenerFn fn = nullptr; JsListenerExFn fnEx = nullptr; void* user = nullptr; };
        std::map<std::string, Listener> listeners;
        std::vector<OutboundCall> outbound;
        std::vector<PendingEval> evals;     // EvalJS requests; drained with outbound
        // v4: z-order = (layer, order); lifecycle intents the render thread applies.
        int layer = 1;
        std::uint64_t order = 0;
        bool escapeCapture = false;   // 0.28.0: the page owns Escape (delivered as a key, no UI-mode exit)
        bool freezeWorld = false;     // 0.31.0: freeze the world behind this view while it holds UI mode paused
        NetLevel netLevel = NetLevel::File;   // the owning mod's NetworkPolicy; File when no mod owns the view
        int  scrollStep = 40;         // 0.28.0: px per wheel notch
        std::string soundOpen, soundClose;   // 0.29.0: host-played on UI-mode enter/exit for this view ("" = none)
        CursorSet cursorOwn;            // 0.31.0: the view's own cursor (SetViewCursor / SetCursor / manifest view)
        CursorSet cursorMod;            // 0.31.0: its mod's manifest default, used while cursorOwn is empty
        CursorTintSet cursorTint;       // 0.31.1: the drawn cursor's colours over this view (SetViewCursorTint)
        std::optional<HostTheme::KbTheme> kbTheme;   // 0.31.9: the VR keyboard's colours while this view holds UI mode
        // The page's text focus as the EditorListener last reported it, applied by DrainEditableStates (render
        // thread) and reset with the page (ForgetEditStateLocked), since a replaced page reports nothing.
        // Brace-initialised: EditableState's default constructor is a runtime import (a UExport struct with member
        // initialisers), and CreateView builds an MlView on an inert host too (invariant 3).
        ultralight::EditableState edit{};
        bool editNumber = false;        // its focused field is an <input type=number> (DrainEditableStates)
        ThemeLogRate themeLog;          // 0.31.9: this page's hostTheme log lines (OnPageHostTheme)
        bool outboundDropLogged = false;   // the outbound cap's one warning for this view (QueueOutboundLocked)
        bool tooltipText = false;       // 1.2.0: the page was last sent a non-empty title (DrainTooltips)
        // 1.6.0 gamepad layer (GamepadCore.h): the opt-in (SetViewGamepad, kept across reloads), the mode the input
        // sink last left it in, and the page helper's id from its last 'ready' ("" = none yet: the view cannot own
        // the pad). Each ready bumps the epoch, so a reloaded page re-arms the held-button rules like a new owner.
        Gamepad::ViewConfig gamepad;
        Gamepad::Mode gamepadMode = Gamepad::Mode::Navigation;
        std::string gamepadPage;
        std::uint64_t gamepadEpoch = 0;
        bool gamepadDirty = false;      // the page has not seen the state since the opt-in or its ready changed
        bool destroyPending = false;
        bool reloadPending = false;
        bool reloadedFlag = false;      // tag the next DOM-ready as a ViewReloaded
        std::string navigateUrl;        // non-empty = LoadURL next frame
        std::string sessionName;        // "" = default session; else a per-mod persistent session
        ConsoleRate consoleRate;        // 0.31.0: console lines written to the log (s_viewsMutex)
        // Render-thread-owned:
        ultralight::RefPtr<ultralight::View> ul;
        ID3D11Texture2D* tex = nullptr;             // CPU-surface path only
        ID3D11ShaderResourceView* srv = nullptr;
        int texW = 0, texH = 0;
    };
    // An OwnedMutex (PageThreadCore.h): a page thread that faults while holding it gives it back before it parks.
    static PageThread::OwnedMutex s_viewsMutex;
    static std::vector<std::unique_ptr<MlView>> s_views;  // z-order = (layer, order), see SortViewsLocked
    static std::atomic<std::uint64_t> s_nextViewId{ 1 };
    static std::atomic<std::uint64_t> s_uiModeView{ 0 };  // UI mode's show/focus target
    // The view that entered UI mode last (Api4::ViewLabel), kept in a plain buffer so the exception handler can name
    // it without a lock or an allocation. Game thread writes; a torn read only garbles a log line.
    static char s_uiModeLabel[160] = "none yet";
    // The view the TOGGLE KEY opens — set only by SetUIModeView (the host's
    // own/demo UI). EnterUIMode deliberately does NOT set it: a consumer's
    // (SA's) view must never be re-opened by our key behind the consumer's
    // open bookkeeping — the key stays exit-only for consumer views.
    static std::atomic<std::uint64_t> s_toggleView{ 0 };
    static std::atomic<std::uint64_t> s_nextViewOrder{ 1 };
    static std::atomic<std::uint64_t> s_inspectorRequest{ 0 };   // inspector toggle request: page (or inspector) view id; render thread consumes (no key bound yet)
    static std::map<ViewId, ViewId> s_inspectorOf;               // page -> hosted inspector view (render thread only)
    static std::atomic<std::uint64_t> s_inspectorVisibleFor{ 0 };   // page whose inspector is showing (0 = none); any thread reads
    static std::atomic<std::uint64_t> s_inspectorHideRequest{ 0 }; // 0.28.0: page whose inspector should hide; render thread
    static std::atomic<HostEventFn> s_hostEventSink{ nullptr };

    static void Emit(HostEvent type, ViewId view, int x = 0, int y = 0, const char* detail = "")
    {
        if (HostEventFn fn = s_hostEventSink.load()) fn(type, view, x, y, detail ? detail : "");
    }

    // s_viewsMutex must be held. Keeps the registry in z-order.
    static void SortViewsLocked()
    {
        std::stable_sort(s_views.begin(), s_views.end(),
            [](const std::unique_ptr<MlView>& a, const std::unique_ptr<MlView>& b) {
                return a->layer != b->layer ? a->layer < b->layer : a->order < b->order;
            });
    }

    // s_viewsMutex must be held.
    static MlView* FindViewLocked(ViewId id)
    {
        for (auto& v : s_views)
            if (v->id == id) return v.get();
        return nullptr;
    }
    static MlView* FindViewByUlLocked(ultralight::View* ul)
    {
        for (auto& v : s_views)
            if (v->ul.get() == ul) return v.get();
        return nullptr;
    }

    // Page tooltips (1.2.0, TooltipCore.h). WebCore reports the title under the pointer from inside a mouse event or
    // an update (MlViewListener::OnChangeTooltip, render thread); the changes wait here, a leaf lock, until the frame
    // hands them to the pages (DrainTooltips), which draw the tooltip themselves (the page core's '__tooltip').
    static std::mutex s_tooltipMutex;
    static std::vector<std::pair<ultralight::View*, std::string>> s_tooltipChanges;
    static std::atomic<bool> s_tooltipLeaveAll{ false };   // UI mode ended or moved: every page hides its tooltip
    static void QueueOutboundLocked(MlView& v, OutboundCall call);

    // Text focus (EditStateCore.h). The EditorListener records each report here from inside whatever Ultralight call
    // fires it (a click, a key, Focus / Unfocus, a script, Update, Render, LoadViewLocked's LoadURL under
    // s_viewsMutex), so s_editMutex is a leaf: never take s_viewsMutex while holding it. The frame applies the
    // reports once, after the input (DrainEditableStates). View pointers are only compared; a released, reloaded or
    // destroyed View's waiting reports are dropped (ForgetEditStateLocked), so a recycled address cannot inherit them.
    struct EditChange {
        ultralight::View* ul;
        ultralight::EditableState st{};
        bool hostFocus;   // fired inside the focus marker's View::Focus / Unfocus (DrainInputQueue)
    };
    static std::mutex s_editMutex;
    static std::vector<EditChange> s_editChanges;   // under s_editMutex; the oldest goes past kEditChangeCap
    static constexpr std::size_t kEditChangeCap = 64;
    // Under s_editMutex: which main-frame window object is a new document's, where MlLoadListener drops the replaced
    // page's state (a page navigating on its own, which no host path sees).
    static EditState::DocLoads s_docLoads;
    static bool s_hostFocusCall = false;   // render thread: inside the focus marker's Focus / Unfocus
    static std::vector<ViewId> s_editResets;   // render thread: the views whose state was reset since the last drain
    static std::vector<EditState::Click> s_uiClicks;   // render thread: this frame's left presses on the UI-mode view

    // s_viewsMutex held, render thread. The view's page is gone or going (a load, reload, navigation, release or
    // destroy): no report says so, so its waiting reports and its state are dropped here. released: the View itself
    // goes, so DocLoads forgets it too (a new View can get its address).
    static void ForgetEditStateLocked(MlView& v, bool released = false)
    {
        if (v.ul) {
            std::lock_guard<std::mutex> lk(s_editMutex);
            std::erase_if(s_editChanges, [&](const EditChange& c) { return c.ul == v.ul.get(); });
            if (released) s_docLoads.Released(v.ul.get());
        }
        v.edit = {};
        v.editNumber = false;
        if (std::find(s_editResets.begin(), s_editResets.end(), v.id) == s_editResets.end()) s_editResets.push_back(v.id);
    }

    // s_viewsMutex must be held. The pointer left the page (another view, a hide, the end of UI mode): WebCore never
    // sees that, so the page is told; it keeps the title and shows it again when the pointer comes back. Sent whether
    // or not the page holds a title (one may still be on its way), but only to a loaded page, so it never queues up.
    static void QueueTooltipLeaveLocked(MlView& v)
    {
        if (!v.ul || !v.domReady) return;
        QueueOutboundLocked(v, { "__tooltip", "{\"l\":1}", false });
    }

    // s_viewsMutex must be held. ShowView's body, for a caller that must change visibility in the same critical
    // section as something else (ShowOwnKeyboard). It changes only the record (any thread); the Ultralight View
    // follows on the render thread (SyncViewVisibility).
    static void ShowViewLocked(MlView& v, bool show)
    {
        if (v.visible != show) v.hiddenSince = show ? 0 : GetTickCount64();
        v.visible = show;
        if (!v.ul) v.loadNow = show;   // an open never waits behind the load queue
        if (!show) v.pageCursor = 0;   // reopened, the page reports again on the first mouse move
        if (!show) QueueTooltipLeaveLocked(v);
    }

    // s_viewsMutex must be held. A C++ -> JS call waits here until the page reaches DOM ready (DrainBridgeQueues);
    // past kOutboundCap the rest are dropped, so a page that never loads cannot grow the queue without bound.
    static constexpr std::size_t kOutboundCap = 256;
    static void QueueOutboundLocked(MlView& v, OutboundCall call)
    {
        if (v.outbound.size() < kOutboundCap) {
            v.outbound.push_back(std::move(call));
            return;
        }
        if (v.outboundDropLogged) return;
        v.outboundDropLogged = true;
        SKSE::log::warn("Magelight: view {} has {} calls waiting for its page (domReady={}) - {} was dropped, as is "
                        "every call over that while the queue is full (logged once per view)", v.id, v.outbound.size(),
                        v.domReady, call.raw ? std::string("a script") : "'" + call.fn + "'");
    }

    // ── Texture-backed image registry (Ultralight ImageSource) ──────────────
    // Registered from ANY thread (RegisterTextureImage queues intent); the
    // render thread materializes (driver id + ImageSource + provider entry)
    // and drains invalidations each frame (MaterializeImages), exactly like the view registry.
    // Entries are never erased (an unregistered image is tombstoned) so
    // ImageUrl's pointer and the driver id stay valid for pages that still
    // reference them.
    struct MlImage {
        ImageId id = 0;
        std::string name;
        std::uint32_t w = 0, h = 0;
        ID3D11ShaderResourceView* srv = nullptr;         // our own AddRef'd ref
        ID3D11ShaderResourceView* pendingSrv = nullptr;  // UpdateTextureImage, applied on the render thread
        bool dirty = false;
        bool unregister = false;
        bool removed = false;
        std::string url;
        // Render-thread-owned:
        std::uint32_t texId = 0;
        ultralight::RefPtr<ultralight::ImageSource> src;
    };
    static PageThread::OwnedMutex s_imagesMutex;   // an OwnedMutex for the same reason as s_viewsMutex
    static std::vector<std::unique_ptr<MlImage>> s_images;
    static std::atomic<std::uint32_t> s_nextImageId{ 1 };

    static MlImage* FindImageLocked(ImageId id)
    {
        for (auto& i : s_images)
            if (i->id == id) return i.get();
        return nullptr;
    }

    // Resolve the anchored position against the current backbuffer size.
    static void EffectivePos(const MlView& v, float bw, float bh, float& outX, float& outY)
    {
        outX = (v.x >= 0) ? static_cast<float>(v.x) : bw + static_cast<float>(v.x);
        outY = (v.y >= 0) ? static_cast<float>(v.y) : bh + static_cast<float>(v.y);
    }

    static bool ViewShows(const MlView& v)
    {
        return PageThread::ViewShowsNow(s_pageThreadMode.load() == PageThread::Mode::Off, v.mainMenu,
                                        s_mainMenuOpen.load(), s_viewsReleased.load());
    }

    static void NoteMainMenuPagesLocked()
    {
        bool any = false;
        for (const auto& v : s_views) any = any || (v->mainMenu && !v->destroyPending);
        s_anyMainMenuPage.store(any);
    }

    // True when any view wants pixels this frame.
    static bool AnyViewVisible()
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        for (const auto& v : s_views)
            if (v->visible && ViewShows(*v)) return true;
        return false;
    }

    // A visible view the player can click: the UI-pass carrier opens only for one of these (see
    // MagelightOverlayMenu), so a HUD view that stays up never holds it open over gameplay.
    static bool AnyInteractiveViewVisible()
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        for (const auto& v : s_views)
            if (v->visible && !v->clickThrough && ViewShows(*v)) return true;
        return false;
    }

    static std::string JSStringToStd(JSStringRef s) { return JsBridge::ToStd(s); }

    static std::string JsonEscape(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (const char c : s) {
            switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else {
                    out += c;
                }
            }
        }
        return out;
    }

    // Render thread (JS execution context). Handlers fire HERE — game-state
    // work must marshal via GameTask::Post (MagelightGameTask.h).
    // Args: (viewIdString, listenerName, payload).
    // ── Our own virtual keyboard (0.20.0) ───────────────────────────────
    // A host page (views/keyboard) the VR laser clicks, raised when a text
    // field in ANOTHER view takes focus. It never takes UI mode, so the page
    // being typed into keeps key focus and the keys we inject land there —
    // DrainInputQueue routes keys to the UI-mode view, and the laser's clicks
    // hit-test to whichever view is under the ray. Runtime-independent: no
    // ShowKeyboardForOverlay (it steals input focus), no export lookup.
    static std::atomic<std::uint64_t> s_kbViewId{ 0 };
    // CreateView / SetViewLayer / ShowView / GetDisplaySize are the public
    // namespace functions declared in Magelight.h — do NOT redeclare them
    // static here, that gives them internal linkage and breaks every other
    // translation unit that calls them. Only the file-local queue needs a fwd.
    // keys: the Input::KeyBits a key message is read with; < 0 = capture them now (CaptureKeyBits(false)).
    // source: the Input::PointerSource a pointer message comes from.
    static void QueueInput(UINT msg, WPARAM w, LPARAM l, int keys = -1, std::uint8_t source = Input::kPointerMouse);
    static std::uint16_t CaptureKeyBits(bool windowProc);

    static void EnsureKeyboardView()
    {
        if (s_kbViewId.load()) return;
        int bw = 0, bh = 0;
        GetDisplaySize(bw, bh);
        constexpr int kKbW = 980, kKbH = 330;
        const int x = (bw > kKbW + 40) ? (bw - kKbW) / 2 : 20;
        const int y = (bh > kKbH + 100) ? (bh - kKbH - 70) : 20;
        const ViewId id = CreateView("views/keyboard/index.html", x, y, kKbW, kKbH, nullptr, false, false);
        if (!id) return;
        SetViewLayer(id, 3);   // System: above the panel it serves
        s_kbViewId.store(id);
        VR::Placement p;
        // Placed, not following — same reasoning as the page layer. Left at
        // the host default so the vr.panelFollow setting covers it too.
        p.mode = VR::DefaultPanelMode();
        // In FRONT of the page, not level with it. The panel layer sits at
        // panelDistanceM (1.2 m by default) — at the keyboard's old 1.1 m the
        // two quads were 10 cm apart with overlapping vertical extents, which
        // sort order hides but which reads as one surface clipping another.
        // 0.9 m keeps it plainly nearer than the page it serves and inside
        // easy pointing reach.
        p.distanceMeters = 0.9f;
        p.widthMeters = 1.0f;
        p.heightOffset = -0.42f;  // below the panel, angled into reach
        VR::SetPlacement(id, p);
        SKSE::log::info("Magelight: own keyboard view {} created ({}x{} at {},{})", id, kKbW, kKbH, x, y);
    }

    // Keyboard state (0.31.9). Every change the keyboard page should know of queues one message holding the whole
    // state, {"shown":bool,"theme":<the UI-mode view's theme>|null,"field":<its focused field>|null}, built here from
    // the stored numbers and EditState::FieldJson's fixed literals (the page never sees a page's text) on this
    // host-only channel. It replaces any message of the channel still queued, so the page always ends on the stored
    // state, and it is exempt from the outbound cap, which would drop the newest. Queued whether or not the keyboard
    // is up, so the hidden page is already current when it is next shown, and under s_viewsMutex together with each
    // store and visibility change, so an older state cannot overtake a newer. "shown" false puts the page back in its
    // blank wait: a show stays blank until its own message lands.
    static constexpr const char* kKbThemeChannel = "__kbtheme";
    static void QueueKeyboardThemeLocked()
    {
        MlView* kb = FindViewLocked(static_cast<ViewId>(s_kbViewId.load()));
        if (!kb || kb->destroyPending) return;
        const MlView* ui = FindViewLocked(static_cast<ViewId>(s_uiModeView.load()));
        std::string msg = std::string("{\"shown\":") + (kb->visible ? "true" : "false") + ",\"theme\":" +
                          ((ui && ui->kbTheme) ? HostTheme::KeyboardJson(*ui->kbTheme) : "null") + ",\"field\":" +
                          ((ui && ui != kb) ? EditState::FieldJson(ui->edit, ui->editNumber) : std::string("null")) + "}";
        std::erase_if(kb->outbound, [](const OutboundCall& c) { return !c.raw && c.fn == kKbThemeChannel; });
        kb->outbound.push_back({ kKbThemeChannel, std::move(msg), false });
    }

    static void ShowOwnKeyboard(bool on)
    {
        if (on) {
            if (!VR::OwnKeyboardEnabled()) return;   // flat types on the real keyboard
            if (!IsUIModeActive()) return;            // nothing is open to type into
            EnsureKeyboardView();
        }
        // The visibility and the message that says so change together (QueueKeyboardThemeLocked).
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        MlView* kb = FindViewLocked(static_cast<ViewId>(s_kbViewId.load()));
        if (!kb || (!on && !kb->visible)) return;   // already down: the page was told then
        ShowViewLocked(*kb, on);
        QueueKeyboardThemeLocked();
    }

    // A key the host makes up from a bare virtual key (the VR keyboards, Magelight_DeliverVKey): down, the character
    // Enter and Tab also type (a form's implicit submit and a textarea's newline come from it), up. Any thread.
    static void QueueVirtualKey(int vk)
    {
        if (vk <= 0 || vk > 0xFF) return;
        const LPARAM down = Input::SyntheticKeyLParam(static_cast<UINT>(vk), false);
        QueueInput(WM_KEYDOWN, static_cast<WPARAM>(vk), down);
        if (vk == VK_RETURN || vk == VK_TAB)
            QueueInput(WM_CHAR, static_cast<WPARAM>(vk == VK_RETURN ? L'\r' : L'\t'), down);
        QueueInput(WM_KEYUP, static_cast<WPARAM>(vk), Input::SyntheticKeyLParam(static_cast<UINT>(vk), true));
    }
    static void QueueEditCommand(ultralight::EditorCommand cmd);   // after the edit-op queue it uses

    // A key from that page: {"c":"a"} text (up to kOwnKeyboardTextMax UTF-16 units; a key such as '.com' is one
    // edit), {"vk":8} a virtual key, {"cmd":"undo"} an edit button (EditState::CommandByName; any other name does
    // nothing), {"close":true} dismiss. Text and keys reach the UI-mode page through the ordinary queue.
    static constexpr int kOwnKeyboardTextMax = 64;
    static void OnOwnKeyboardKey(const std::string& json)
    {
        try {
            const auto j = nlohmann::json::parse(json);
            if (j.value("close", false)) { ShowOwnKeyboard(false); return; }
            if (auto d = j.find("drag"); d != j.end() && d->is_string()) {
                const std::string what = d->get<std::string>();
                const ViewId kb = static_cast<ViewId>(s_kbViewId.load());
                if (what == "start") VR::BeginViewDrag(kb);
                else VR::EndViewDrag();
                return;
            }
            if (auto it = j.find("vk"); it != j.end() && it->is_number_integer()) {
                QueueVirtualKey(it->get<int>());
                return;
            }
            if (auto it = j.find("cmd"); it != j.end() && it->is_string()) {
                ultralight::EditorCommand cmd{};
                if (EditState::CommandByName(it->get<std::string>(), cmd)) QueueEditCommand(cmd);
                return;
            }
            if (auto it = j.find("c"); it != j.end() && it->is_string()) {
                const std::string c = it->get<std::string>();
                wchar_t wide[kOwnKeyboardTextMax]{};
                const int n = c.empty() ? 0 : MultiByteToWideChar(CP_UTF8, 0, c.data(), static_cast<int>(c.size()),
                                                                 wide, kOwnKeyboardTextMax);
                if (n > 0) QueueTypedText(wide, static_cast<std::size_t>(n));
            }
        } catch (...) {
            SKSE::log::warn("Magelight: unreadable key payload from the keyboard page");
        }
    }

    // IME (0.27.0) — the body lives after the window statics it touches.
    static void ImeOnTextFocus(bool focused);

    static void PlayPageSound(ViewId id, const std::string& arg);   // fwd (defined after the dispatcher)
    static void OnPageHostTheme(ViewId id, const std::string& arg); // fwd (defined after the dispatcher)

    // 1.6.0: the gamepad helper's requests ({op, page[, mode]}). 'ready' names the document's helper, which the
    // input sink's packets carry; 'mode' asks for cursor or navigation mode, and cursor only where the view allows
    // it. Anything else is dropped: the page decides nothing about who holds the pad.
    static void OnPageGamepad(ViewId id, const std::string& arg)
    {
        if (arg.size() > 512) return;
        const auto req = nlohmann::json::parse(arg, nullptr, false);
        if (!req.is_object()) return;
        const auto op = req.find("op"), page = req.find("page");
        if (op == req.end() || !op->is_string() || page == req.end() || !page->is_string()) return;
        const std::string pageId = page->get<std::string>();
        if (!Gamepad::ValidPageId(pageId)) return;
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(id);
        if (!v) return;
        if (op->get<std::string>() == "ready") {
            v->gamepadPage = pageId;
            ++v->gamepadEpoch;
            v->gamepadDirty = true;
        } else if (op->get<std::string>() == "mode" && pageId == v->gamepadPage) {
            const auto mode = req.find("mode");
            if (mode == req.end() || !mode->is_string()) return;
            const std::string m = mode->get<std::string>();
            if (m == "cursor" && v->gamepad.allowCursor) v->gamepadMode = Gamepad::Mode::Cursor;
            else if (m == "navigation") v->gamepadMode = Gamepad::Mode::Navigation;
            else return;
            v->gamepadDirty = true;
        }
    }

    // userData is the view id the page's own __mlNative was made with
    // (InstallBridgeShims). The id a page passes is only checked against it:
    // trusting it would let any page fire another view's listeners, sounds or
    // keys as that view.
    static JSValueRef MlNativeDispatch(JSContextRef ctx, void* userData, JSObjectRef, JSObjectRef,
        size_t argc, const JSValueRef argv[], JSValueRef*)
    {
        auto argToStr = [&](size_t i) -> std::string {
            if (argc <= i) return {};
            JSStringRef js = JSValueToStringCopy(ctx, argv[i], nullptr);
            if (!js) return {};
            std::string out = JSStringToStd(js);
            JSStringRelease(js);
            return out;
        };
        const std::string idStr = argToStr(0);
        const std::string name = argToStr(1);
        const std::string arg = argToStr(2);
        const ViewId id = static_cast<ViewId>(reinterpret_cast<std::uintptr_t>(userData));
        if (!id || std::strtoull(idStr.c_str(), nullptr, 10) != id) {
            static std::atomic<int> s_forgedLogged{ 0 };
            if (s_forgedLogged.fetch_add(1) < 32) {
                SKSE::log::warn("Magelight: view {} called '{}' as view '{}' — refused (a page acts only as itself)",
                    id, name, idStr);
            }
            return JSValueMakeUndefined(ctx);
        }

        // Reserved channels: the '__' prefix is the host's, so none of these
        // ever reaches a mod's listeners.
        if (name == "__escapecapture") {   // 0.28.0: the page owns Escape ('1') or hands it back ('0')
            SetViewEscapeCapture(id, arg == "1");
            return JSValueMakeUndefined(ctx);
        }
        if (name == "__prepaint") {   // 0.31.11: SetViewPrepaint from the page itself ('1' / '0')
            SetViewPrepaint(id, arg == "1");
            return JSValueMakeUndefined(ctx);
        }
        if (name == "__gamepad") {   // 1.6.0: the page core's gamepad helper (GamepadScript.h)
            OnPageGamepad(id, arg);
            return JSValueMakeUndefined(ctx);
        }
        // Retired: text focus comes from the EditorListener now. Pages may still send them (a mod's editor shim
        // reports its iframes on '__textfocus'); they are dropped here so no mod listener gets them.
        if (name == "__textfocus" || name == "__imecaret") return JSValueMakeUndefined(ctx);
        if (name == "__key") {   // the host's own keyboard page only: the keys go to the UI-mode view
            if (id == static_cast<ViewId>(s_kbViewId.load())) OnOwnKeyboardKey(arg);
            return JSValueMakeUndefined(ctx);
        }
        if (name == "__sound") {   // 0.29.0: page-driven UI sound; "hover:<name>" is throttled
            PlayPageSound(id, arg);
            return JSValueMakeUndefined(ctx);
        }
        if (name == "__hosttheme") {   // 0.31.9: the page's VR keyboard and cursor colours (magelight.hostTheme)
            OnPageHostTheme(id, arg);
            return JSValueMakeUndefined(ctx);
        }

        MlView::Listener l;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            if (MlView* v = FindViewLocked(id)) {
                auto it = v->listeners.find(name);
                if (it != v->listeners.end()) l = it->second;
            }
        }
        if (l.fnEx) l.fnEx(id, arg.c_str(), l.user);
        else if (l.fn) l.fn(arg.c_str());
        else SKSE::log::warn("Magelight: JS called unregistered listener '{}' (view {})", name, id);
        return JSValueMakeUndefined(ctx);
    }

    // 0.29.0, JS thread: validate here, play on the game thread.
    // Only a VISIBLE view may play — a hidden or hibernated page that fires a
    // sound on load must not be heard. 'hover:<name>' is throttled to one per
    // ~60 ms per view so a mouse sweep over a list cannot spam the engine.
    static void PlayPageSound(ViewId id, const std::string& arg)
    {
        std::string name = arg;
        bool hover = false;
        if (name.rfind("hover:", 0) == 0) { hover = true; name.erase(0, 6); }
        if (name.empty() || name.size() > 128) return;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(id);
            if (!v || !v->visible || v->dormant) return;
        }
        if (hover && !Sound::HoverAllowed(id)) return;
        GameTask::Post([id, name]() { Sound::Play(name, id); });
    }

    // "lit FFD8CCB0 shade ..." or "none (the host's colours)", for the log.
    static std::string DescribeCursorTint(const CursorTintSet& t)
    {
        if (t.Empty()) return "none (the host's colours)";
        char buf[96];
        std::snprintf(buf, sizeof(buf), "lit %08X shade %08X ink %08X glow %08X ibeam %08X", static_cast<unsigned>(t.lit),
                      static_cast<unsigned>(t.shade), static_cast<unsigned>(t.ink), static_cast<unsigned>(t.glow),
                      static_cast<unsigned>(t.ibeam));
        return buf;
    }

    // 0.31.9, JS thread: magelight.hostTheme(obj | null). The keyboard page types into the UI-mode view, so each
    // part is checked here in full before it is stored: a part with a bad value is refused alone and changes
    // nothing (1.1.0; the other part still applies), a message over 2 KB, not a JSON object or with v != 1 changes
    // nothing at all, and only the caller's own slots change. Log lines are rate-limited per view (ThemeLogRate).
    static constexpr std::size_t kHostThemeMaxBytes = 2048;

    static void RefuseHostTheme(ViewId id, const std::string& why)
    {
        std::uint32_t n = 0;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            if (MlView* v = FindViewLocked(id)) n = ++v->themeLog.refusals;
        }
        if (n && n <= kThemeRefusalsLogged)
            SKSE::log::warn("Magelight: view {} hostTheme refused - {} (refusal {}{})", id, why, n,
                            n == kThemeRefusalsLogged ? "; this view's later refusals are counted, not logged" : "");
    }

    // The page's reply on the reserved '__hostthemeresult' channel (1.1.0), sent only when its message asked with
    // "reply": true, so a page that never listens is sent nothing: each part "set", "cleared", "kept" or "refused",
    // and why the first refused part was refused.
    static void ReplyHostTheme(ViewId id, const char* keyboard, const char* cursor, const std::string& why)
    {
        nlohmann::json j{ { "keyboard", keyboard }, { "cursor", cursor } };
        if (!why.empty()) j["why"] = why;
        InteropCall(id, "__hostthemeresult", j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    }

    static void OnPageHostTheme(ViewId id, const std::string& arg)
    {
        if (id == static_cast<ViewId>(s_kbViewId.load())) return RefuseHostTheme(id, "the keyboard page cannot set a theme");
        if (arg.size() > kHostThemeMaxBytes) return RefuseHostTheme(id, "the message is over 2 KB");
        enum class Part { Keep, Clear, Set };   // the part absent, null, or an object
        Part kbPart = Part::Keep, cursorPart = Part::Keep;
        // A part with a bad value is refused alone (1.1.0) and the other part still applies: a keyboard that fails
        // the contrast rules no longer drops the cursor tint sent with it.
        bool kbRefused = false, cursorRefused = false;
        std::string why;   // the first refused part's reason, for the reply
        bool reply = false;
        HostTheme::KbTheme kb{};
        std::string notes;   // optional colours ResolveKeyboard discarded, a low-contrast accent
        CursorTintSet tint;
        if (arg.empty()) {   // hostTheme(null)
            kbPart = cursorPart = Part::Clear;
        } else {
            nlohmann::json j;
            try {
                j = nlohmann::json::parse(arg);
            } catch (...) {
                return RefuseHostTheme(id, "the message is not JSON");
            }
            if (!j.is_object()) return RefuseHostTheme(id, "the message is not a JSON object");
            if (const auto r = j.find("reply"); r != j.end() && r->is_boolean()) reply = r->get<bool>();
            const auto ver = j.find("v");
            if (ver == j.end() || !ver->is_number_integer() || ver->get<std::int64_t>() != 1) {
                RefuseHostTheme(id, "\"v\" must be 1");
                if (reply) ReplyHostTheme(id, "refused", "refused", "\"v\" must be 1");
                return;
            }
            const auto refusePart = [&](bool& refused, const std::string& reason) {
                refused = true;
                if (why.empty()) why = reason;
                RefuseHostTheme(id, reason);
            };
            // A colour of a part: false when absent or bad; `bad` names the part's first bad one. Unknown keys are ignored.
            std::string bad;
            const auto colour = [&bad](const nlohmann::json& part, const char* partName, const char* key, std::uint32_t& rgb) {
                const auto it = part.find(key);
                if (it == part.end()) return false;
                if (it->is_string() && HostTheme::ParseHex(it->get_ref<const std::string&>(), rgb)) return true;
                if (bad.empty()) bad = std::string(partName) + "." + key + " is not a #rrggbb colour";
                return false;
            };
            if (const auto k = j.find("keyboard"); k != j.end()) {
                if (k->is_null()) {
                    kbPart = Part::Clear;
                } else if (!k->is_object()) {
                    refusePart(kbRefused, "\"keyboard\" must be an object or null");
                } else {
                    HostTheme::KbInput in;
                    for (int i = 0; i < HostTheme::kKbTokens; ++i)
                        in.given[i] = colour(*k, "keyboard", HostTheme::kKbNames[i], in.rgb[i]);
                    std::string resolveWhy;
                    if (!bad.empty()) refusePart(kbRefused, bad);
                    else if (!HostTheme::ResolveKeyboard(in, kb, resolveWhy, &notes)) refusePart(kbRefused, "keyboard: " + resolveWhy);
                    else kbPart = Part::Set;
                }
            }
            bad.clear();
            if (const auto c = j.find("cursor"); c != j.end()) {
                if (c->is_null()) {
                    cursorPart = Part::Clear;
                } else if (!c->is_object()) {
                    refusePart(cursorRefused, "\"cursor\" must be an object or null");
                } else {
                    std::uint32_t* const slots[] = { &tint.lit, &tint.shade, &tint.ink, &tint.glow, &tint.ibeam };
                    static_assert(std::size(slots) == HostTheme::kCursorNames.size());
                    for (std::size_t i = 0; i < std::size(slots); ++i) {
                        std::uint32_t rgb = 0;
                        if (colour(*c, "cursor", HostTheme::kCursorNames[i], rgb)) *slots[i] = 0xFF000000u | rgb;
                    }
                    if (!bad.empty()) refusePart(cursorRefused, bad);
                    else cursorPart = Part::Set;   // a colour left out keeps the host's, as in CursorTint
                }
            }
        }
        // Both parts in one critical section. The cursor part is SetViewCursorTint's store without its per-call log
        // line; the one line here is built from what is stored and written after unlocking.
        std::string line;
        bool logIt = false;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(id);
            if (!v || v->destroyPending) return;
            bool kbChanged = false, cursorChanged = false;
            if (kbPart != Part::Keep) {
                const std::optional<HostTheme::KbTheme> next = kbPart == Part::Set ? std::optional(kb) : std::nullopt;
                kbChanged = v->kbTheme != next;
                v->kbTheme = next;
                if (kbChanged && id == static_cast<ViewId>(s_uiModeView.load())) QueueKeyboardThemeLocked();
            }
            if (cursorPart != Part::Keep) {
                cursorChanged = !(v->cursorTint == tint);
                v->cursorTint = tint;
            }
            std::uint32_t skipped = 0;
            logIt = (kbChanged || cursorChanged) && v->themeLog.Admit(GetTickCount64(), skipped);
            if (logIt) {
                if (kbChanged)
                    line = "keyboard " + (v->kbTheme ? HostTheme::KeyboardJson(*v->kbTheme) : std::string("none (the host's look)"));
                if (cursorChanged) line += std::string(line.empty() ? "" : "; ") + "cursor tint " + DescribeCursorTint(v->cursorTint);
                if (kbChanged && !notes.empty()) line += "; " + notes;
                else notes.clear();
                if (skipped) line += " (" + std::to_string(skipped) + " earlier changes not logged)";
            }
        }
        if (reply) {
            const auto status = [](Part part, bool refused) {
                return refused ? "refused" : part == Part::Set ? "set" : part == Part::Clear ? "cleared" : "kept";
            };
            ReplyHostTheme(id, status(kbPart, kbRefused), status(cursorPart, cursorRefused), why);
        }
        if (!logIt) return;
        if (!notes.empty()) SKSE::log::warn("Magelight: view {} hostTheme (page): {}", id, line);
        else SKSE::log::info("Magelight: view {} hostTheme (page): {}", id, line);
    }

    // Render thread. full (OnWindowObjectReady, a fresh window object): the
    // native dispatcher, the page core (__MAGELIGHT__, window.magelight and
    // the document listeners) and one shim per listener. !full (shimsDirty, a
    // listener registered after load): a fresh __mlNative and every listener
    // shim, early ones included; the page keeps its window.magelight and every
    // subscription on it. The core also guards itself with window.__mlCore:
    // it runs once per window, never twice.
    static void InstallBridgeShims(MlView& v, bool full)
    {
        if (!v.ul) return;
        auto ctxScope = v.ul->LockJSContext();
        if (!ctxScope) return;   // documented as possible (a frame that cannot run script); never seen
        JSContextRef ctx = (*ctxScope);
        JSObjectRef global = JSContextGetGlobalObject(ctx);

        // A real function (Function.prototype: call/apply/bind) that carries the view id as its user data, which
        // is how the dispatcher knows which page called it. No finalizer: the id owns nothing, and a finalizer
        // would run at some later collection with the context group's lock held (JSObjectRef.h).
        JSStringRef dispName = JSStringCreateWithUTF8CString("__mlNative");
        const JSObjectRef fnObj = JSObjectMakeFunctionWithData(ctx, dispName, &MlNativeDispatch,
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(v.id)), nullptr);
        if (fnObj) JSObjectSetProperty(ctx, global, dispName, fnObj, kJSPropertyAttributeNone, nullptr);
        JSStringRelease(dispName);

        // Host marker FIRST — pages detect which host they run on at their
        // first line (runtime host detection for the SA frontend; the prior
        // host installed no page global, so presence == Magelight).
        // __MAGELIGHT__ = who am I / what can the host do; magelight = the two
        // verbs every page gets (send/on/off) with pre-subscribe buffering,
        // so a payload that lands before the app mounts is replayed, not
        // lost. InteropCall to a channel with no window shim routes into
        // _dispatch (see DrainBridgeQueues) — plain HTML and the SDK share it.
        const std::string idStr0 = std::to_string(v.id);
        std::string shims;
        if (full) shims =
            "if(!window.__mlCore){Object.defineProperty(window,'__mlCore',{value:true});"
            "window.__MAGELIGHT__={version:'" PLUGIN_VERSION "',versionNumber:" +
            std::to_string(PLUGIN_VERSION_MAJOR * 10000 + PLUGIN_VERSION_MINOR * 100 + PLUGIN_VERSION_PATCH) +
            ",viewId:" + idStr0 + "," + Api4::ViewIdentityJson(v.id) +
            ",capabilities:" + Api4::CapabilitiesJson() +
            ",dev:" + (s_devMode.load() ? "true" : "false") +
            ",runtimeUrl:" + MlJsonQuote(PathToFileUrl(s_runtimeDir) + "/") + "};"
            "window.magelight=(function(){var subs={},buf={},id='" + idStr0 + "';"
            "function dispatch(ch,arg){var s=subs[ch];if(!s||!s.length){(buf[ch]=buf[ch]||[]).push(arg);return;}"
            "for(var i=0;i<s.length;i++){try{s[i](arg);}catch(e){console.error('[magelight] handler for '+ch+' threw',e);}}}"
            "return{send:function(ch,p){var a=p==null?'':(typeof p==='string'?p:JSON.stringify(p));__mlNative(id,ch,a);},"
            // 0.29.0: magelight.sound('click') -> the reserved '__sound' channel (host plays it through the game's audio)
            "sound:function(n){try{__mlNative(id,'__sound',n==null?'':String(n));}catch(e){}},"
            // 0.31.9: magelight.hostTheme({v:1,keyboard:{..},cursor:{..}}) or (null) -> the reserved '__hosttheme' channel
            "hostTheme:function(o){try{__mlNative(id,'__hosttheme',o==null?'':JSON.stringify(o));}catch(e){}},"
            "on:function(ch,fn){(subs[ch]=subs[ch]||[]).push(fn);var b=buf[ch];if(b){delete buf[ch];for(var i=0;i<b.length;i++){try{fn(b[i]);}catch(e){console.error('[magelight] replay for '+ch+' threw',e);}}}"
            "return function(){window.magelight.off(ch,fn);};},"
            "off:function(ch,fn){var s=subs[ch];if(!s)return;var i=s.indexOf(fn);if(i>=0)s.splice(i,1);},"
            "_dispatch:dispatch,pending:function(ch){return (buf[ch]||[]).length;}};})();"
            // UI sounds (0.29.0): the markup convention. data-ml-sound="click"
            // plays on click (keyboard activation raises click too), data-ml-sound-hover
            // ="focus" on entering the element. Delegated on document so a manifest-only
            // mod gets button sounds from one attribute; 'mouseover' (bubbles) with a
            // last-element latch, since 'mouseenter' does not bubble. The host throttles
            // hover per view.
            "(function(){var id='" + idStr0 + "',last=null;"
            "function up(e,a){while(e&&e.getAttribute){if(e.hasAttribute(a))return e;e=e.parentNode;}return null;}"
            "document.addEventListener('click',function(ev){var t=up(ev.target,'data-ml-sound');if(!t)return;"
            // A <label> wrapping its control re-dispatches the click on the control: play that one only.
            "if(t.control&&!t.control.disabled&&!t.control.contains(ev.target)&&t.contains(t.control))return;"
            "var n=t.getAttribute('data-ml-sound');if(n)try{__mlNative(id,'__sound',n);}catch(e){}},true);"
            "document.addEventListener('mouseover',function(ev){var t=up(ev.target,'data-ml-sound-hover');if(t===last)return;last=t;if(!t)return;"
            "var n=t.getAttribute('data-ml-sound-hover');if(n)try{__mlNative(id,'__sound','hover:'+n);}catch(e){}},true);"
            "})();" +
            // Middle, back and forward buttons: no autoscroll, auxclick instead of click (InputCore.h).
            std::string(Input::kPointerGuardScript) +
            // IME: the page core draws the IME's candidate list the host hands it on '__ime' (ImeCore.h), in a box at
            // the anchor, the composition character the IME's window would open at, in viewport CSS px (the
            // composition itself is the field's own: Editor::SetComposition). Every message is first a cancelable
            // 'magelight:ime' window event carrying it whole; a page that draws its own list cancels it, or sets
            // data-ml-ime=off on the field, an ancestor, or an iframe on the way to it (<html>: the whole page). The
            // --ml-ime-bg/-fg/-border/-font/-selected-bg/-selected-fg/-max-width properties style the box; without
            // --ml-ime-font it takes the focused field's font family and rendered size, and without a colour of its own
            // the page's --ml-tooltip-* one, else one read from the field (colors).
            "(function(){if(!window.magelight)return;var box=null,edge=null,selBg='#5c5c60',selFg='#ffffff';"
            "function off(e){while(e&&e.getAttribute){if(e.getAttribute('data-ml-ime')==='off')return true;e=e.parentNode;}return false;}"
            // The focused element through same-origin iframes (an editor written into about:blank), and the frames on
            // the way.
            "function deep(){var e=document.activeElement||document.documentElement,fr=[];"
            "for(var i=0;e&&i<16&&(e.tagName||'').toUpperCase()==='IFRAME';i++){var d=null;try{d=e.contentDocument;}catch(x){}"
            "if(!d||!d.activeElement)break;fr.push(e);e=d.activeElement;}return{e:e,frames:fr};}"
            "function offFocus(){var f=deep();if(off(f.e))return true;for(var i=0;i<f.frames.length;i++)if(off(f.frames[i]))return true;return false;}"
            // The CSS zoom an element is drawn at within its document (an iframe's content is drawn 1:1 whatever zoom
            // its frame is under).
            "function zoomOf(e){var z=1;for(;e&&e.nodeType===1;e=e.parentNode){var v=parseFloat(e.ownerDocument.defaultView.getComputedStyle(e).zoom);if(v>0)z*=v;}return z;}"
            // The box's font: --ml-ime-font when the page sets it, else the focused field's family at the size it is
            // drawn at, carried into the box's own px (its zoom over the box's), line-height 1.4; 14px sans-serif with
            // no field. Padding and the default max-width are in em, so they follow it.
            // A custom property the page sets, as seen from the box ('' when unset).
            "function cv(e,n){var r='';try{r=getComputedStyle(e).getPropertyValue(n);}catch(x){}return r&&r.trim()?r.trim():'';}"
            // The focused field (through same-origin iframes), or null when focus is on no element of its own.
            "function field(){var f=deep().e;return f&&f.nodeType===1&&f!==f.ownerDocument.documentElement&&"
            "(f!==f.ownerDocument.body||f.isContentEditable)?f:null;}"
            "function font(b){var s=b.style;if(cv(b,'--ml-ime-font')){s.font='var(--ml-ime-font)';return;}"
            "var f=null,px=14,fam='sans-serif';"
            "try{f=field();if(f){var cs=f.ownerDocument.defaultView.getComputedStyle(f),p=parseFloat(cs.fontSize);"
            "if(p>0){px=p*zoomOf(f)/zoomOf(b);if(cs.fontFamily)fam=cs.fontFamily;}}}catch(x){}"
            "s.font=(Math.round(px*100)/100)+'px/1.4 '+fam;}"
            // Colours, read once per change: rgba() of a computed colour, the WCAG contrast of two.
            "function rgba(s){var m=String(s||'').match(/-?[\\d.]+/g);if(!m||m.length<3)return null;return[+m[0],+m[1],+m[2],m.length>3?+m[3]:1];}"
            "function css(c){return 'rgb('+Math.round(c[0])+','+Math.round(c[1])+','+Math.round(c[2])+')';}"
            "function mix(a,b,t){return[a[0]+(b[0]-a[0])*t,a[1]+(b[1]-a[1])*t,a[2]+(b[2]-a[2])*t,1];}"
            "function lum(c){function f(x){x/=255;return x<=0.03928?x/12.92:Math.pow((x+0.055)/1.055,2.4);}"
            "return 0.2126*f(c[0])+0.7152*f(c[1])+0.0722*f(c[2]);}"
            "function ratio(a,b){var x=lum(a),y=lum(b);return(Math.max(x,y)+0.05)/(Math.min(x,y)+0.05);}"
            // The gradients of a computed background-image, top layer first ({k: linear|radial|conic, a: its arguments});
            // url() images are not colours and are left out.
            "function grads(s){var out=[],re=/(?:repeating-)?(linear|radial|conic)-gradient\\(/g,m;"
            "while((m=re.exec(s))){var j=re.lastIndex,d=1;for(;j<s.length&&d>0;j++){if(s[j]==='(')d++;else if(s[j]===')')d--;}"
            "out.push({k:m[1],a:s.slice(re.lastIndex,j-1)});re.lastIndex=j;}return out;}"
            // A gradient's colour at the point: along a linear one, the colour at the point's place on its line (t, 0..1
            // from its start); a radial or conic one, its stops averaged by their alpha.
            "function gcol(g,er,pr){var st=[],re=/rgba?\\([^)]*\\)([^,]*)/g,m;while((m=re.exec(g.a))){var c=rgba(m[0]),q=/(-?[\\d.]+)%/.exec(m[1]);"
            "if(c)st.push({c:c,p:q?(+q[1])/100:null});}if(!st.length)return null;"
            "if(g.k!=='linear'){var w=0,r=[0,0,0,0];for(var i=0;i<st.length;i++){var s=st[i].c;w+=s[3];r[0]+=s[0]*s[3];r[1]+=s[1]*s[3];r[2]+=s[2]*s[3];}"
            "return w>0?[r[0]/w,r[1]/w,r[2]/w,w/st.length]:null;}"
            "for(var i=0;i<st.length;i++)if(st[i].p==null)st[i].p=st.length>1?i/(st.length-1):0;"
            "var head=g.a.slice(0,g.a.indexOf('rgb')),deg=180,tm=/to\\s+(top|bottom|left|right)(?:\\s+(top|bottom|left|right))?/.exec(head);"
            "if(tm){var A={top:0,right:90,bottom:180,left:270},x=A[tm[1]];if(tm[2]){var y=A[tm[2]];x=(x===0&&y===270)||(x===270&&y===0)?315:(x+y)/2;}deg=x;}"
            "else{var dm=/(-?[\\d.]+)(deg|turn|rad|grad)/.exec(head);if(dm){var n=+dm[1];deg=dm[2]==='turn'?n*360:dm[2]==='rad'?n*180/Math.PI:dm[2]==='grad'?n*0.9:n;}}"
            "var th=deg*Math.PI/180,dx=Math.sin(th),dy=-Math.cos(th),L=Math.abs(er.width*dx)+Math.abs(er.height*dy),t=0.5;"
            "if(L>0)t=0.5+(((pr.left+pr.right)/2-(er.left+er.width/2))*dx+((pr.top+pr.bottom)/2-(er.top+er.height/2))*dy)/L;"
            "t=Math.max(0,Math.min(1,t));if(t<=st[0].p)return st[0].c;"
            "for(var i=1;i<st.length;i++){if(t<=st[i].p){var u=(t-st[i-1].p)/Math.max(1e-6,st[i].p-st[i-1].p),A0=st[i-1].c,B0=st[i].c;"
            "return[A0[0]+(B0[0]-A0[0])*u,A0[1]+(B0[1]-A0[1])*u,A0[2]+(B0[2]-A0[2])*u,A0[3]+(B0[3]-A0[3])*u];}}return st[st.length-1].c;}"
            // What shows behind the field: the backgrounds painted under its centre (an element's gradient layers, then
            // its colour), composited front to back until opaque, out of an iframe into its frame's page. A ground still
            // see-through at the top shows the game behind the overlay, which black stands in for; one with no colour at
            // all is null. The layers come from elementsFromPoint, which leaves out pointer-events:none elements (a
            // backdrop layer behind the field, a vignette in front of it), so it runs under a constructed sheet that
            // makes every element hit-testable, adopted for the call and taken off again (no DOM change for the page to
            // observe); the box and its edge marker are skipped. Its point is in viewport px, a rect's in its own
            // zoom's units (zoomOf). Without elementsFromPoint, the field and its ancestors.
            "function behind(f){var a=[0,0,0,0];"
            "function over(c){if(!c||!(c[3]>0)||a[3]>=0.995)return;var k=(1-a[3])*Math.min(1,c[3]);a=[a[0]+k*c[0],a[1]+k*c[1],a[2]+k*c[2],a[3]+k];}"
            "function paint(e,pr){var cs=e.ownerDocument.defaultView.getComputedStyle(e),gs=grads(cs.backgroundImage||'');"
            "if(gs.length){var er=e.getBoundingClientRect();for(var j=0;j<gs.length;j++)over(gcol(gs[j],er,pr));}over(rgba(cs.backgroundColor));}"
            "function stack(d,x,y){var w=d.defaultView,old=d.adoptedStyleSheets,s=null;"
            "try{s=new w.CSSStyleSheet();s.replaceSync('*,*::before,*::after{pointer-events:auto!important}');d.adoptedStyleSheets=old.concat([s]);}catch(x2){s=null;}"
            "try{return d.elementsFromPoint(x,y);}finally{if(s)d.adoptedStyleSheets=old;}}"
            "try{if(!f.ownerDocument.elementsFromPoint)throw 0;var d=f.ownerDocument,fz=zoomOf(f),q=f.getBoundingClientRect(),"
            "x=(q.left+q.right)/2*fz,y=(q.top+q.bottom)/2*fz;"
            "for(var n=0;d&&n<8&&a[3]<0.995;n++){var els=stack(d,x,y);"
            "for(var i=0;i<els.length&&a[3]<0.995;i++){var e=els[i];if(e===box||e===edge)continue;var z=zoomOf(e);"
            "paint(e,{left:x/z,right:x/z,top:y/z,bottom:y/z});}"
            "var fe=d.defaultView.frameElement;if(!fe)break;var r=fe.getBoundingClientRect(),zz=zoomOf(fe);"
            "x=(r.left+fe.clientLeft)*zz+x;y=(r.top+fe.clientTop)*zz+y;d=fe.ownerDocument;}}"
            "catch(x3){a=[0,0,0,0];for(var e=f,pt=f,i=0;e&&i<64&&a[3]<0.995;i++){if(e.nodeType===1)paint(e,pt.getBoundingClientRect());"
            "var p=e.parentNode;if(p&&p.nodeType===1)e=p;else{var w2=e.ownerDocument.defaultView;e=w2&&w2.frameElement;pt=e;}}}"
            "return a[3]>0.01?[a[0],a[1],a[2],1]:null;}"
            // Each colour: the page's --ml-ime-* one, else its --ml-tooltip-* one (bg, fg, border), else the field's
            // (its text colour, what shows behind it, its border or a mix of the two), else the defaults. Unless the
            // page set --ml-ime-bg or -fg, text and ground under 3:1 apart fall back to the defaults. The selected row:
            // --ml-ime-selected-*, else the defaults over the default colours, else the ground mixed toward the text
            // with the text on it, or the two swapped when that is under 3:1.
            "function colors(b){var s=b.style,fg=null,bg=null,bd=null;"
            "try{var f=field();if(f){var cs=f.ownerDocument.defaultView.getComputedStyle(f);fg=rgba(cs.color);bg=behind(f);"
            "var bw=parseFloat(cs.borderTopWidth),bst=cs.borderTopStyle,bc=rgba(cs.borderTopColor);"
            "if(bw>0&&bst!=='none'&&bst!=='hidden'&&bc&&bc[3]>0.1)bd=bc;}}catch(x){fg=bg=bd=null;}"
            "if(!fg||fg[3]<0.5||!bg)fg=bg=bd=null;"
            "function pick(n,d){if(cv(b,'--ml-ime-'+n))return 'var(--ml-ime-'+n+')';"
            "if(cv(b,'--ml-tooltip-'+n))return 'var(--ml-tooltip-'+n+')';return d;}"
            "var own=cv(b,'--ml-ime-bg')||cv(b,'--ml-ime-fg');"
            "s.background=pick('bg',bg?css(bg):'#1e1e20');s.color=pick('fg',fg?css(fg):'#f0f0f0');"
            "s.border='1px solid '+pick('border',bd?css(bd):bg?css(mix(bg,fg,0.35)):'#5c5c60');"
            "var g=getComputedStyle(b),cb=rgba(g.backgroundColor),cf=rgba(g.color),dflt=false;"
            "if(!own&&(!cb||!cf||ratio(cf,cb)<3)){s.background='#1e1e20';s.color='#f0f0f0';"
            "if(!cv(b,'--ml-ime-border'))s.border='1px solid #5c5c60';cb=[30,30,32,1];cf=[240,240,240,1];}"
            "dflt=cb&&cf&&cb[0]===30&&cb[1]===30&&cb[2]===32&&cf[0]===240&&cf[1]===240&&cf[2]===240;"
            "var sb='#5c5c60',sf='#ffffff';"
            "if(!dflt&&cb&&cf){var m=mix(cb,cf,0.3);if(ratio(cf,m)>=3){sb=css(m);sf=css(cf);}else{sb=css(cf);sf=css(cb);}}"
            "selBg=cv(b,'--ml-ime-selected-bg')?'var(--ml-ime-selected-bg)':sb;"
            "selFg=cv(b,'--ml-ime-selected-fg')?'var(--ml-ime-selected-fg)':sf;}"
            // The box sits just below the tooltip (2147483646) and the devMode error banner (2147483647). It lives in
            // <body>, so a page's UI-scale zoom on <body> scales it like the page; page CSS stays out of it and its rows
            // (all:initial). `edge`, a zero-size marker at the window's bottom-right, measures the window in the units
            // getBoundingClientRect reports there, whatever zoom applies.
            "function el(){if(box&&box.parentNode)return box;box=document.createElement('div');box.id='__ml_ime';box.setAttribute('aria-hidden','true');"
            "edge=document.createElement('div');var es=edge.style;es.all='initial';es.position='fixed';es.right='0';es.bottom='0';"
            "es.width='0';es.height='0';es.visibility='hidden';es.pointerEvents='none';"
            "var s=box.style;s.all='initial';s.position='fixed';s.left='0';s.top='0';s.zIndex='2147483645';s.pointerEvents='none';s.display='none';"
            "s.boxSizing='border-box';s.padding='0.2143em 0';s.borderRadius='0.2143em';s.overflow='hidden';"
            "s.maxWidth='min(var(--ml-ime-max-width,34em),90vw)';"
            "s.boxShadow='0 2px 8px rgba(0,0,0,0.45)';var host=document.body||document.documentElement;host.appendChild(box);host.appendChild(edge);return box;}"
            "function hide(){if(box)box.style.display='none';}"
            "function row(t,on){var d=document.createElement('div'),s=d.style;s.all='initial';s.display='block';s.font='inherit';s.color='inherit';"
            "s.padding='0.0714em 0.714em';s.whiteSpace='nowrap';s.overflow='hidden';s.textOverflow='ellipsis';d.textContent=t;"
            "if(on){s.background=selBg;s.color=selFg;}return d;}"
            // Below the anchor, else above it, kept in the window. Placement works in getBoundingClientRect's units: `k`
            // is those units per CSS px of the box's own left/top (measured), the window's size in them is the edge
            // marker's position, and the anchor's viewport px scale into them by that size over innerWidth/innerHeight.
            "function draw(m){var c=m.candidates||[],a=m.anchor;if(!c.length||!a){hide();return;}"
            "var b=el();font(b);colors(b);b.textContent='';for(var i=0;i<c.length;i++)b.appendChild(row((i+1)+'. '+c[i],i===m.selectedIndex));"
            "b.style.display='block';b.style.left='0';b.style.top='0';var b0=b.getBoundingClientRect();b.style.left='100px';"
            "var k=(b.getBoundingClientRect().left-b0.left)/100;if(!(k>0))k=1;b.style.left='0';"
            "var er=edge.getBoundingClientRect(),w=er.left,h=er.top,u=innerWidth>0&&w>0?w/innerWidth:1,v=innerHeight>0&&h>0?h/innerHeight:1;"
            "var br=b.getBoundingClientRect(),g=4*k,l=a.x*u,y=a.y*v,ah=a.h*v,fe=document.activeElement;"
            // A single-line field of this page: the field's own top and bottom, so the box keeps clear of its border
            // (its rect is in units of its own zoom: carried into the box's).
            "if(fe&&(fe.tagName||'').toUpperCase()==='INPUT'){var fq=fe.getBoundingClientRect(),fz=zoomOf(fe)/zoomOf(b),"
            "fr={left:fq.left*fz,right:fq.right*fz,top:fq.top*fz,bottom:fq.bottom*fz},cy=y+ah/2;"
            "if(l>=fr.left-1&&l<=fr.right+1&&cy>=fr.top&&cy<=fr.bottom){y=fr.top;ah=fr.bottom-fr.top;}}"
            "var t=y+ah+2*k;"
            "if(l+br.width>w-g)l=w-g-br.width;if(l<g)l=g;if(t+br.height>h-g)t=Math.max(g,y-2*k-br.height);"
            "b.style.left=Math.round((l-b0.left)/k)+'px';b.style.top=Math.round((t-b0.top)/k)+'px';}"
            "window.magelight.on('__ime',function(a){var m;try{m=JSON.parse(a);}catch(e){return;}"
            "var go=true;try{go=window.dispatchEvent(new CustomEvent('magelight:ime',{cancelable:true,detail:m}));}catch(x){}"
            "if(go&&m.state!=='end'&&!offFocus())draw(m);else hide();});})();"
            // Tooltips (1.2.0): the page draws the title the host hands it on '__tooltip' ({t, c}: the text, the
            // cursor's height in CSS px; {l:1} = the pointer left the page, so nothing shows until it moves here
            // again). 400 ms on a title, below the cursor, kept on screen; a click, a key or the wheel hides it and the
            // same title stays hidden until it changes; a scroll hides it until the pointer moves. Opt out with
            // data-ml-tooltip=off on an element (or <html>), or by cancelling the 'magelight:tooltip' event; the
            // --ml-tooltip-bg/-fg/-border/-font properties style it.
            "(function(){if(!window.magelight)return;var tip=null,text='',shown=false,timer=0,x=0,y=0,c=24,over=null,muted=null,away=false;"
            "function off(e){while(e&&e.getAttribute){if(e.getAttribute('data-ml-tooltip')==='off')return true;e=e.parentNode;}return false;}"
            "function el(){if(tip)return tip;tip=document.createElement('div');tip.id='__ml_tip';tip.setAttribute('aria-hidden','true');"
            "var s=tip.style;s.position='fixed';s.left='0';s.top='0';s.zIndex='2147483646';s.pointerEvents='none';s.display='none';"
            "s.whiteSpace='pre-line';s.boxSizing='border-box';s.padding='4px 8px';s.borderRadius='3px';"
            "s.background='var(--ml-tooltip-bg,#1e1e20)';s.color='var(--ml-tooltip-fg,#f0f0f0)';"
            "s.border='1px solid var(--ml-tooltip-border,#5c5c60)';s.font='var(--ml-tooltip-font,12px/1.35 sans-serif)';"
            "s.boxShadow='0 2px 8px rgba(0,0,0,0.45)';document.documentElement.appendChild(tip);return tip;}"
            "function hide(){clearTimeout(timer);timer=0;if(shown&&tip)tip.style.display='none';shown=false;}"
            "function place(){var t=tip;t.style.left='0';t.style.top='0';var w=window.innerWidth,h=window.innerHeight,r=t.getBoundingClientRect(),l=x+2,o=y+c+4;"
            "if(l+r.width>w-4)l=Math.max(4,w-4-r.width);if(o+r.height>h-4)o=Math.max(4,y-4-r.height);"
            "t.style.left=Math.round(l)+'px';t.style.top=Math.round(o)+'px';}"
            "function show(){timer=0;if(!text||text===muted||away)return;var go=!off(over);"
            "if(go)try{go=window.dispatchEvent(new CustomEvent('magelight:tooltip',{cancelable:true,detail:{text:text,target:over}}));}catch(e){}"
            "if(!go){hide();muted=text;return;}"
            "var t=el();t.style.maxWidth=Math.max(160,Math.round(window.innerWidth*0.4))+'px';t.textContent=text;"
            "t.style.display='block';shown=true;place();}"
            "function arm(){clearTimeout(timer);timer=setTimeout(show,400);}"
            "function quiet(){if(text)muted=text;hide();}"
            "window.magelight.on('__tooltip',function(a){var m;try{m=JSON.parse(a);}catch(e){return;}"
            "if(m.l){away=true;hide();return;}if(typeof m.c==='number')c=m.c;var t=typeof m.t==='string'?m.t:'';"
            "if(t!==muted)muted=null;if(t===text&&(shown||timer))return;text=t;if(!t){hide();return;}if(shown)show();else arm();});"
            "document.addEventListener('mousemove',function(ev){x=ev.clientX;y=ev.clientY;away=false;if(text&&!shown&&!timer&&text!==muted)arm();},true);"
            "document.addEventListener('mouseover',function(ev){over=ev.target;},true);"
            "document.addEventListener('mousedown',quiet,true);document.addEventListener('keydown',quiet,true);"
            "document.addEventListener('wheel',quiet,true);document.addEventListener('scroll',hide,true);"
            "})();" +
            // Gamepad (1.6.0): window.magelight.gamepad, idle until the host sends '__gamepad' (GamepadScript.h).
            std::string(Gamepad::kPageScript) +
            "}";
        std::size_t count = 0;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            const std::string idStr = idStr0;
            for (const auto& [n, l] : v.listeners) {
                (void)l;
                // JsonQuote: the listener name is mod-supplied — a quote in it
                // must not break (or break out of) the generated script.
                shims += "window[" + JsonQuote(n) + "]=function(a){__mlNative('" + idStr +
                         "'," + JsonQuote(n) + ",a==null?'':String(a));};";
            }
            count = v.listeners.size();
            v.shimsDirty = false;  // this install covers everything registered so far
        }
        // Named, so an error thrown later inside a core function points at magelight:bridge in its stack.
        std::string ignored;
        JsBridge::ErrorInfo err;
        if (!JsBridge::Evaluate(ctx, shims, "magelight:bridge", ignored, err)) {
            // The page is left without window.magelight or some listener shims: name it rather than let it go blank.
            SKSE::log::error("Magelight: the bridge script ({}) failed on view {}: {}",
                full ? "page core" : "listener shims", v.id, JsBridge::LogText(err));
        }
        if (full) SKSE::log::info("Magelight: bridge installed on view {} ({} JS listeners)", v.id, count);
        else SKSE::log::info("Magelight: {} JS listener shim(s) refreshed on view {}", count, v.id);
    }

    // What a view may show as its main frame, and what the bridge is installed on: the banner (LoadHTML,
    // about:) and the HTML files MlFileSystem serves, which are the documents that carry a policy. A file: SVG
    // runs script with no policy and keeps the file: origin, and a data: document inherits none, so a
    // navigation to either is stopped like a remote one.
    static bool IsLocalPageUrl(const std::string& url)
    {
        if (url.empty() || url.rfind("about:", 0) == 0) return true;
        if (url.rfind("file:", 0) != 0) return false;
        std::string path = url;
        if (const auto q = path.find_first_of("?#"); q != std::string::npos) path.resize(q);
        for (auto& c : path) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return path.ends_with(".html") || path.ends_with(".htm");
    }

    // A static that is never destroyed. The platform bridges and listeners derive from SDK classes whose destructors
    // are delay-loaded runtime imports: as plain statics those would run from DLL_PROCESS_DETACH even when
    // PreloadRuntime never loaded the runtime, and raise 0xC06D007E at process exit (invariant 3). The classes held
    // here keep no state, so skipping their destructors loses nothing. Declare each one constinit: a dynamically
    // initialized static would call its SDK base's imported constructor (every SDK base here exports one) from
    // DllMain and fail the plugin's load. constinit rules out any dynamic initializer, so a class that cannot be
    // constant-initialized (one with a non-constexpr constructor of its own, as FontLoaderWin has) fails to compile
    // instead.
    template <class T>
    union NeverDestroyed {
        T value;
        constexpr NeverDestroyed() : value() {}
        ~NeverDestroyed() {}
    };

    // Main-frame loads OnBeginLoading let through (Ultralight thread only). It fires inside the LoadURL or Reload that
    // starts a load, so ApplyPendingLifecycle reads this around its call: no change = the document stayed (only its
    // fragment added or changed, or a navigation stopped below), and no DOM ready follows.
    static std::uint64_t s_mainLoadsBegun = 0;

    // A page's settle, under s_viewsMutex: marks a hidden prepaint view for its settle repaint and logs it.
    static void ApplyPageSettledLocked(ultralight::View* caller)
    {
        static std::atomic<int> s_logged{ 0 };
        MlView* v = FindViewByUlLocked(caller);
        if (!v) return;
        const bool repaint = !VR::IsLive() && v->prepaint && v->prepainted && !v->visible && !v->settleRepainted;
        v->settleRepaint = repaint;
        if (s_logged.fetch_add(1) < 64)
            SKSE::log::info("Magelight: view {} page settled{}", v->id,
                            repaint ? " - its prepaint is painted once more while hidden" : "");
    }
    // Settles that found the registry busy, applied by the next PageWorkBegin. Ultralight's thread only (the settle
    // fires in its Update and PageWorkBegin runs there too); no view is erased between the two, as ApplyPendingLifecycle
    // runs after the apply.
    static std::vector<ultralight::View*> s_pendingSettles;
    // GameBookkeeping's page-cursor reset after UI mode, when the registry was busy: PageWorkBegin does it.
    static std::atomic<bool> s_pageCursorResetPending{ false };

    class MlLoadListener final : public ultralight::LoadListener {
    public:
        // Ultralight fires OnBeginLoading SYNCHRONOUSLY inside LoadURL, which
        // LoadViewLocked calls while holding s_viewsMutex — taking the lock
        // here is a self-deadlock that the SEH guard turned into a dead
        // overlay (0.16.0 field log). So this takes no lock but the leaf
        // s_editMutex: it notes the load for the text-focus reset
        // (OnWindowObjectReady), and stops a main-frame navigation off the
        // page's own schemes. A top-level
        // navigation is outside the page's CSP, and a remote document would
        // get the bridge and the mod's network reach; Stop() here keeps the
        // request from being sent (a later LoadURL back leaked the requests
        // of the page already loading). The domReady flag is cleared where
        // loads are REQUESTED (MaterializeViews, ReloadView, NavigateView), and
        // put back when a host request kept its page: by ApplyPendingLifecycle
        // for one that began no load, by OnFailLoading for one whose load failed.
        void OnBeginLoading(ultralight::View* caller, uint64_t, bool is_main_frame,
            const ultralight::String& url) override
        {
            if (!is_main_frame) return;
            const std::string u = url.utf8().data();
            if (IsLocalPageUrl(u)) {
                ++s_mainLoadsBegun;
                std::lock_guard<std::mutex> lk(s_editMutex);
                s_docLoads.Begun(caller);
                return;
            }
            caller->Stop();
            static std::atomic<int> s_logged{ 0 };
            if (s_logged.fetch_add(1) < 32)
                SKSE::log::warn("Magelight: stopped a page navigating to {} - a view shows its own HTML files only", u);
        }
        // A main-frame window object. In a View that has had one before, one that comes after a load began is the new
        // document's, made as it begins and before its scripts and its reports (EditState::DocLoads), and drops the
        // old page's text focus: a page that replaces its own document (a link, location, reload, a form's submit)
        // reports no loss of it. A View's first one can come after its page reported (a deferred or end-of-body
        // script) and drops nothing.
        void OnWindowObjectReady(ultralight::View* caller, uint64_t, bool is_main_frame,
            const ultralight::String& url) override
        {
            if (!is_main_frame) return;
            bool newDoc = false;
            {
                std::lock_guard<std::mutex> lk(s_editMutex);
                newDoc = s_docLoads.WindowReady(caller);
            }
            const bool local = IsLocalPageUrl(url.utf8().data());
            if (!newDoc && !local) return;
            MlView* v = nullptr;
            {
                std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
                v = FindViewByUlLocked(caller);
                if (v && newDoc) {
                    ForgetEditStateLocked(*v);
                    v->hostLoad.pending = false;   // the page is gone: a failure now is the new document's
                    v->gamepadPage.clear();        // the pad waits for the new document's helper to say ready
                }
            }
            if (v && local) InstallBridgeShims(*v, true);
        }
        void OnDOMReady(ultralight::View* caller, uint64_t, bool is_main_frame,
            const ultralight::String&) override
        {
            if (!is_main_frame) return;
            {
                std::lock_guard<std::mutex> lk(s_editMutex);
                s_docLoads.Ended(caller);
            }
            ViewId id = 0;
            DomReadyFn cb = nullptr;
            {
                std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
                if (MlView* v = FindViewByUlLocked(caller)) {
                    id = v->id;
                    cb = v->onDomReady;
                    v->domReady = true;   // outbound calls held during the load flow now
                    v->docDomReady = true;
                    v->hostLoad.pending = false;
                    v->prepainted = false;
                    v->paintableAt = GetTickCount64();
                    v->settleRepaint = false;
                    v->settleRepainted = false;
                }
            }
            SKSE::log::info("Magelight: DOM ready (view {})", id);
            if (cb) cb(id);
            Emit(HostEvent::ViewDomReady, id);
            bool reloaded = false;
            {
                std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
                if (MlView* v = FindViewLocked(id)) { reloaded = v->reloadedFlag; v->reloadedFlag = false; }
            }
            if (reloaded) Emit(HostEvent::ViewReloaded, id);
        }
        void OnFailLoading(ultralight::View* caller, uint64_t, bool is_main_frame,
            const ultralight::String& url, const ultralight::String& description,
            const ultralight::String& error_domain, int error_code) override
        {
            if (!is_main_frame) return;
            {
                std::lock_guard<std::mutex> lk(s_editMutex);
                s_docLoads.Ended(caller);   // the page that started the load stays
            }
            ViewId id = 0;
            bool kept = false, flowing = false;
            {
                std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
                if (MlView* v = FindViewByUlLocked(caller)) {
                    id = v->id;
                    v->loadFailed = true;
                    // A host load that failed: the page it would have replaced stays and takes calls again. Its text
                    // focus stays too, unless the View had no window object (ApplyPendingLifecycle).
                    if (v->hostLoad.pending) {
                        kept = true;
                        v->hostLoad.pending = false;
                        v->docDomReady = v->hostLoad.docDomReady;
                        v->reloadedFlag = v->hostLoad.reloadedFlag;
                        v->htmlPath = std::move(v->hostLoad.htmlPath);   // what a wake or a rebuild loads
                        v->pageUrl = std::move(v->hostLoad.pageUrl);
                        // A request made since cleared the rest for its own load.
                        if (v->docDomReady && v->navigateUrl.empty() && !v->reloadPending) {
                            flowing = true;
                            v->domReady = true;
                            v->prepainted = false;   // the request cleared its timing: prepainted again
                            v->paintableAt = GetTickCount64();
                            v->settleRepaint = false;
                            v->settleRepainted = false;
                        }
                    }
                }
            }
            const std::string detail = std::string(url.utf8().data()) + ": " + description.utf8().data() +
                " (" + error_domain.utf8().data() + ":" + std::to_string(error_code) + ")";
            SKSE::log::error("Magelight: view {} FAILED to load - {}{}", id, detail,
                !kept ? "" : flowing ? " - it keeps its page, which takes calls again" : " - it keeps its page");
            Emit(HostEvent::ViewLoadFailed, id, error_code, 0, detail.c_str());
        }
        // Network and layout have gone quiet after a navigation. On 2.0 it fires only once the page has been painted
        // since that navigation (a view loaded hidden settles after its prepaint, or its first show), and once per
        // navigation, a change of the URL's fragment included (one made after the last paint waits for the next);
        // a resize does not reset it (tools/visibility-test section H). The repaint it asks for is latched until the
        // next DOM ready or kept host request (settleRepainted), and never asked in VR, where TakePrepaintView paints
        // nothing. It fires inside Renderer::Update, outside s_viewsMutex, but the header promises no call it runs
        // in, so try_lock (as OnChangeCursor). A settle that finds the lock held (another thread's API call, or with
        // pageThread the game thread's registry reads) waits for the next PageWorkBegin (s_pendingSettles): the page
        // settles once per navigation, so a dropped one would lose its repaint for the load.
        void OnPageSettled(ultralight::View* caller, const ultralight::String&) override
        {
            std::unique_lock<PageThread::OwnedMutex> lk(s_viewsMutex, std::try_to_lock);
            if (!lk) {
                s_pendingSettles.push_back(caller);
                return;
            }
            ApplyPageSettledLocked(caller);
        }
    };
    static constinit NeverDestroyed<MlLoadListener> s_loadListener;

    static std::string CapLogText(const std::string& in, std::size_t maxBytes);   // fwd
    static void ReportScriptError(ViewId id, const std::string& what, const JsBridge::ErrorInfo& e, bool uncaught);   // fwd

    // Render thread, once per frame: deliver queued C++ -> JS calls per view. Each call holds the page's JS context
    // lock for itself only, and the EvalJS callback and the error report (an event, which a render-thread mod gets
    // inline) run after it is released: a mod's callback runs under that lock only when the call's page code reaches
    // it (a JS listener it calls, or the ConsoleMessage event of a console message it writes, which the console
    // listener emits inside the call and a render-thread mod gets inline).
    static void DrainBridgeQueues()
    {
        // Snapshot the calls under the lock; deliver outside it.
        struct OutWork { ultralight::RefPtr<ultralight::View> ul; ViewId id; std::vector<OutboundCall> calls; };
        std::vector<OutWork> work;
        struct EvalWork { ultralight::RefPtr<ultralight::View> ul; ViewId id; std::vector<PendingEval> evals; };
        std::vector<EvalWork> evalWork;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            for (auto& v : s_views) {
                if (!v->ul || !v->domReady) continue;   // held until the page can take them
                if (!v->outbound.empty()) {
                    work.push_back(OutWork{ v->ul, v->id, std::move(v->outbound) });
                    v->outbound.clear();
                }
                if (!v->evals.empty()) {
                    evalWork.push_back(EvalWork{ v->ul, v->id, std::move(v->evals) });
                    v->evals.clear();
                }
            }
        }
        for (auto& ew : evalWork) {
            for (const auto& e : ew.evals) {
                std::string result;
                JsBridge::ErrorInfo err;
                bool ok = true;
                {
                    auto ctxScope = ew.ul->LockJSContext();
                    if (ctxScope) {
                        JSContextRef ctx = (*ctxScope);
                        // A mod's script runs unnamed (see JsBridge::Evaluate): its relative import() resolves.
                        ok = JsBridge::Evaluate(ctx, e.script, nullptr, result, err);
                    }
                }
                // View::EvaluateScript's strings: result "" and exception "" when the frame cannot run script.
                if (e.fn) e.fn(ew.id, result.c_str(), ok ? "" : err.text.c_str(), e.user);
                if (!ok) ReportScriptError(ew.id, "EvalJS", err, false);
            }
        }
        for (auto& w : work) {
            for (const auto& c : w.calls) {
                JsBridge::ErrorInfo err;
                std::string threw;   // what to name in the report; empty = nothing to report
                {
                    auto ctxScope = w.ul->LockJSContext();
                    if (!ctxScope) continue;
                    JSContextRef ctx = (*ctxScope);
                    std::string ignored;
                    if (c.raw) {   // fn is the script: a mod's runs unnamed, as EvalJS's (OutboundCall)
                        if (!JsBridge::Evaluate(ctx, c.fn, nullptr, ignored, err) && !c.quiet)
                            threw = "InvokeJS";
                    } else {
                        JSObjectRef global = JSContextGetGlobalObject(ctx);
                        JSStringRef fnName = JSStringCreateWithUTF8CString(c.fn.c_str());
                        JSValueRef fnVal = JSObjectGetProperty(ctx, global, fnName, nullptr);
                        JSStringRelease(fnName);
                        JSObjectRef fnObj = (fnVal && JSValueIsObject(ctx, fnVal))
                            ? JSValueToObject(ctx, fnVal, nullptr) : nullptr;
                        if (!fnObj || !JSObjectIsFunction(ctx, fnObj)) {
                            // No window shim for this channel: hand it to the page core, which buffers until someone
                            // subscribes (and reports a subscriber that throws to the console itself).
                            if (!JsBridge::Evaluate(ctx, "window.magelight&&window.magelight._dispatch(" +
                                    MlJsonQuote(c.fn) + "," + MlJsonQuote(c.arg) + ");", "magelight:dispatch", ignored, err))
                                threw = "InteropCall('" + CapLogText(c.fn, 128) + "')";
                        } else {
                            JSStringRef argStr = JSStringCreateWithUTF8CString(c.arg.c_str());
                            JSValueRef argVal = JSValueMakeString(ctx, argStr);
                            JSStringRelease(argStr);
                            JSValueRef exc = nullptr;
                            JSObjectCallAsFunction(ctx, fnObj, nullptr, 1, &argVal, &exc);
                            if (exc) {
                                err = JsBridge::Describe(ctx, exc);
                                threw = "InteropCall('" + CapLogText(c.fn, 128) + "')";
                            }
                        }
                    }
                }
                if (!threw.empty()) ReportScriptError(w.id, threw, err, true);
            }
        }
    }

    // ── Milestone-4 demo handlers (the playground page) ─────────────────────
    // Prove both directions plus the game-thread marshalling pattern the real
    // bridge will use for every SeverActions page.
    static ViewId s_playgroundId = 0;
    static ViewId s_badgeId = 0;
    static ViewId s_probeViewId = 0;

    static void OnRequestPageData(const char* arg)
    {
        SKSE::log::info("Magelight: requestPageData('{}') — gathering on game thread",
            arg ? arg : "");
        GameTask::Post([]() {
            std::string name = "?", loc;
            int gold = 0, level = 0, hp = 0, hpMax = 0;
            if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
                if (const char* n = pc->GetDisplayFullName()) name = n;
                gold = pc->GetGoldAmount();
                level = static_cast<int>(pc->GetLevel());
                if (auto* avo = pc->AsActorValueOwner()) {
                    hp = static_cast<int>(avo->GetActorValue(RE::ActorValue::kHealth));
                    hpMax = static_cast<int>(avo->GetPermanentActorValue(RE::ActorValue::kHealth));
                }
                if (auto* l = pc->GetCurrentLocation()) {
                    if (l->GetName()) loc = l->GetName();
                }
                if (loc.empty()) {
                    if (auto* cell = pc->GetParentCell()) {
                        if (cell->GetFullName()) loc = cell->GetFullName();
                    }
                }
            }
            const std::string json =
                "{\"name\":\"" + JsonEscape(name) +
                "\",\"level\":" + std::to_string(level) +
                ",\"gold\":" + std::to_string(gold) +
                ",\"health\":" + std::to_string(hp) +
                ",\"healthMax\":" + std::to_string(hpMax) +
                ",\"location\":\"" + JsonEscape(loc) + "\"}";
            InteropCall(s_playgroundId, "receivePageData", json);
        });
    }

    static void OnSettingChanged(const char* arg)
    {
        // Render-thread handler answering directly — the synchronous pattern.
        const std::string payload = arg ? arg : "";
        SKSE::log::info("Magelight: settingChanged: {}", payload);
        InteropCall(s_playgroundId, "showNotification",
            "C++ received setting payload (" + std::to_string(payload.size()) + " bytes)");
    }

    // ── UI mode + input routing (milestone 3) ───────────────────────────────
    // The toggle key (Page Up by default) toggles UI MODE: overlay shown +
    // focused, vanilla cursor up, the game's own controls suspended
    // (ToggleControls with storeState=false so NOTHING persists into saves —
    // the classic stuck-controls trap). A
    // WndProc hook on the game window captures real Windows messages (the only
    // way to get properly translated WM_CHAR text) and queues them; the render
    // thread — the only thread allowed to touch Ultralight — drains the queue
    // into the view each frame. ESC exits UI mode.
    struct InputMsg {
        UINT msg;       // WM_*; 0 = control marker (wparam 1 = focus, 0 = unfocus)
        WPARAM wparam;
        LPARAM lparam;
        std::uint16_t keys = 0;   // a key message's Input::KeyBits, captured when it was queued (Input::MakeKeyEvent)
        std::uint8_t source = Input::kPointerMouse;   // a pointer message's Input::PointerSource
    };
    static HWND                  s_hwnd = nullptr;
    static WNDPROC               s_origWndProc = nullptr;
    static std::atomic<WNDPROC>  s_ansiShimPrev{ nullptr };   // HookWndProc as an A-callable value (AnsiShimWndProc)
    static std::atomic<bool>     s_focused{ false };
    static std::mutex            s_inputMutex;
    static std::vector<InputMsg> s_inputQueue;
    // Edits that are not keys, queued in order with the keys and run by DrainInputQueue on the UI-mode view's
    // Editor: the IME's composition (the window thread turns each IME message into an op), text the VR keyboards
    // typed (QueueTypedText) and the keyboard page's edit buttons. An InputMsg of kEditOpMsg carries its index here.
    static constexpr UINT kEditOpMsg = WM_APP + 0x4F47;
    // The window lost OS focus: the keys and buttons the pages saw go down will send no release (DrainInputQueue).
    static constexpr UINT kKeyReleaseMsg = WM_APP + 0x4F48;
    // The gamepad's keys-style scroll: wparam = whole px (signed, positive up), lparam = the pointer. Unlike a wheel
    // notch it is not multiplied by the view's scroll step.
    static constexpr UINT kPadScrollMsg = WM_APP + 0x4F49;
    struct EditOp {
        // Compose / Commit / End: the IME. Insert: Editor::InsertText(text). Command: Editor::Execute(cmd).
        // Notify: no edit, only the page's '__ime' message (a candidate list that opened, changed or closed).
        enum Kind : std::uint8_t { Compose, Commit, End, Insert, Command, Notify } kind = End;
        std::wstring text;
        std::vector<ultralight::CompositionSegment> segs;   // one per clause; the clause being converted is thick
        std::uint32_t cursor = 0;                           // the caret within `text`, UTF-16 units
        ultralight::EditorCommand cmd{};
        bool keep = false;   // End: keep the composition as typed (FinishComposition) instead of cancelling it
        // An IME op's '__ime' state ("" = none): sent once the op has run, with `ime` and the anchor read then.
        const char* imeState = "";
        Ime::Snapshot ime;
    };
    static std::vector<EditOp> s_editOps;   // under s_inputMutex
    // The SA-port close hook (Magelight.h): fired on the game thread on
    // every UI-mode exit — the host's own menu-close notification.
    // MULTICAST (0.9.1): every embedding plugin registers its own hook and
    // hears every exit with the exiting view, filtering by the views it
    // owns. The 0.9.0 single slot silently replaced the previous
    // registrant the moment a second consumer loaded.
    static std::mutex                s_uiModeExitMutex;
    static std::vector<UIModeExitFn> s_uiModeExitCbs;

    // ── Our own engine menu ─────────────────────────────────────────────
    // Pushed on UI-mode entry, popped on exit. Owning a real IMenu is what
    // makes the engine cooperate with an overlay: the engine drives
    // MenuCursor only while a kUsesCursor menu is topmost (field-found 2026-
    // 09-01 — SA's item-carrier menu froze the cursor the moment it became
    // the only real menu on the stack), kUsesMenuContext puts input into the
    // menu context natively (gameplay controls off, gamepad included), the
    // engine's Cancel user event (Escape / gamepad B) lands in ProcessMessage,
    // MenuOpenCloseEvent fires for anyone sinking menu state, and
    // kPausesGame is a per-open flag — pause-on-open for free. We still draw
    // our own cursor sprite (the engine's is a pre-Present Scaleform menu,
    // always under our overlay) and keep the delta-integration cursor
    // fallback as belt-and-suspenders. The movie is a blank SWF shipped at
    // Data/Interface/magelightfocus.swf; if it's missing the menu is skipped
    // and UI mode runs exactly as before (no engine menu).
    static void SetUIMode(bool on, bool hideView = true, bool pauseGame = false, bool noTextEntry = false);   // fwd (below)
    static bool EscapeCapturedByUiView();                                               // fwd (below)
    static bool UiModeViewHoldsPad();                                                   // fwd (below)
    static std::filesystem::path GameRoot();                                          // fwd (below)
    static bool PathIsUnder(const std::filesystem::path& p, const std::filesystem::path& base);   // fwd (below)
    static void CursorMenuWatchdog();                                                   // fwd (below)
    static std::atomic<bool> s_focusMenuPause{ false };   // flag for the NEXT open
    static std::atomic<bool> s_focusMenuFreeze{ false };  // the UI-mode view wants the world freeze (see ApplyFreezeWorld)
    static std::atomic<bool> s_focusMenuHideSeen{ false };   // a kHide reached the menu while focused
    static void CheckFocusMenuClosed();                       // fwd (below)

    // The next main-thread FrameWork frame runs CheckFocusMenuClosed; this timer runs it too, for frames that
    // never reach that point (a loading screen presenting from another thread, a dead renderer).
    static void ArmFocusMenuCloseCheck()
    {
        try {
            std::thread([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                GameTask::Post([]() { CheckFocusMenuClosed(); });
            }).detach();
        } catch (...) {}   // FrameWork still checks
    }
    static bool              s_focusMenuRegistered = false;

    class MagelightFocusMenu final : public RE::IMenu
    {
    public:
        static constexpr const char* MENU_NAME = "MagelightFocus";
        static constexpr std::int8_t kDepth = 10;   // above SA's carrier (0), below the CursorMenu

        MagelightFocusMenu()
        {
            using F = RE::UI_MENU_FLAGS;
            depthPriority = kDepth;
            inputContext  = RE::UserEvents::INPUT_CONTEXT_ID::kMenuMode;
            menuFlags.set(F::kUsesCursor, F::kUsesMenuContext, F::kDontHideCursorWhenTopmost,
                          F::kCustomRendering, F::kAllowSaving);
            if (s_focusMenuPause.load()) menuFlags.set(F::kPausesGame);
            // The world freeze opens with the pause it depends on, as the Journal's does. Never
            // kTopmostRenderedMenu: it stops the menus below this one drawing, and MagelightOverlay's
            // PostDisplay is the UI-pass composite (the page would fall back to Present, which frame
            // generation drops).
            if (s_focusMenuPause.load() && s_focusMenuFreeze.load()) {
                menuFlags.set(F::kFreezeFrameBackground);
                SKSE::log::info("Magelight: world freeze ON (focus menu open)");
            }
            if (auto* sm = RE::BSScaleformManager::GetSingleton()) {
                sm->LoadMovie(this, uiMovie, "magelightfocus");
            }
        }

        static RE::IMenu* Create() { return new MagelightFocusMenu(); }

        RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& msg) override
        {
            GameTask::Scope pumpScope;   // posts from here go through the pump (MagelightGameTask.h)
            if (msg.type == RE::UI_MESSAGE_TYPE::kUserEvent) {
                if (auto* d = static_cast<RE::BSUIMessageData*>(msg.data)) {
                    if (d->fixedStr == "Cancel") {
                        // Escape / gamepad B through the engine: exit UI mode
                        // (idempotent with the WndProc Escape path). A page
                        // that captured Escape (0.28.0) keeps it on THIS path
                        // too: on VR the keyboard poll is not muted, so a
                        // physical Escape reaches the engine's MenuControls as
                        // Cancel and closed the UI under a modal that had
                        // asked for the key (review 2026-09-09).
                        // A view holding the pad gets B from the gamepad router, which closes it itself. On flat
                        // the keyboard device is muted while UI mode is on, so this Cancel is the pad's.
                        if (UiModeViewHoldsPad()) return RE::UI_MESSAGE_RESULTS::kHandled;
                        if (!EscapeCapturedByUiView()) {
                            SKSE::log::info("Magelight: Cancel through the focus menu - leaving UI mode");
                            GameTask::Post([]() { SetUIMode(false); });
                        }
                        return RE::UI_MESSAGE_RESULTS::kHandled;
                    }
                    // Every other menu-mode user event (Backspace, Enter,
                    // arrows, F-keys — the controlmap's 0x10 group) is the
                    // page's business and was already delivered through the
                    // window proc. Swallow it here so it cannot fall through
                    // to whatever engine menu sits beneath us (field
                    // 2026-09-03: the cursor froze right after Backspace).
                    static std::unordered_set<std::string> s_seen;
                    const std::string ev = d->fixedStr.c_str() ? d->fixedStr.c_str() : "";
                    if (s_seen.insert(ev).second)
                        SKSE::log::info("Magelight: menu user event '{}' swallowed by the focus menu", ev);
                    return RE::UI_MESSAGE_RESULTS::kHandled;
                }
            } else if (msg.type == RE::UI_MESSAGE_TYPE::kHide) {
                // Engine-initiated close (loading screen, force-close): UI mode must follow, decided once the
                // queue has run (CheckFocusMenuClosed): a hide of our own while UI mode is on (the held menu
                // closed before a re-entry's show) leaves the menu open again by then.
                if (s_focused.load()) {
                    s_focusMenuHideSeen.store(true);
                    ArmFocusMenuCloseCheck();
                }
            }
            return RE::IMenu::ProcessMessage(msg);
        }

        void PostDisplay() override {}  // kCustomRendering: nothing to draw

        // ── Skyrim VR object layout (0.17.2) ─────────────────────────────
        // IMenu is 0x30 bytes on SE/AE but 0x40 on VR: the VR engine keeps
        // unk30 (set through the VR-only vfunc 09), unk34 and a
        // BSFixedString menuName at 0x38 on EVERY menu. CommonLibSSE-NG's
        // cross-runtime build leaves those out of the struct and reaches
        // them by relocation (GetVRRuntimeData -> this+0x30), so an object
        // allocated at the SE size hands the VR engine garbage past its end:
        // opening this menu crashed SkyrimVR.exe reading menuName on the
        // first F10 in the headset (2026-09-03, VR-2 field pass). These
        // members reproduce the VR tail exactly; on SE they are dead weight
        // the engine never looks at.
        REX::EnumSet<RE::UI_MENU_Unk09, std::uint32_t> vrUnk30{ RE::UI_MENU_Unk09::kNone };   // 30
        std::byte                                      vrUnk34{ 1 };                          // 34
        RE::BSFixedString                              vrMenuName{ MENU_NAME };               // 38
    };
    static_assert(sizeof(MagelightFocusMenu) == 0x40, "MagelightFocusMenu must match the VR IMenu size (0x40)");

    static void RegisterFocusMenu()
    {
        std::error_code ec;
        const auto swf = GameRoot() / L"Data" / L"Interface" / L"magelightfocus.swf";
        if (!std::filesystem::exists(swf, ec)) {
            SKSE::log::warn("Magelight: {} missing — UI mode runs without an engine menu",
                SysInfo::ForLog(swf));
            return;
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->Register(MagelightFocusMenu::MENU_NAME, MagelightFocusMenu::Create);
            s_focusMenuRegistered = true;
            SKSE::log::info("Magelight: engine menu '{}' registered", MagelightFocusMenu::MENU_NAME);
        }
    }

    static LONG LogSehAndDisable(const char* where, unsigned long code, void* addr);  // fwd (frame section)

    // ── UI-pass composite ──────────────────────────────────────────────────
    // With the composite in the game's UI pass, Present still updates and renders the views (into their own
    // textures) and records the quads it would have drawn; the carrier menu's PostDisplay, which the engine
    // calls inside its UI render with the UI's render target bound, draws them there one frame later.
    struct QueuedQuad {
        ID3D11ShaderResourceView* srv = nullptr;   // owned ref
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
        float x = 0, y = 0, w = 0, h = 0;
        bool  hasCut = false;
        float cut[4] = { 0, 0, 0, 0 };
    };
    static std::mutex              s_uiPassMutex;
    static std::vector<QueuedQuad> s_uiPassQuads;            // the last published frame
    static float                   s_uiPassBackW = 0, s_uiPassBackH = 0;   // the back buffer they were laid out for
    static std::chrono::steady_clock::time_point s_uiPassPublished{};   // a set older than 250 ms is not drawn
    static std::atomic<std::uint64_t> s_compositeFrames{ 0 };          // FrameWork composites, deferred or not
    static std::atomic<std::uint64_t> s_lastUiPassFrame{ 0 };          // s_compositeFrames at the last UI-pass draw (PostDisplay or the end-of-frame fallback)
    static std::atomic<bool>          s_uiPassSeen{ false };
    static bool                       s_overlayMenuRegistered = false;

    static void ReleaseQuads(std::vector<QueuedQuad>& qs)
    {
        for (auto& q : qs) if (q.srv) q.srv->Release();
        qs.clear();
    }

    // Present defers to the UI pass only while the carrier menu is actually drawing: its PostDisplay (or, behind
    // a wrapper, the end-of-frame fallback) ran within the last two composites. Menus hidden (the console's tm, a
    // photo mode, loading screens) fall back to Present at once, so the views never vanish with them.
    static bool UiPassActive()
    {
        if (!s_uiPassSeen.load() || s_uiPassBroken.load()) return false;
        return s_compositeFrames.load() - s_lastUiPassFrame.load() <= 2;
    }

    static void UiPassComposite(bool endOfFrameFallback = false);   // fwd (frame section)

    // The carrier: an invisible menu that exists for its PostDisplay, open only while an interactive view is
    // visible. In the menu stack it reads to the engine as an open menu, so Escape no longer opens the Journal
    // while it is up: it must never stay open over plain gameplay, and a click-through HUD view draws from the
    // end-of-frame hook instead behind a wrapper (s_endOfFrameFallback), else at Present. It takes no input; user
    // events pass on.
    class MagelightOverlayMenu final : public RE::IMenu
    {
    public:
        static constexpr const char* MENU_NAME = "MagelightOverlay";

        MagelightOverlayMenu()
        {
            using F = RE::UI_MENU_FLAGS;
            depthPriority = 9;  // over the game's menus (3) and HUD, under the focus menu (10), which stays topmost
            // kAssignCursorToRenderer, as the HUD has: when a menu closes, the engine keeps or hides the cursor by the
            // topmost menu without it, and this menu (no kUsesCursor) would hide the cursor of the Journal or the
            // Inventory under it.
            menuFlags.set(F::kAllowSaving, F::kAssignCursorToRenderer);
            // Out of the capture the world freeze shows as its background, or a translucent page would show
            // its own stale copy under itself. Only while the freeze is enabled: "freezeWorld": false gives
            // back 0.30.6's carrier flags.
            if (s_freezeWorldCfg.load() && s_freezeSkipCapture.load())
                menuFlags.set(F::kSkipRenderDuringFreezeFrameScreenshot);
            if (auto* sm = RE::BSScaleformManager::GetSingleton()) {
                sm->LoadMovie(this, uiMovie, "magelightfocus");
            }
            s_carrierOpen.store(true);
        }

        ~MagelightOverlayMenu() override { s_carrierOpen.store(false); }

        static RE::IMenu* Create() { return new MagelightOverlayMenu(); }

        RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& msg) override
        {
            if (msg.type == RE::UI_MESSAGE_TYPE::kUserEvent) return RE::UI_MESSAGE_RESULTS::kPassOn;
            return RE::IMenu::ProcessMessage(msg);
        }

        void PostDisplay() override { if (!s_engineComposite.load()) UiPassComposite(); }

        // VR object layout: see MagelightFocusMenu.
        REX::EnumSet<RE::UI_MENU_Unk09, std::uint32_t> vrUnk30{ RE::UI_MENU_Unk09::kNone };   // 30
        std::byte                                      vrUnk34{ 1 };                          // 34
        RE::BSFixedString                              vrMenuName{ MENU_NAME };               // 38
    };
    static_assert(sizeof(MagelightOverlayMenu) == 0x40, "MagelightOverlayMenu must match the VR IMenu size (0x40)");

    // `chain`: what the game swapchain's vtable says it is (InstallHook reads it).
    enum class SwapChainKind { kDxgi, kStreamline, kWrapper };

    static bool UiCompositeWanted(SwapChainKind chain)
    {
        if (REL::Module::IsVR()) return false;
        if (s_engineMode || s_compositeMode == "ui") return true;
        if (s_compositeMode == "present") return false;
        if (SmoothMotionLoaded()) return false;   // no engine mode: drawn at Present (late when InstallLatePresent can)
        return chain != SwapChainKind::kDxgi || GetModuleHandleW(L"SkyrimUpscaler.dll") != nullptr;
    }

    using EngineEndFn = void (*)(std::uint32_t);
    static EngineEndFn s_engineEndNext = nullptr;

    static void EngineEndThunk(std::uint32_t a)
    {
        s_engineEndNext(a);
        if (s_engineComposite.load()) {
            UiPassComposite();
        } else if (s_endOfFrameFallback.load() && !s_carrierOpen.load() && GetCurrentThreadId() == s_mainThreadId &&
                   s_lastUiPassFrame.load() != s_compositeFrames.load()) {
            // Only the thread FrameWork composites on (loading screens and the main menu present from another),
            // and only once a frame: Present bumps s_compositeFrames after this call.
            UiPassComposite(true);
        }
    }

    // Flat only. Refused unless the bytes at the site are a call, so a game version that moved it keeps the other
    // paths. Installed at most once; the caller sets which composite it serves.
    static bool InstallEngineEndHook()
    {
        if (REL::Module::IsVR()) return false;
        if (s_engineEndNext) return true;
        const std::uintptr_t site = REL::RelocationID(75461, 77246).address() + 0x9;
        if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8) {
            SKSE::log::warn("Magelight: the engine's end-of-frame call was not found");
            return false;
        }
        SKSE::AllocTrampoline(32);
        s_engineEndNext = reinterpret_cast<EngineEndFn>(
            SKSE::GetTrampoline().write_call<5>(site, reinterpret_cast<std::uintptr_t>(&EngineEndThunk)));
        return true;
    }

    static void RegisterOverlayMenu(SwapChainKind chain)
    {
        if (!UiCompositeWanted(chain)) {
            SKSE::log::info("Magelight: composite '{}' - views draw at Present", s_compositeMode);
            return;
        }
        std::error_code ec;
        if (!std::filesystem::exists(GameRoot() / L"Data" / L"Interface" / L"magelightfocus.swf", ec)) {
            SKSE::log::warn("Magelight: magelightfocus.swf missing - views draw at Present");
            return;
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->Register(MagelightOverlayMenu::MENU_NAME, MagelightOverlayMenu::Create);
            s_overlayMenuRegistered = true;
            SKSE::log::info("Magelight: composite '{}' - views draw in the game's UI pass ('{}'){}", s_compositeMode,
                MagelightOverlayMenu::MENU_NAME,
                s_compositeMode == "auto" && chain == SwapChainKind::kWrapper && !s_engineMode
                    ? " - the game swapchain is a wrapper, not dxgi's own" : "");
            if (chain == SwapChainKind::kWrapper && !s_engineMode && InstallEngineEndHook()) {
                s_endOfFrameFallback.store(true);
                SKSE::log::info("Magelight: while the carrier menu is closed (only click-through views up), the views "
                                "draw from the engine's end-of-frame call, not at Present");
            }
        }
    }

    // Opens the carrier while a view is visible and closes it a second after the last one hides. Main thread only
    // (the statics are not atomic): FrameWork, and the UI-mode entry task, which runs there on flat; the overlay is
    // never registered on VR, where SKSE drains tasks on job threads. A show the engine drops (a load closes every
    // menu) is re-queued after two seconds; a hide waits so a view flickering between pages does not open and close
    // the menu every frame. A hide still in the queue counts as closed, so an entry right after it shows the menu
    // again before the focus menu opens; a hide that never lands is retried after two seconds.
    static void SyncOverlayMenu(bool wanted)
    {
        if (!s_overlayMenuRegistered) return;
        if (s_mainMenuOpen.load()) wanted = false;   // main-menu pages take no carrier
        if (wanted && s_renderDead.load()) return;   // FrameWork stops at render death: nothing would close it again
        auto* ui = RE::UI::GetSingleton();
        auto* q = RE::UIMessageQueue::GetSingleton();
        if (!ui || !q) return;
        using Clock = std::chrono::steady_clock;
        static Clock::time_point s_nextShow{}, s_nextHide{}, s_lastWanted{};
        static bool s_hideQueued = false;
        const auto now = Clock::now();
        const bool openNow = ui->IsMenuOpen(MagelightOverlayMenu::MENU_NAME);
        if (!openNow || (s_hideQueued && now >= s_nextHide)) s_hideQueued = false;
        const bool open = openNow && !s_hideQueued;
        if (wanted) {
            s_lastWanted = now;
            s_nextHide = Clock::time_point{};
            if (!open && now >= s_nextShow) {
                s_nextShow = now + std::chrono::seconds(2);
                s_hideQueued = false;
                q->AddMessage(MagelightOverlayMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
            }
        } else if (open && now - s_lastWanted >= std::chrono::seconds(1) && now >= s_nextHide) {
            s_nextHide = now + std::chrono::seconds(2);
            s_nextShow = Clock::time_point{};
            s_hideQueued = true;
            q->AddMessage(MagelightOverlayMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
        }
    }

    // Present thread, UI-pass composite only. Logs when the carrier is open over a visible interactive view
    // but its PostDisplay stopped running (the views fall back to Present, which frame generation drops),
    // and when it comes back, with the world freeze's state: the one signal that the freeze silenced the
    // carrier. Debounced over 10 composites; at most 20 stop lines a session (the console's tm causes them too).
    static void NoteUiPassState(bool defer, bool interactiveVisible)
    {
        if (!s_overlayMenuRegistered || !s_uiPassSeen.load() || s_uiPassBroken.load()) return;
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        static bool s_logged = true;   // last logged state: drawing in the UI pass
        static int  s_streak = 0, s_stops = 0;
        const bool carrierUp = interactiveVisible && ui->IsMenuOpen(MagelightOverlayMenu::MENU_NAME);
        const bool state = defer || !carrierUp;   // only a silent carrier counts as stopped
        if (state == s_logged) { s_streak = 0; return; }
        if (++s_streak < 10) return;
        s_streak = 0;
        s_logged = state;
        bool frozen = false;
        if (s_focusMenuRegistered) {
            if (const auto m = ui->GetMenu(MagelightFocusMenu::MENU_NAME))
                frozen = m->menuFlags.all(RE::UI_MENU_FLAGS::kFreezeFrameBackground);
        }
        if (s_stops > 20) return;   // capped: the state still tracks, silently
        if (!state) {
            if (++s_stops > 20) { SKSE::log::info("Magelight: UI pass stop lines capped for this session"); return; }
            SKSE::log::info("Magelight: UI pass stopped drawing (no PostDisplay for {} composites) - views draw at "
                            "Present; world freeze {}", s_compositeFrames.load() - s_lastUiPassFrame.load(),
                            frozen ? "ON" : "off");
        } else {
            SKSE::log::info("Magelight: UI pass drawing again; world freeze {}", frozen ? "ON" : "off");
        }
    }

    static void QueueInput(UINT msg, WPARAM w, LPARAM l, int keys, std::uint8_t source)
    {
        const bool keyMsg = msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
                            msg == WM_CHAR;
        const std::uint16_t bits = !keyMsg ? 0 : keys >= 0 ? static_cast<std::uint16_t>(keys) : CaptureKeyBits(false);
        std::lock_guard<std::mutex> lk(s_inputMutex);
        // A move merges into a move of its source at the tail; past the soft cap only releases and markers go in.
        const bool tailMove = !s_inputQueue.empty() && s_inputQueue.back().msg == WM_MOUSEMOVE &&
                              s_inputQueue.back().source == source;
        switch (Input::QueueRule(s_inputQueue.size(), tailMove, msg, msg == 0 || msg == kKeyReleaseMsg)) {
        case Input::QueueStep::Append: s_inputQueue.push_back({ msg, w, l, bits, source }); break;
        case Input::QueueStep::ReplaceTail: s_inputQueue.back() = { msg, w, l, bits, source }; break;
        case Input::QueueStep::Drop: break;
        }
    }

    // Any thread. Shares the queue's soft cap (Input::kQueueSoftCap a drain); at most 64 ops a drain, the rest are
    // dropped.
    static void QueueEditOp(EditOp op)
    {
        std::lock_guard<std::mutex> lk(s_inputMutex);
        if (s_inputQueue.size() >= Input::kQueueSoftCap || s_editOps.size() >= 64) return;
        s_inputQueue.push_back({ kEditOpMsg, static_cast<WPARAM>(s_editOps.size()), 0 });
        s_editOps.push_back(std::move(op));
    }

    static void QueueEditCommand(ultralight::EditorCommand cmd)
    {
        EditOp op;
        op.kind = EditOp::Command;
        op.cmd = cmd;
        QueueEditOp(std::move(op));
    }

    // Public bridge (Magelight.h): text from a VR keyboard, for the UI-mode page. Each run of printable characters
    // is one Editor::InsertText (an edit as typing makes it: beforeinput and input, one undo step with the typing
    // around it); Backspace, Enter and Tab are key events (QueueVirtualKey), because pages act on their keydown
    // (EditState::SplitTyped). Any thread.
    void QueueTypedText(const wchar_t* s, std::size_t n)
    {
        if (!s || !n) return;
        for (auto& piece : EditState::SplitTyped(s, n)) {
            if (piece.vk) {
                QueueVirtualKey(static_cast<int>(piece.vk));
                continue;
            }
            EditOp op;
            op.kind = EditOp::Insert;
            op.text = std::move(piece.text);
            QueueEditOp(std::move(op));
        }
    }

    // ── Polled keys (0.31.10) ───────────────────────────────────────────
    // Muting the engine's keyboard and taking the window's key messages hides a key from everything that reads
    // the game's input, but not from a plugin that asks Windows for the key state itself (GetAsyncKeyState /
    // GetKeyState), so a plugin that polls its hotkeys that way acted on letters typed into a page.
    // While a page holds UI mode both calls answer "up" for keyboard keys to another mod's SKSE plugin (a DLL
    // under SKSE\Plugins), except the DLL that owns the page, which may poll its own close key. Magelight, its
    // runtime folder, the game and Windows itself (the IME reads modifier state) are never filtered, nor are the
    // mouse buttons. GetKeyState keeps its toggle bit (Caps Lock, Num Lock). Magelight.json
    // "blockPolledKeys": false leaves both calls alone (no hook is installed).
    static std::string WideToUtf8(const std::wstring& w);
    using KeyStateFn = SHORT(WINAPI*)(int);
    static KeyStateFn           s_getAsyncKeyStateNext = nullptr;
    static KeyStateFn           s_getKeyStateNext = nullptr;
    static const void*          s_selfModule = nullptr;

    // Another mod's SKSE plugin? Cached per image base. The file name is read outside the lock: a thread holding
    // the loader lock can poll a key, and GetModuleFileNameW wants that lock.
    static bool IsForeignPluginModule(const void* base)
    {
        static std::mutex s_mutex;
        static std::unordered_map<const void*, bool> s_known;
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            if (const auto it = s_known.find(base); it != s_known.end()) return it->second;
        }
        wchar_t buf[1024]{};
        const DWORD n = GetModuleFileNameW(reinterpret_cast<HMODULE>(const_cast<void*>(base)), buf, 1023);
        std::wstring path(buf, n);
        std::wstring lower(path);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        const bool foreign = n > 0 && Input::IsForeignPluginPath(lower);
        bool first = false;
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            first = s_known.emplace(base, foreign).second;
        }
        if (first && foreign) {
            const auto slash = path.find_last_of(L"\\/");
            SKSE::log::info("Magelight: keys {} polls are hidden while a page holds UI mode (Magelight.json "
                            "\"blockPolledKeys\": false allows them)",
                            WideToUtf8(slash == std::wstring::npos ? path : path.substr(slash + 1)));
        }
        return foreign;
    }

    static bool HidePolledKey(int vk, void* returnAddress)
    {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2)
            return false;
        if (!IsUIModeActive()) return false;
        PVOID base = nullptr;
        if (!RtlPcToFileHeader(returnAddress, &base) || !base) return false;
        if (base == s_selfModule || base == Api4::GetUIModeOwnerModule()) return false;
        return IsForeignPluginModule(base);
    }

    // Magelight's own key-state reads. They call the originals behind the filter: a plugin that hooks these
    // exports after Magelight puts its own DLL at the return address, so a read made through it would be taken
    // for that plugin's and hidden.
    static SHORT MlGetAsyncKeyState(int vk)
    {
        return s_getAsyncKeyStateNext ? s_getAsyncKeyStateNext(vk) : GetAsyncKeyState(vk);
    }

    static SHORT MlGetKeyState(int vk)
    {
        return s_getKeyStateNext ? s_getKeyStateNext(vk) : GetKeyState(vk);
    }

    // The modifiers and locks a key message is read with (Input::KeyBits). windowProc: the message the window proc
    // is handling, whose state the thread's GetKeyState holds; otherwise the keyboard's live state (the VR sink,
    // a key the host makes up). The lock toggles come from GetKeyState either way, as QueueScancodeAsText's text does.
    static std::uint16_t CaptureKeyBits(bool windowProc)
    {
        std::uint16_t bits = 0;
        for (int i = 0; i < 8; ++i) {
            const int vk = Input::kModifierVk[i];
            if ((windowProc ? MlGetKeyState(vk) : MlGetAsyncKeyState(vk)) & 0x8000) bits |= static_cast<std::uint16_t>(1u << i);
        }
        if (MlGetKeyState(VK_CAPITAL) & 1) bits |= Input::kKbCaps;
        if (MlGetKeyState(VK_NUMLOCK) & 1) bits |= Input::kKbNumLock;
        return bits;
    }

    static SHORT WINAPI HookGetAsyncKeyState(int vk)
    {
        const SHORT state = s_getAsyncKeyStateNext(vk);
        return (state != 0 && HidePolledKey(vk, _ReturnAddress())) ? 0 : state;
    }

    static SHORT WINAPI HookGetKeyState(int vk)
    {
        const SHORT state = s_getKeyStateNext(vk);
        return ((state & 0x8000) && HidePolledKey(vk, _ReturnAddress())) ? static_cast<SHORT>(state & 1) : state;
    }

    // kDataLoaded, after LoadHostSettings. Both or neither: a half-installed filter would be harder to reason about.
    static void InstallPolledKeyFilter()
    {
        if (!s_blockPolledKeys) {
            SKSE::log::info("Magelight: polled keys pass through while a page holds UI mode (blockPolledKeys false)");
            return;
        }
        PVOID self = nullptr;
        RtlPcToFileHeader(reinterpret_cast<PVOID>(&InstallPolledKeyFilter), &self);
        s_selfModule = self;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        void* async = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, "GetAsyncKeyState")) : nullptr;
        void* sync = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, "GetKeyState")) : nullptr;
        const MH_STATUS init = MH_Initialize();
        if (!async || !sync || (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)) {
            SKSE::log::warn("Magelight: polled-key filter not installed ({}) - a plugin that polls the keyboard sees "
                            "keys typed into a page", !async || !sync ? "user32 exports not found" : MH_StatusToString(init));
            return;
        }
        MH_STATUS st = MH_CreateHook(async, reinterpret_cast<void*>(&HookGetAsyncKeyState),
                                     reinterpret_cast<void**>(&s_getAsyncKeyStateNext));
        if (st == MH_OK) {
            st = MH_CreateHook(sync, reinterpret_cast<void*>(&HookGetKeyState), reinterpret_cast<void**>(&s_getKeyStateNext));
            if (st != MH_OK) MH_RemoveHook(async);
        }
        if (st == MH_OK) {
            st = MH_EnableHook(async);
            if (st == MH_OK) {
                st = MH_EnableHook(sync);
                if (st != MH_OK) MH_DisableHook(async);
            }
            if (st != MH_OK) { MH_RemoveHook(async); MH_RemoveHook(sync); }
        }
        if (st != MH_OK) {
            s_getAsyncKeyStateNext = nullptr;   // removed trampolines: MlGet* must fall back to the imports
            s_getKeyStateNext = nullptr;
            SKSE::log::warn("Magelight: polled-key filter not installed (MinHook: {}) - a plugin that polls the keyboard "
                            "sees keys typed into a page", MH_StatusToString(st));
            return;
        }
        SKSE::log::info("Magelight: polled-key filter on - other plugins' GetAsyncKeyState / GetKeyState read keys as up "
                        "while a page holds UI mode");
    }

    // ── Typing on VR: the engine's keyboard device, not the window ──────
    // In a headset the game window almost never holds OS focus, so it receives no keyboard messages at all (not one
    // WM_CHAR, WM_KEYDOWN or private message with a physical keyboard and the runtime's virtual keyboard both in use).
    // The SKSE input sink does see the engine's keyboard device (that is how Escape and the hotkeys work in VR), so
    // its DirectInput scancode is queued as the messages the window would have had (Input::FromScancode), with the
    // modifier state from GetAsyncKeyState, which needs no window focus. This also picks up a keyboard the runtime
    // synthesises at the driver level (OpenComposite's virtual keyboard sends scancodes beside its own paths).
    // Game thread (the input sink).
    enum class KeyPhase { Down, Repeat, Up };
    // The text a key makes, if any. Printable? Ask the active layout, with the live modifier state, or `bits` when
    // given. It keeps the key's lParam, as the window's WM_CHAR does, so its keypress carries the key's code. Returns
    // the characters made.
    static int QueueKeyText(UINT vk, std::uint32_t make, LPARAM down, const std::uint16_t* bits = nullptr)
    {
        BYTE ks[256]{};
        if (bits) {
            Input::ApplyKeyBits(ks, *bits);
            if (*bits & Input::kKbRAlt) ks[VK_CONTROL] = ks[VK_LCONTROL] = 0x80;   // Windows types AltGr as Ctrl+Alt
        } else {
            if (MlGetAsyncKeyState(VK_SHIFT) & 0x8000)   ks[VK_SHIFT]   = 0x80;
            if (MlGetAsyncKeyState(VK_CONTROL) & 0x8000) ks[VK_CONTROL] = 0x80;
            if (MlGetAsyncKeyState(VK_MENU) & 0x8000)    ks[VK_MENU]    = 0x80;
            if (MlGetKeyState(VK_CAPITAL) & 1)           ks[VK_CAPITAL] = 1;
        }
        wchar_t buf[8]{};
        const int n = ToUnicode(vk, make, ks, buf, 7, 0);
        for (int i = 0; i < n && i < 7; ++i) {
            const wchar_t ch = buf[i];
            if (ch >= 0x20 || ch == L'\t' || ch == L'\r')
                QueueInput(WM_CHAR, static_cast<WPARAM>(ch), down);
        }
        return n;
    }

    static void QueueScancodeAsText(std::uint32_t scan, KeyPhase phase)
    {
        const Input::ScancodeKey key = Input::FromScancode(scan, (MlGetKeyState(VK_NUMLOCK) & 1) != 0);
        if (!key.vk) return;
        const UINT vk = key.vk;
        if (phase == KeyPhase::Up) {
            QueueInput(WM_KEYUP, vk, key.lparam | 1 | (1 << 30) | (1 << 31));
            return;
        }
        // Repeat count 1 in both; a repeat sets bit 30 (the key was already down).
        const LPARAM down = key.lparam | 1 | (phase == KeyPhase::Repeat ? (1 << 30) : 0);
        QueueInput(WM_KEYDOWN, vk, down);
        const int n = QueueKeyText(vk, key.make, down);
        static std::atomic<int> s_typeLog{ 0 };
        if (phase == KeyPhase::Down && s_typeLog.fetch_add(1) < 12)
            SKSE::log::info("Magelight: VR typing — scancode 0x{:02X} -> vk 0x{:02X}, {} char(s)", scan, vk, n);
    }

    // The engine's keyboard device sends a held key every frame with its held time and never a repeat, so the
    // window's auto-repeat is rebuilt from the player's Windows keyboard delay and rate. Only a key whose press the
    // page got repeats. Game thread (the input sink).
    static std::array<int, 256> s_vrKeyRepeats{};       // repeats sent since the press; -1 = press not delivered
    static void NoteVrKeyPress(std::uint32_t scan, bool delivered)
    {
        if (scan < s_vrKeyRepeats.size()) s_vrKeyRepeats[scan] = delivered ? 0 : -1;
    }
    static bool VrKeyRepeatDue(std::uint32_t scan, float heldSecs)
    {
        if (scan >= s_vrKeyRepeats.size() || s_vrKeyRepeats[scan] < 0) return false;
        static const float s_delay = []() {
            int d = 1;   // 0..3 = 250..1000 ms
            SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &d, 0);
            return 0.25f * static_cast<float>(std::clamp(d, 0, 3) + 1);
        }();
        static const float s_rate = []() {
            DWORD r = 31;   // 0..31 = about 2.5..30 per second
            SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &r, 0);
            return 2.5f + 27.5f * static_cast<float>(std::min<DWORD>(r, 31)) / 31.0f;
        }();
        if (heldSecs < s_delay) return false;
        const int due = 1 + static_cast<int>((heldSecs - s_delay) * s_rate);
        if (due <= s_vrKeyRepeats[scan]) return false;
        s_vrKeyRepeats[scan] = due;   // one message a frame at most; a slow frame does not queue a burst
        return true;
    }

    // Public bridge (declared in Magelight.h with stdint types): the VR laser's pointer messages, each tagged with
    // its hand (DrainInputQueue routes every hand on its own). Keys and text go through QueueTypedText.
    void QueueSyntheticInput(unsigned int msg, std::uintptr_t wparam, std::intptr_t lparam, int hand)
    {
        const std::uint8_t source = hand == 1 ? Input::kPointerHand1 : Input::kPointerHand0;
        QueueInput(static_cast<UINT>(msg), static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam), -1, source);
    }

    // Game thread only (a posted task). Idempotent. Step-logged (0.3.2 diagnostics:
    // the teardown is the crash neighborhood) and SEH-guarded by the caller.
    // hideView applies only to the OFF path: key-driven / load-boundary
    // exits hide the UI-mode view (the demo + SA close behavior); the public
    // ExitUIMode passes false so a view can stay rendered while the game
    // owns input (SA's Free Look).
    // The cursor movie last hidden while UI mode is on (Hook_CursorAdvanceMovie, or the fallback HideVanillaCursor),
    // compared, never read: the hook shows that movie again after UI mode, and the fallback's watchdog warns when it
    // draws again, while a rebuilt Cursor Menu (a new movie, shown) is only hidden again.
    static std::atomic<const void*> s_hiddenCursorMovie{ nullptr };
    // When UI mode last ended (GetTickCount64): MenuSink logs the Cursor Menu, the focus menu and the overlay for
    // three seconds after.
    static std::atomic<std::uint64_t> s_uiModeExitedMs{ 0 };
    // Frames left in which FrameWork hides the cursor sprite again after the Cursor Menu reopened (MenuSink).
    static std::atomic<int> s_cursorRehideFrames{ 0 };

    // The Cursor Menu's own AdvanceMovie hides its movie while a page holds UI mode and shows it again after,
    // as PrismaUI does, so the movie's state is only changed where the game updates it, never from a task (a mod reads
    // it from its input hook, and two threads in the movie at once crash). The hide is set after the advance: the game
    // shows the movie again earlier in the frame, and nothing between the advance and the menu's PostDisplay does.
    // PrismaUI's own hook on this slot sets visibility before its call through, so ours is last in either order.
    // s_hiddenCursorMovie is the movie it last hid; a new movie at the same address is shown anyway. On VR the root is
    // made transparent too, once per movie (the engine never resets the root), as SeverActions 4.5 does for its own
    // pages, the one hide of the VR quill known to work; whether hiding the movie alone would do there is untested.
    // s_cursorRootStale: the Cursor Menu reopened, so the movie the hook sees may be a new one at the old address.
    static std::atomic<bool> s_cursorRootStale{ false };
    // VR: the root shows again (another writer: SeverActions before 1.6.1 restores it when its own page closes, even
    // with another mod's page up). Read every 15th advance, on the advance's own thread.
    static bool CursorRootShown(RE::GFxMovieView* movie)
    {
        static std::uint32_t s_n = 0;
        if (!REL::Module::IsVR() || ++s_n % 15) return false;
        RE::GFxValue visible;
        return movie->GetVariable(&visible, "_root._visible") && visible.IsBool() && visible.GetBool();
    }
    static void SetCursorRootHidden(RE::GFxMovieView* movie, bool hide)
    {
        if (!REL::Module::IsVR()) return;
        movie->SetVariableDouble("_root._alpha", hide ? 0.0 : 100.0);
        movie->SetVariable("_root._visible", RE::GFxValue(!hide));
    }
    using CursorAdvanceMovieFn = void (*)(RE::IMenu*, float, std::uint32_t);
    static CursorAdvanceMovieFn s_origCursorAdvanceMovie = nullptr;
    static bool CursorHiddenInAdvance() { return s_origCursorAdvanceMovie != nullptr; }
    static void Hook_CursorAdvanceMovie(RE::IMenu* self, float interval, std::uint32_t currentTime)
    {
        if (s_origCursorAdvanceMovie) s_origCursorAdvanceMovie(self, interval, currentTime);
        RE::GFxMovieView* movie = self ? self->uiMovie.get() : nullptr;
        if (!movie) return;
        if (s_focused.load()) {
            if (movie->GetVisible()) movie->SetVisible(false);
            const bool stale = s_cursorRootStale.exchange(false);
            if (s_hiddenCursorMovie.exchange(movie) != movie || stale || CursorRootShown(movie))
                SetCursorRootHidden(movie, true);
        } else if (s_hiddenCursorMovie.load() == movie) {
            movie->SetVisible(true);
            SetCursorRootHidden(movie, false);
            s_hiddenCursorMovie.store(nullptr);
        }
    }

    static void InstallCursorAdvanceHide()
    {
        static bool s_done = false;
        if (s_done) return;
        s_done = true;
        // VR too (1.6.1): the vanilla quill showed at the pages' edges. The pin patches slot 4 of this vtable on VR too.
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_CursorMenu[0] };
        s_origCursorAdvanceMovie = reinterpret_cast<CursorAdvanceMovieFn>(vtbl.write_vfunc(0x5, &Hook_CursorAdvanceMovie));
        SKSE::log::info("Magelight: the game's cursor is hidden in its own menu's update while a page holds the mouse "
                        "(Cursor Menu AdvanceMovie)");
    }

    // Game thread. The fallback for Hook_CursorAdvanceMovie, which does nothing here once it is installed.
    // The CursorMenu stays OPEN while UI mode is on (it drives the MenuCursor position) but its sprite
    // must not draw: at Present it lands under the overlay, but in the UI pass the CursorMenu draws after us, on top.
    // The sprite is the menu's movie, whose root is made transparent (flat only: VR keeps its own pointer path): the
    // engine re-shows the movie itself every frame, which made GFxMovieView::SetVisible blink, but never touches the
    // root's own _alpha/_visible, and a reskinned cursor movie hides the same way. FrameWork re-asserts this shortly
    // after entry, every second, and for a few frames after the Cursor Menu reopens (s_cursorRehideFrames: with
    // "pinGameCursor" false it is not kept open, so another menu's open that makes the engine close and re-show it
    // builds a new, visible movie, which drew over the pages until the next second); exit restores the root.
    // The Windows cursor is left to the engine, which keeps it hidden over the game (a minimize and restore hides a
    // stray one again). Never call MenuCursor::SetCursorVisibility here: its show is a forced ShowCursor(TRUE) loop
    // until the count is >= 0, not a restore of the earlier state, so the Windows cursor stayed up beside the game's
    // after a page closed. Whether ShowCursor's per-thread count also played a part is suspected, not verified;
    // SampleCursorState logs the threads.
    static void HideVanillaCursor(bool hide)
    {
        if (REL::Module::IsVR() || CursorHiddenInAdvance()) return;
        if (!hide) s_hiddenCursorMovie.store(nullptr);
        if (auto* ui = RE::UI::GetSingleton()) {
            if (const auto menu = ui->GetMenu(RE::CursorMenu::MENU_NAME); menu && menu->uiMovie) {
                menu->uiMovie->SetVariableDouble("_root._alpha", hide ? 0.0 : 100.0);
                menu->uiMovie->SetVariable("_root._visible", RE::GFxValue(!hide));
                if (hide) s_hiddenCursorMovie.store(menu->uiMovie.get());
            }
        }
    }

    // The game draws its cursor in the Cursor Menu's PostDisplay (a kCustomRendering menu). While a page holds UI mode
    // on flat that draw is skipped, so nothing that shows the sprite again (a cursor replacer's script, another mod, a
    // rebuilt menu) can put the game's cursor over Magelight's. The menu stays open and keeps moving the MenuCursor;
    // Hook_CursorAdvanceMovie hides the movie as well. "skipGameCursor": false installs nothing. Main thread (the UI pass).
    using CursorPostDisplayFn = void (*)(RE::IMenu*);
    static CursorPostDisplayFn s_origCursorPostDisplay = nullptr;
    // Draws skipped since the last watchdog pass (it logs the first count of a session: proof the skip works).
    static std::atomic<std::uint32_t> s_gameCursorSkips{ 0 };
    static bool GameCursorSkipped() { return s_origCursorPostDisplay && s_focused.load() && !s_renderDead.load(); }
    static void Hook_CursorPostDisplay(RE::IMenu* self)
    {
        if (GameCursorSkipped()) {
            s_gameCursorSkips.fetch_add(1);
            return;
        }
        if (s_origCursorPostDisplay) s_origCursorPostDisplay(self);
    }

    static void InstallGameCursorSkip()
    {
        static bool s_done = false;
        if (s_done) return;
        s_done = true;
        // Not on VR: the headset has its own pointer path and HideVanillaCursor leaves VR alone too.
        if (REL::Module::IsVR() || !s_skipGameCursor) {
            SKSE::log::info("Magelight: the game's cursor draw is left alone ({})",
                            REL::Module::IsVR() ? "VR" : "Magelight.json skipGameCursor false");
            return;
        }
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_CursorMenu[0] };
        s_origCursorPostDisplay = reinterpret_cast<CursorPostDisplayFn>(vtbl.write_vfunc(0x6, &Hook_CursorPostDisplay));
        SKSE::log::info("Magelight: the game's cursor is not drawn while a page holds the mouse (Cursor Menu PostDisplay)");
    }

    // While a page holds UI mode the Cursor Menu is not closed. When another menu opens over a page, the engine closes
    // the Cursor Menu and opens a new one within a frame or two, and a mod that reads the cursor's movie every frame
    // (a gamepad cursor hide reading "_root" from its input hook) crashes on the half-built or half-torn-down one.
    // Only kHide is refused, as PrismaUI does for its own focus, and only while the page's menu still holds the cursor:
    // the exit's hide arrives after s_focused is cleared, and when the game closes the focus menu itself (UI mode then
    // follows a frame later, CheckFocusMenuClosed) the menu is off the stack by the time the cursor's hide is read.
    // kForceHide always passes. "pinGameCursor": false installs nothing. Runs on the thread processing UI messages (on
    // AE not always the main thread, see MenuSink): it reads atomics and the menu stack only.
    static bool FocusMenuHoldsCursor()
    {
        if (!s_focusMenuRegistered) return true;   // no focus menu: Magelight shows and hides the cursor itself
        auto* ui = RE::UI::GetSingleton();
        return ui && ui->IsMenuOpen(MagelightFocusMenu::MENU_NAME);
    }
    using CursorProcessMessageFn = RE::UI_MESSAGE_RESULTS (*)(RE::IMenu*, RE::UIMessage&);
    static CursorProcessMessageFn     s_origCursorProcessMessage = nullptr;
    static std::atomic<std::uint64_t> s_cursorPinLoggedMs{ 0 };   // GetTickCount64 of the last kept-open log line
    static RE::UI_MESSAGE_RESULTS Hook_CursorProcessMessage(RE::IMenu* self, RE::UIMessage& msg)
    {
        if (msg.type == RE::UI_MESSAGE_TYPE::kHide && s_focused.load() && FocusMenuHoldsCursor()) {
            const std::uint64_t now = GetTickCount64();
            if (now - s_cursorPinLoggedMs.load() > 10000) {
                s_cursorPinLoggedMs.store(now);
                SKSE::log::info("Magelight: the Cursor Menu kept open while a page holds UI mode (the engine asked to "
                                "close it, as it does when another menu opens over the page)");
            }
            return RE::UI_MESSAGE_RESULTS::kIgnore;
        }
        return s_origCursorProcessMessage ? s_origCursorProcessMessage(self, msg) : RE::UI_MESSAGE_RESULTS::kPassOn;
    }

    static void InstallCursorMenuPin()
    {
        static bool s_done = false;
        if (s_done) return;
        s_done = true;
        if (!s_pinGameCursor) {
            SKSE::log::info("Magelight: the game's Cursor Menu is left to the engine (Magelight.json pinGameCursor false)");
            return;
        }
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_CursorMenu[0] };
        s_origCursorProcessMessage =
            reinterpret_cast<CursorProcessMessageFn>(vtbl.write_vfunc(0x4, &Hook_CursorProcessMessage));
        SKSE::log::info("Magelight: the game's Cursor Menu stays open while a page holds UI mode (Cursor Menu ProcessMessage)");
    }

    // Menu depth diagnostics. In the UI-pass composite the views draw in MagelightOverlay's PostDisplay, so every
    // menu the engine renders after it in its stack (a higher depthPriority, or the same one opened later) draws
    // over the pages and the cursor; something drawn at Present (an ImGui overlay) is not a menu and is not listed.
    // Each menu's depth and flags are logged once per process, the first time it is seen open, which builds the
    // table a depth choice needs. Game thread only: the menu sink (VR too), and on flat the UI-mode entry task and
    // the once-a-second watchdog (LogMenusOverPages).
    static std::string MenuFlagNames(REX::EnumSet<RE::UI_MENU_FLAGS, std::uint32_t> flags)
    {
        using F = RE::UI_MENU_FLAGS;
        static constexpr std::pair<F, const char*> kNames[] = {
            { F::kPausesGame, "pauses" },          { F::kAlwaysOpen, "alwaysOpen" },
            { F::kUsesCursor, "usesCursor" },      { F::kModal, "modal" },
            { F::kTopmostRenderedMenu, "topmostRendered" }, { F::kCustomRendering, "customRendering" },
            { F::kRendersOffscreenTargets, "offscreenTargets" }, { F::kApplicationMenu, "application" },
            { F::kDontHideCursorWhenTopmost, "keepsCursor" }, { F::kAssignCursorToRenderer, "cursorToRenderer" },
            { F::kSkipRenderDuringFreezeFrameScreenshot, "skipsFreezeCapture" },
        };
        std::string names;
        for (const auto& [flag, name] : kNames) {
            if (!flags.all(flag)) continue;
            if (!names.empty()) names += ',';
            names += name;
        }
        return names;
    }

    static void NoteMenuDepth(const RE::BSFixedString& name, const RE::IMenu* menu)
    {
        static std::unordered_set<std::string> seen;
        if (!menu || !name.c_str() || !seen.insert(name.c_str()).second) return;
        SKSE::log::info("Magelight: menu '{}' depth {} flags 0x{:X} [{}]", name.c_str(),
                        static_cast<int>(menu->depthPriority), menu->menuFlags.underlying(),
                        MenuFlagNames(menu->menuFlags));
    }

    // The menu registered under `name`, or nullptr; the map entry read in place (the sink runs inside the engine's
    // menu dispatch).
    static const RE::IMenu* MenuNamed(RE::UI* ui, const RE::BSFixedString& name)
    {
        const auto it = ui->menuMap.find(name);
        return it != ui->menuMap.end() ? it->second.menu.get() : nullptr;
    }

    // The name `menu` is registered under, or nullptr.
    static const RE::BSFixedString* MenuNameOf(RE::UI* ui, const RE::IMenu* menu)
    {
        for (const auto& [name, entry] : ui->menuMap) {
            if (entry.menu.get() == menu) return &name;
        }
        return nullptr;
    }

    // The game's cursor sprite as the log describes it: the Cursor Menu open or not, its depth, whether its movie is
    // hidden (the movie's visible flag with Hook_CursorAdvanceMovie; its root's _alpha and _visible with the fallback
    // HideVanillaCursor) and the movie it loads (a cursor replacer names its own). shows: the sprite draws; movie: the
    // movie read. Game thread. With the hook it reads no ActionScript variable: a task must not.
    static std::string DescribeCursorSprite(RE::UI* ui, bool* shows = nullptr, const void** movie = nullptr)
    {
        if (shows) *shows = false;
        if (movie) *movie = nullptr;
        const auto menu = ui ? ui->GetMenu(RE::CursorMenu::MENU_NAME) : nullptr;
        if (!menu) return "no Cursor Menu";
        const bool open = ui->IsMenuOpen(RE::CursorMenu::MENU_NAME);
        const int depth = static_cast<int>(menu->depthPriority);
        if (!menu->uiMovie) return fmt::format("{}, depth {}, no movie", open ? "open" : "closed", depth);
        const auto* def = menu->uiMovie->GetMovieDef();
        const char* url = def ? def->GetFileURL() : nullptr;
        if (CursorHiddenInAdvance()) {
            // The movie's own visible flag, which the hook sets: no _root read from this thread (see the hook).
            const bool shown = menu->uiMovie->GetVisible();
            const bool drawnNow = open && shown;
            if (shows) *shows = drawnNow;
            if (movie) *movie = menu->uiMovie.get();
            const char* hookState = GameCursorSkipped() ? (drawnNow ? "shown but not drawn (skipGameCursor)" : "hidden, not drawn (skipGameCursor)")
                                                        : (drawnNow ? "DRAWN" : "hidden");
            return fmt::format("{}, depth {}, {} (movie visible {}), movie '{}'", open ? "open" : "closed", depth,
                               hookState, shown, url ? url : "?");
        }
        const double alpha = menu->uiMovie->GetVariableDouble("_root._alpha");
        RE::GFxValue v;
        const bool visible = menu->uiMovie->GetVariable(&v, "_root._visible") && v.IsBool() && v.GetBool();
        const bool drawn = open && visible && alpha > 0.0;
        if (shows) *shows = drawn;
        if (movie) *movie = menu->uiMovie.get();
        // With skipGameCursor the sprite's own state does not matter: its draw is skipped.
        const char* state = GameCursorSkipped() ? (drawn ? "shown but not drawn (skipGameCursor)" : "hidden, not drawn (skipGameCursor)")
                                                : (drawn ? "DRAWN" : "hidden");
        return fmt::format("{}, depth {}, {} (_alpha {:.0f}, _visible {}), movie '{}'", open ? "open" : "closed", depth,
                           state, alpha, visible, url ? url : "?");
    }

    // Whether Windows' own cursor shows over the game's client area with the game in front. Game thread.
    static bool WindowsCursorOverGame(CURSORINFO& ci)
    {
        ci = {};
        ci.cbSize = sizeof(ci);
        if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !s_hwnd || GetForegroundWindow() != s_hwnd) return false;
        RECT rc{};
        POINT origin{ 0, 0 };
        if (!GetClientRect(s_hwnd, &rc) || !ClientToScreen(s_hwnd, &origin)) return false;
        OffsetRect(&rc, origin.x, origin.y);
        return PtInRect(&rc, ci.ptScreenPos) != 0;
    }

    // UI-mode entry, about 30 frames in, and every second while UI mode is on (the watchdog): notes the depth of
    // every open menu (one opened before the sink was registered has not been noted), then says what draws over the
    // pages. Logged again only when that changes. At entry it also says where the game's cursor sprite is and how
    // it stands (DescribeCursorSprite), once per change, since a second cursor over the page is its usual cause.
    // Flat only: SKSE VR also drains tasks on game job threads, which may run while the main thread changes the
    // menu stack, and the views are headset overlays there, never drawn among the menus.
    static void LogMenusOverPages(bool entry)
    {
        if (REL::Module::IsVR()) return;
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        const RE::IMenu* overlay = MenuNamed(ui, MagelightOverlayMenu::MENU_NAME);
        // Ours, drawing nothing over the pages: the focus menu is an empty movie, and the vanilla cursor is kept
        // hidden while UI mode is on (HideVanillaCursor).
        const RE::IMenu* focus = MenuNamed(ui, MagelightFocusMenu::MENU_NAME);
        const RE::IMenu* cursor = MenuNamed(ui, RE::CursorMenu::MENU_NAME);
        bool past = false;
        bool cursorAfterPages = false;
        std::string over;
        for (const auto& menu : ui->menuStack) {
            const RE::IMenu* m = menu.get();
            if (!m) continue;
            const RE::BSFixedString* name = MenuNameOf(ui, m);
            if (name) NoteMenuDepth(*name, m);
            if (m == overlay) { past = true; continue; }
            if (m == cursor) cursorAfterPages = past;
            if (!past || m == focus || m == cursor) continue;
            if (!over.empty()) over += ", ";
            over += std::string(name && name->c_str() ? name->c_str() : "?") + " (" +
                    std::to_string(static_cast<int>(m->depthPriority)) + ")";
        }
        std::string line;
        if (s_engineComposite.load()) {
            line = "Magelight: the views draw after every menu (engine composite)";
        } else if (!UiPassActive() || !past) {
            line = s_endOfFrameFallback.load() && UiPassActive()
                       ? "Magelight: the views draw after every menu (the engine's end of frame, not in the UI pass)"
                       : "Magelight: the views draw after every menu (at Present, not in the UI pass)";
        } else {
            line = "Magelight: menus drawn over the pages: " + (over.empty() ? std::string("none") : over);
        }
        static std::string last;
        if (line != last) {
            last = line;
            SKSE::log::info("{}", line);
        }
        if (!entry) return;
        const bool uiPass = !s_engineComposite.load() && UiPassActive() && past;
        CURSORINFO ci;
        const bool windowsCursor = WindowsCursorOverGame(ci);
        const std::string cursorLine = fmt::format(
            "Magelight: the game's cursor while a page holds the mouse: {}, drawn {}; Windows' cursor {}",
            DescribeCursorSprite(ui), uiPass ? (cursorAfterPages ? "over the pages" : "under the pages") : "under the pages (the views draw last)",
            windowsCursor ? "SHOWS over the game" : "hidden over the game");
        static std::string lastCursor;
        if (cursorLine == lastCursor) return;
        lastCursor = cursorLine;
        SKSE::log::info("{}", cursorLine);
    }

    // Diagnostics for a stray Windows cursor. Game thread, run from a task FrameWork posts about 30 frames after a
    // UI-mode switch, so the queued CursorMenu and focus-menu messages have been processed. Logs whether Windows
    // shows the cursor and whether it is over the game's client area with the game window in front, the sampling
    // thread against the window's own thread, the engine's MenuCursor showCursorCount and the mouse's mode. The first
    // entry and exit of a session are logged, then up to five later exits that leave the cursor over the game.
    static void SampleCursorState(bool on)
    {
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        const bool showing = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
        bool overGame = false;
        if (showing && s_hwnd && GetForegroundWindow() == s_hwnd) {
            RECT rc{};
            POINT origin{ 0, 0 };
            if (GetClientRect(s_hwnd, &rc) && ClientToScreen(s_hwnd, &origin)) {
                OffsetRect(&rc, origin.x, origin.y);
                overGame = PtInRect(&rc, ci.ptScreenPos) != 0;
            }
        }
        static std::atomic<bool> s_logged[2]{};
        static std::atomic<int> s_strayLogged{ 0 };
        const bool first = !s_logged[on ? 1 : 0].exchange(true);
        const bool stray = !first && !on && overGame && s_strayLogged.fetch_add(1) < 5;
        if (!first && !stray) return;
        const auto* mc = RE::MenuCursor::GetSingleton();
        const int showCount = mc ? mc->GetRuntimeData().showCursorCount : INT_MIN;
        const DWORD windowTid = s_hwnd ? GetWindowThreadProcessId(s_hwnd, nullptr) : 0;
        auto* devices = RE::BSInputDeviceManager::GetSingleton();
        const bool nonExclusive = devices && devices->IsMouseBackground();
        SKSE::log::info("Magelight: cursor after UI mode {}{}: thread {} (window thread {}), showCursorCount {}, "
                        "Windows cursor {}, mouse {}",
            on ? "entry" : "exit", stray ? " (later exit, cursor over the game)" : "", GetCurrentThreadId(), windowTid,
            showCount, showing ? (overGame ? "showing over the game" : "showing, not over the game") : "hidden",
            nonExclusive ? "non-exclusive (bBackgroundMouse)" : "exclusive");
    }

    // ── Cursor clip (1.5.1) ──────────────────────────────────────────────
    // A page's cursor follows the game's mouse input (invariant 6), never the Windows cursor. When the game leaves the
    // Windows cursor free (a non-exclusive mouse, another UI host holding focus), it wanders onto another monitor
    // while the page's cursor goes on moving, and every click lands on both. So while a page holds the mouse and the
    // game window is in front, the Windows cursor is clipped to the window's client area. It is let go at UI-mode
    // exit, at render death, when the window loses focus and on a frame where the window is not in front, and the
    // clip that was there before comes back. Flat only.
    static std::mutex s_clipMutex;    // leaf: guards the four below; nothing but Win32 cursor calls under it
    static bool       s_cursorClipped = false;
    static bool       s_hadClipBefore = false;   // false: there was none (the whole desktop), released to none
    static RECT       s_clipBefore{};
    static RECT       s_clipOurs{};

    // Clip rects are compared with a few pixels' slack: a DPI-unaware process on mixed-DPI monitors reads clips back
    // through DPI virtualization, which can move an edge, and reads the unclipped state as a rect that need not
    // equal the virtual desktop exactly.
    constexpr LONG kClipSlack = 4;

    static bool SameClip(const RECT& a, const RECT& b)
    {
        return std::abs(a.left - b.left) <= kClipSlack && std::abs(a.top - b.top) <= kClipSlack &&
               std::abs(a.right - b.right) <= kClipSlack && std::abs(a.bottom - b.bottom) <= kClipSlack;
    }

    // A real clip leaves out more than the slack of the virtual desktop; anything else is no clip.
    static bool IsRealClip(const RECT& r)
    {
        const LONG x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const RECT desktop{ x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
        return !SameClip(r, desktop) && (r.left > desktop.left || r.top > desktop.top || r.right < desktop.right ||
                                         r.bottom < desktop.bottom);
    }

    static void ReleaseCursorClip(const char* why)   // any thread
    {
        {
            std::lock_guard<std::mutex> lk(s_clipMutex);
            if (!s_cursorClipped) return;
            s_cursorClipped = false;
            RECT now{};
            // Only a clip that is still ours is put back: one set since then (the game's own, another mod's) stays.
            if (!GetClipCursor(&now) || !SameClip(now, s_clipOurs)) return;
            ClipCursor(s_hadClipBefore ? &s_clipBefore : nullptr);
        }
        static std::atomic<int> s_n{ 0 };
        if (s_n.fetch_add(1) < 3) SKSE::log::info("Magelight: Windows cursor released ({})", why);
    }

    // Once a frame (FrameWork, the owner thread): clips while `want` (a page holds UI mode) and the game window is in
    // front, else releases. Re-clipped each frame, so a moved or resized window and a clip someone reset are followed.
    static void ConfineCursor(bool want)
    {
        if (!want || !s_clipCursorCfg.load() || REL::Module::IsVR() || !s_hwnd || GetForegroundWindow() != s_hwnd) {
            ReleaseCursorClip(want ? "the game window is not in front" : "UI mode ended");
            return;
        }
        RECT client{};
        POINT origin{ 0, 0 };
        if (!GetClientRect(s_hwnd, &client) || !ClientToScreen(s_hwnd, &origin)) return;
        OffsetRect(&client, origin.x, origin.y);
        if (IsRectEmpty(&client)) return;
        bool first = false;
        {
            std::lock_guard<std::mutex> lk(s_clipMutex);
            // Again under the lock: the window proc's release takes it too, so a focus loss either lands after this
            // clip and undoes it or is seen here.
            if (GetForegroundWindow() != s_hwnd) return;
            RECT now{};
            if (!GetClipCursor(&now)) return;
            // Before ours, and any clip set by someone else since (it is theirs to have back at the release).
            if (!s_cursorClipped || !SameClip(now, s_clipOurs)) {
                s_hadClipBefore = IsRealClip(now);
                s_clipBefore = now;
            }
            if (!SameClip(now, client) && !ClipCursor(&client)) return;
            s_clipOurs = client;
            first = !s_cursorClipped;
            s_cursorClipped = true;
        }
        if (first) {
            static std::atomic<int> s_n{ 0 };
            if (s_n.fetch_add(1) < 3)
                SKSE::log::info("Magelight: Windows cursor kept inside the game window ({},{} {}x{}) while a page "
                                "holds the mouse", client.left, client.top, client.right - client.left,
                                client.bottom - client.top);
        }
    }

    // ── World freeze (0.31.0) ─────────────────────────────────────────────
    // kFreezeFrameBackground on the focus menu: the engine shows a frozen frame instead of rendering the 3D
    // world. A freeze over a running game deadlocks or pins a stale frame, so it is set only while OUR menu
    // carries kPausesGame and the game is paused, and dropped BEFORE the pause on every retarget. It rides
    // the menu that holds the pause, so an engine-forced close takes both together. Never on VR: untested
    // there, excluded as a precaution (the freeze frame may never be rendered in the headset).

    // The UI-mode view asks for the freeze (its preference only). Any thread; takes s_viewsMutex.
    static bool UiViewWantsFreeze()
    {
        if (!FreezeWorldAvailable() || !s_focused.load()) return false;
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        const MlView* v = FindViewLocked(static_cast<ViewId>(s_uiModeView.load()));
        return v && v->freezeWorld;
    }

    // Game thread.
    static void DropFreezeWorld(RE::IMenu* menu, const char* why)
    {
        using F = RE::UI_MENU_FLAGS;
        if (menu && menu->menuFlags.all(F::kFreezeFrameBackground)) {
            menu->menuFlags.reset(F::kFreezeFrameBackground);
            SKSE::log::info("Magelight: world freeze OFF ({})", why);
        }
    }

    // Game thread. Makes the live focus menu's flag match what the UI-mode view wants and the pause allows.
    static void ApplyFreezeWorld(const char* why)
    {
        const bool want = UiViewWantsFreeze() && !s_mainMenuOpen.load();   // no world to freeze at the main menu
        s_focusMenuFreeze.store(want);   // a focus menu shown later (the watchdog's re-show) starts with it
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !s_focusMenuRegistered) return;
        const auto menu = ui->GetMenu(MagelightFocusMenu::MENU_NAME);
        if (!menu) return;
        using F = RE::UI_MENU_FLAGS;
        const bool paused = menu->menuFlags.all(F::kPausesGame) && ui->GameIsPaused();
        const bool has = menu->menuFlags.all(F::kFreezeFrameBackground);
        if (want && paused) {
            if (!has) {
                menu->menuFlags.set(F::kFreezeFrameBackground);
                SKSE::log::info("Magelight: world freeze ON (view {}, {})", s_uiModeView.load(), why);
            }
            return;
        }
        if (has && want) {
            static std::atomic<bool> s_warned{ false };
            if (!s_warned.exchange(true))
                SKSE::log::warn("Magelight: the game is running under the world freeze (another mod cleared the "
                                "pause?) - freeze dropped; it returns with the pause");
        }
        DropFreezeWorld(menu.get(), why);
    }

    // The engine's input-context stack, bottom to top, in ControlMap's numbering (0 gameplay, 1 menu mode); the top
    // one names the user events the engine gives buttons. For the gamepad trace only: read without a lock, in the
    // input dispatch that reads the same array to name the batch's events (a menu push can tear it, never crash it).
    static std::string InputContextStackText()
    {
        auto* cm = RE::ControlMap::GetSingleton();
        if (!cm) return "none";
        std::string s;
        for (const auto c : cm->GetRuntimeData().contextPriorityStack) {
            if (!s.empty()) s += ',';
            s += std::to_string(static_cast<int>(c));
        }
        return s.empty() ? "empty" : s;
    }

    // The control set UI mode suspends (true) or gives back (false). storeState=false: a transient toggle
    // that can never bake into a save; Console stays reachable as an escape hatch. Fighting is suspended only
    // without the focus menu: the engine sheathes drawn weapons and puts readied spells away the moment
    // fighting goes off, and the focus menu's menu context already keeps attack and block from the game. The
    // give-back restores exactly the set the suspend took (s_suspendedControls).
    static std::atomic<std::underlying_type_t<RE::ControlMap::UEFlag>> s_suspendedControls{ 0 };
    static void SetGameControlsSuspended(RE::ControlMap* cm, bool suspend)
    {
        using UEFlag = RE::ControlMap::UEFlag;
        // Scoped enum without a free operator| — combine the bits directly.
        auto bits = std::to_underlying(UEFlag::kMovement) | std::to_underlying(UEFlag::kLooking) |
                    std::to_underlying(UEFlag::kActivate) | std::to_underlying(UEFlag::kMenu) |
                    std::to_underlying(UEFlag::kPOVSwitch) | std::to_underlying(UEFlag::kWheelZoom);
        if (suspend) {
            if (!s_focusMenuRegistered) bits |= std::to_underlying(UEFlag::kFighting);
            s_suspendedControls.store(bits);
        } else if (const auto taken = s_suspendedControls.exchange(0)) {
            bits = taken;
        }
        cm->ToggleControls(static_cast<UEFlag>(bits), !suspend, false);
    }

    // VR: UI mode closes on a controller button's PRESS (a chord, the B/Y Cancel), and the game acts on that
    // button (its release, or a press it reads as new) once the focus menu has gone and the controls are
    // back. So a VR exit with a button down that was pressed while the page was open keeps BOTH, the focus
    // menu (menu context, which is what shields the opening press) and the suspended controls, until those
    // buttons have been up kHeldReleaseSettleMs (FrameWork polls), or kHeldMaxMs at most (a timer that does
    // not depend on FrameWork running). A button resting down from before the page opened, or pressed after
    // the exit, does not count.
    static std::atomic<std::uint32_t> s_heldGen{ 0 };        // the hold in force; 0 = none
    static std::atomic<std::uint32_t> s_heldRuntime{ 0 };    // the VR::HeldButtons it waits for, by half
    static std::atomic<std::uint32_t> s_heldEngine{ 0 };
    static std::atomic<std::uint32_t> s_holdCounter{ 0 };
    static bool                       s_exitWithoutHold = false;   // game thread: ForceExitUIMode
    static std::atomic<std::uint64_t> s_uiModeEnteredMs{ 0 };      // GetTickCount64 of the last UI-mode entry
    static constexpr std::uint64_t    kHeldReleaseSettleMs = 100;  // a few frames: the engine reads the release first
    static constexpr std::uint32_t    kHeldMaxMs = 1500;

    // Ends hold `gen` (a later hold is left alone). Game thread.
    static void EndControlsHold(const char* why, std::uint32_t gen)
    {
        if (!gen || !s_heldGen.compare_exchange_strong(gen, 0)) return;
        if (s_focused.load()) return;   // UI mode came back: it owns the menu and the controls again
        if (s_focusMenuRegistered)
            if (auto* queue = RE::UIMessageQueue::GetSingleton())
                queue->AddMessage(MagelightFocusMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
        if (auto* cm = RE::ControlMap::GetSingleton()) SetGameControlsSuspended(cm, false);
        SKSE::log::info("Magelight: controls restored ({})", why);
    }

    // Gamepad layer (1.6.0): the pad controls down now (a bit per Gamepad::Control) and when each was last pressed,
    // written by GamepadSink, so an exit of a view that held the pad keeps the controls until the button that closed
    // it is up, as on VR. GamepadSink already keeps that release from the game; the hold is the second line, for an
    // engine sink registered ahead of it. s_heldPad is the hold's share.
    static std::atomic<std::uint64_t> s_gamepadOwner{ 0 };              // the view holding the pad, as of the last batch
    static std::atomic<std::uint32_t> s_gamepadHeld{ 0 };
    static std::array<std::atomic<std::uint64_t>, Gamepad::kControlCount> s_gamepadPressedMs{};
    static std::atomic<std::uint32_t> s_heldPad{ 0 };
    static std::atomic<bool>          s_gamepadHidesCursor{ false };    // the pad navigates: no pointer to draw
    static std::atomic<bool>          s_gamepadKeyActivity{ false };    // a key went down: the keyboard is the device

    static bool UiModeViewHoldsPad()
    {
        const std::uint64_t owner = s_gamepadOwner.load();
        return owner != 0 && owner == s_uiModeView.load();
    }

    static void NoteGamepadControl(Gamepad::Control c, bool down)
    {
        const auto i = Gamepad::Index(c);
        if (c == Gamepad::Control::None || i >= Gamepad::kControlCount) return;
        const std::uint32_t bit = 1u << i;
        if (down && !(s_gamepadHeld.fetch_or(bit) & bit)) s_gamepadPressedMs[i].store(GetTickCount64());
        if (!down) s_gamepadHeld.fetch_and(~bit);
    }

    // The pad controls down now that were pressed at or after `sinceTickMs`.
    static std::uint32_t GamepadHeldPressedSince(std::uint64_t sinceTickMs)
    {
        std::uint32_t out = 0;
        const std::uint32_t held = s_gamepadHeld.load();
        for (std::size_t i = 1; i < Gamepad::kControlCount; ++i)
            if ((held & (1u << i)) && s_gamepadPressedMs[i].load() >= sinceTickMs) out |= 1u << i;
        return out;
    }

    // False when the timer cannot start: the caller then exits as usual, never a hold nothing ends.
    static bool ArmControlsHold(const VR::HeldButtons& held, std::uint32_t pad)
    {
        std::uint32_t gen = s_holdCounter.fetch_add(1) + 1;
        if (!gen) gen = s_holdCounter.fetch_add(1) + 1;
        try {
            std::thread([gen]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(kHeldMaxMs));
                GameTask::Post([gen]() { EndControlsHold("hold time limit reached", gen); });
            }).detach();
        } catch (...) {
            SKSE::log::warn("Magelight: could not start the controls-hold timer; controls restored at once");
            return false;
        }
        s_heldRuntime.store(held.runtime);
        s_heldEngine.store(held.engine);
        s_heldPad.store(pad);
        s_heldGen.store(gen);
        return true;
    }

    static void SetUIModeImpl(bool on, bool hideView, bool pauseGame, bool noTextEntry)
    {
        if (s_focused.load() == on) return;
        const ViewId uiView = static_cast<ViewId>(s_uiModeView.load());
        // A mod asking at the main menu makes the view a main-menu page, and the next frame starts the renderer if it
        // is not up: it comes up in a frame or two, so the entry goes on rather than being refused. If it cannot come
        // up, FrameWork ends UI mode.
        const bool mainMenuAsk = on && PageThread::MainMenuAskOpens(
            s_runtimeReady.load() && s_hookInstalled.load() && MainMenuPagesAvailable(), s_mainMenuOpen.load(),
            s_mainPresenting.load(), s_renderDead.load());
        if (mainMenuAsk) SetViewMainMenu(uiView, true);
        if (on && !s_rendererUp.load() && !mainMenuAsk) {
            // No renderer = nothing to show: entering UI mode here would only
            // suspend the game's controls over an empty screen (field
            // 2026-09-03, the missed-kNewGame session). Refuse, loudly.
            SKSE::log::warn("Magelight: UI mode refused — renderer not initialised yet (world not ready)");
            Emit(HostEvent::UIModeRefused, uiView, 0, 0, "renderer not ready");
            return;
        }
        if (on) {
            bool shows = true;
            {
                std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
                if (const MlView* v = FindViewLocked(uiView)) shows = ViewShows(*v);
            }
            if (!shows) {
                SKSE::log::info("Magelight: UI mode refused - view {} does not show here (the main menu has not reached "
                                "the main thread, or the game is still loading)", uiView);
                Emit(HostEvent::UIModeRefused, uiView, 0, 0, "view does not show here");
                return;
            }
        }
        if (on) strncpy_s(s_uiModeLabel, Api4::ViewLabel(uiView).c_str(), _TRUNCATE);
        SKSE::log::info("Magelight: SetUIMode({}{}) begin - {}", on, (on && pauseGame) ? ", paused" : "",
                        s_uiModeLabel);
        s_focused.store(on);
        if (on) { s_escLogSession.store(20); s_keySession.fetch_add(1); }
        s_focusMenuHideSeen.store(false);   // a hide from an earlier session is no news about this one
        if (on) s_uiModeEnteredMs.store(GetTickCount64());
        if (!on && s_hwnd) { s_imeCaretX.store(-1); PostMessageW(s_hwnd, kImeCtlMsg, 0, 0); }   // IME follows UI mode off
        // Here, not on the next frame: a load boundary's exit is followed by frames from another thread, which
        // FrameWork skips.
        if (!on) ReleaseCursorClip("UI mode ended");
        // UI mode shows its target view; other views (HUD badges etc.) keep
        // their own visibility.
        if (on) ShowView(uiView, true);
        else if (hideView) ShowView(uiView, false);
        if (!on) ShowOwnKeyboard(false);
        if (!on) {
            // Inspectors live only inside UI mode: Escape on the inspector (or
            // on its page) would otherwise leave it painted over the lower
            // half of the screen with nothing focused (field log 2026-09-02).
            std::vector<ViewId> inspectors;
            {
                std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
                for (auto& vp : s_views) if (vp->isInspector && vp->visible) inspectors.push_back(vp->id);
            }
            for (ViewId i : inspectors) ShowView(i, false);
        }
        // See s_heldGen. Entering again ends a pending hold here: its focus menu is still open.
        const VR::HeldButtons heldAtExit = (!on && !s_exitWithoutHold && VR::IsVRRuntime())
            ? VR::HeldButtonsPressedSince(s_uiModeEnteredMs.load()) : VR::HeldButtons{};
        // Only a view that held the pad: every other view closes as it always has.
        const std::uint32_t padAtExit = (!on && !s_exitWithoutHold && uiView && s_gamepadOwner.load() == uiView)
            ? GamepadHeldPressedSince(s_uiModeEnteredMs.load()) : 0;
        const bool holdForRelease = (heldAtExit.Any() || padAtExit) && ArmControlsHold(heldAtExit, padAtExit);
        const bool holdCancelled = on && s_heldGen.exchange(0) != 0;

        if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
            // The focus menu is kUsesCursor, so the engine opens and closes the Cursor Menu with it. A show or hide of
            // our own beside it makes the engine rebuild the menu within a millisecond, which cursor mods that keep
            // state on it can crash on. Without the focus menu (its movie missing) nothing else opens the cursor.
            if (!s_focusMenuRegistered)
                queue->AddMessage(RE::CursorMenu::MENU_NAME,
                    on ? RE::UI_MESSAGE_TYPE::kShow : RE::UI_MESSAGE_TYPE::kHide, nullptr);
            // Our engine menu rides the same queue; pause is decided per open
            // (the flag is read by the menu's constructor on show).
            if (s_focusMenuRegistered) {
                if (on) {
                    s_focusMenuPause.store(pauseGame);
                    s_focusMenuFreeze.store(UiViewWantsFreeze() && !s_mainMenuOpen.load());
                } else {
                    ApplyFreezeWorld("UI mode exit");   // s_focused is already off: drops the flag before the hide
                }
                // The overlay opens first: a menu without kUsesCursor that opens while the Cursor Menu is up or about
                // to open makes the engine hide it, and its close then opens a new one for the focus menu.
                if (on) SyncOverlayMenu(true);
                // A held menu closes first, so the show builds a new one with this entry's pause flag.
                if (holdCancelled)
                    queue->AddMessage(MagelightFocusMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
                if (on || !holdForRelease)
                    queue->AddMessage(MagelightFocusMenu::MENU_NAME,
                        on ? RE::UI_MESSAGE_TYPE::kShow : RE::UI_MESSAGE_TYPE::kHide, nullptr);
            }
        }
        SKSE::log::info("Magelight: {} {} queued", s_focusMenuRegistered ? "focus menu" : "cursor-menu",
                        on ? "show" : "hide");
        if (!on) s_uiModeExitedMs.store(GetTickCount64());
        // We draw our own cursor topmost. Fallback only: Hook_CursorAdvanceMovie hides the game's.
        HideVanillaCursor(on);
        if (!on) s_mouseDown.store(false);
        if (auto* cm = RE::ControlMap::GetSingleton()) {
            if (on || !holdForRelease) SetGameControlsSuspended(cm, on);
            // Text-entry flag while UI mode is on (the engine's own text
            // fields do the same through SkyUI's AllowTextInput). SKSE
            // suppresses Papyrus OnKeyDown while it is raised, and the
            // well-behaved native mods skip their hotkeys too — so a key
            // typed into a page can no longer double as another mod's
            // hotkey (field 2026-09-03: Backspace in a search field opened
            // a third mod's display manager and froze the cursor).
            // Balanced: raised once per focus, lowered once per unfocus.
            // kUIModeFlagNoTextEntry: a page with nothing to type into asks
            // us not to raise it — on Skyrim VR the raise starts the engine's
            // virtual keyboard device, which OpenComposite hooks to raise its
            // own keyboard over the page (field 2026-09-05, SA's quick wheel).
            static bool s_textEntryRaised = false;
            if (on && !s_textEntryRaised && !noTextEntry) {
                cm->AllowTextInput(true);
                s_textEntryRaised = true;
            } else if (!on && s_textEntryRaised) {
                cm->AllowTextInput(false);
                s_textEntryRaised = false;
            }
        }
        SKSE::log::info("Magelight: controls {}", on ? "suspended"
                        : holdForRelease ? "held off until the controller buttons are released" : "restored");
        // OpenComposite's own menu laser: its SKSE plugin sets the window
        // property OC_MENU_ACTIVE whenever an engine menu is open (ours
        // counts), and OCU draws a second beam over our panels. Clear it while
        // we hold focus — OCU's own MenuOpenCloseEvent watcher re-asserts it
        // the moment a real menu opens or ours closes, so we never leave it
        // wrong. Their laser could not point at our overlays anyway.
        if (on && s_hwnd && VR::SuppressRuntimeLaser()) {
            SetPropW(s_hwnd, L"OC_MENU_ACTIVE", reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(0)));
            SKSE::log::info("Magelight: cleared OC_MENU_ACTIVE — the runtime's own menu laser stands down");
        }
        QueueInput(0, on ? 1 : 0, 0);  // focus/unfocus marker for the render thread
        SKSE::log::info("Magelight: UI mode {}", on ? "ON (cursor up, game controls suspended)" : "OFF");
        // Close hook: every exit path lands here on the game thread — the
        // embedding mod's (SA's) close bookkeeping hangs off this.
        if (!on) {
            std::vector<UIModeExitFn> cbs;
            {
                std::lock_guard<std::mutex> lock(s_uiModeExitMutex);
                cbs = s_uiModeExitCbs;
            }
            for (UIModeExitFn cb : cbs) cb(uiView);
        }
        // 0.29.0: the view's manifest/API open/close sound. Game
        // thread already — play directly, after the exit hooks so a close
        // sound never precedes the consumer's own close bookkeeping.
        {
            std::string snd;
            {
                std::lock_guard<PageThread::OwnedMutex> lock(s_viewsMutex);
                if (MlView* v = FindViewLocked(uiView)) snd = on ? v->soundOpen : v->soundClose;
            }
            if (!snd.empty()) Sound::Play(snd, uiView);
        }
        if (!on) s_tooltipLeaveAll.store(true);   // the pointer goes with UI mode
        Emit(on ? HostEvent::UIModeEntered : HostEvent::UIModeExited, uiView);
        InteropCall(uiView, "__uimode", on ? "1" : "0");
    }

    // SEH wrapper — no locals with destructors allowed in here.
    static void SetUIMode(bool on, bool hideView, bool pauseGame, bool noTextEntry)
    {
        __try {
            SetUIModeImpl(on, hideView, pauseGame, noTextEntry);
        } __except (LogSehAndDisable("SetUIMode", GetExceptionCode(),
                        (GetExceptionInformation())->ExceptionRecord->ExceptionAddress)) {
        }
    }

    // A kHide reached the focus menu while UI mode was on: if the menu is still closed once the queue has run,
    // the game closed it and UI mode follows. Main thread (FrameWork) or a game task.
    static void CheckFocusMenuClosed()
    {
        if (!s_focusMenuHideSeen.exchange(false) || !s_focused.load() || !s_focusMenuRegistered) return;
        auto* ui = RE::UI::GetSingleton();
        if (!ui || ui->IsMenuOpen(MagelightFocusMenu::MENU_NAME)) return;
        SKSE::log::info("Magelight: the focus menu was closed by the game - leaving UI mode");
        GameTask::Post([]() { SetUIMode(false); });
    }

    // OpenComposite Unleashed's virtual keyboard posts every character to the
    // GAME WINDOW as a private message (VRKeyboard.cpp: WM_OC_CHAR = WM_APP +
    // 0x4F45; wParam = wchar_t, lParam 0 = printable character, 1 = a virtual
    // key such as Backspace/Enter/Tab/Escape). We already subclass that window,
    // so OCU's keyboard reaches a Magelight page with NO change on their side —
    // their dedicated export path (which resolves another host's DLL by name and can
    // never find us) is not the only road in. Only consumed while a page holds
    // focus; otherwise it passes through to the console/Scaleform as usual.
    static constexpr UINT kOcCharMsg = WM_APP + 0x4F45;

    // Returns true when the message was consumed (swallow at the window proc).
    // Focused-window hotkey path. The engine input sink does not deliver
    // keyboard button events while our menu-context engine menu is up (F10
    // opened the manifest panel every time and never closed it, 2026-09-02),
    // but the window still receives WM_KEYDOWN — that is how the page gets
    // its typing. The lparam scancode is the DirectInput code; extended keys
    // (arrows, Page Up/Down, Home/End/Insert/Delete) carry bit 24 and map to
    // the 0x80+ range. Auto-repeat (bit 30) never re-fires.
    static void PeekHotkey(LPARAM lparam)
    {
        if (lparam & (1 << 30)) return;
        const std::uint32_t scan = static_cast<std::uint32_t>((lparam >> 16) & 0xFF);
        if (!scan) return;
        const std::uint32_t dx = scan | ((lparam & (1 << 24)) ? 0x80u : 0u);
        Api4::DispatchHotkey(dx, "window");
    }

    static std::string WideToUtf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<std::size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    // The window's input context for one scope (ImmGetContext / ImmReleaseContext); h is null when it has none.
    // Window thread; never inside an SEH wrapper (it has a destructor).
    struct ImcScope {
        HIMC h = s_hwnd ? ImmGetContext(s_hwnd) : nullptr;
        ImcScope() = default;
        ImcScope(const ImcScope&) = delete;
        ImcScope& operator=(const ImcScope&) = delete;
        ~ImcScope() { if (h) ImmReleaseContext(s_hwnd, h); }
    };

    // One composition property (GCS_*), sized then filled: the bytes the IME holds now. Window thread.
    static std::vector<unsigned char> ImeGetBytes(HIMC hImc, DWORD what)
    {
        const LONG n = ImmGetCompositionStringW(hImc, what, nullptr, 0);
        if (n <= 0) return {};
        std::vector<unsigned char> out(static_cast<std::size_t>(n));
        const LONG got = ImmGetCompositionStringW(hImc, what, out.data(), static_cast<DWORD>(n));
        out.resize(got > 0 ? std::min<std::size_t>(static_cast<std::size_t>(got), out.size()) : 0);
        return out;
    }

    static std::wstring ImeGetString(HIMC hImc, DWORD what)
    {
        const std::vector<unsigned char> b = ImeGetBytes(hImc, what);
        std::wstring out(b.size() / sizeof(wchar_t), L'\0');
        if (!out.empty()) std::memcpy(out.data(), b.data(), out.size() * sizeof(wchar_t));
        return out;
    }

    // The IME's state now, into an IME op (window thread): the composition with its caret and clause segments
    // for the editor, and for the page the same as UTF-8 with the target clause and, while the IME has a list
    // open, the candidate page it shows.
    static void ImeReadState(HIMC hImc, EditOp& op)
    {
        op.text = ImeGetString(hImc, GCS_COMPSTR);
        const std::size_t len = op.text.size();
        const LONG cur = ImmGetCompositionStringW(hImc, GCS_CURSORPOS, nullptr, 0);
        op.cursor = static_cast<std::uint32_t>(cur >= 0 ? std::min<std::size_t>(static_cast<std::size_t>(cur), len) : len);
        const std::vector<unsigned char> attrs = ImeGetBytes(hImc, GCS_COMPATTR);
        const std::vector<unsigned char> clauseBytes = ImeGetBytes(hImc, GCS_COMPCLAUSE);
        // Segments as the IME reports them, clamped to the text; no clause data: the editor underlines all of it.
        const std::size_t nClauses = clauseBytes.size() / sizeof(DWORD);
        for (std::size_t i = 0; nClauses >= 2 && i + 1 < nClauses; ++i) {
            const std::uint32_t a = static_cast<std::uint32_t>(std::min<std::size_t>(Ime::ReadU32(clauseBytes.data() + i * 4), len));
            const std::uint32_t b = static_cast<std::uint32_t>(std::min<std::size_t>(Ime::ReadU32(clauseBytes.data() + (i + 1) * 4), len));
            if (b <= a) continue;
            ultralight::CompositionSegment seg;
            seg.range = { a, b };
            for (std::uint32_t k = a; k < b && k < attrs.size(); ++k)
                if (attrs[k] == ATTR_TARGET_CONVERTED || attrs[k] == ATTR_TARGET_NOTCONVERTED) { seg.thick = true; break; }
            op.segs.push_back(seg);
        }
        Ime::Snapshot& snap = op.ime;
        snap.composition = WideToUtf8(op.text);
        snap.caret = static_cast<int>(op.cursor);
        snap.clauses = Ime::ReadClauses(clauseBytes.data(), clauseBytes.size(), len);
        snap.target = Ime::TargetClause(snap.clauses, attrs);
        if (!s_imeCandidatesOpen.load()) return;
        // The size is the IME's word: past 64 KB (an IME page is about nine short strings) the list is not read.
        const DWORD bytes = ImmGetCandidateListW(hImc, 0, nullptr, 0);
        if (bytes == 0 || bytes > 64 * 1024) return;
        std::vector<unsigned char> list(bytes);
        const DWORD got = ImmGetCandidateListW(hImc, 0, reinterpret_cast<LPCANDIDATELIST>(list.data()), bytes);
        const Ime::CandidatePage page = Ime::ReadCandidatePage(list.data(), std::min<std::size_t>(got, list.size()));
        for (const auto& c : page.items) snap.candidates.push_back(WideToUtf8(c));
        const auto lang = static_cast<std::uint16_t>(PRIMARYLANGID(LOWORD(reinterpret_cast<std::uintptr_t>(GetKeyboardLayout(0)))));
        snap.selectedIndex = Ime::SelectedRow(page, Ime::Converted(attrs), lang);
    }

    // Queues an IME op that also tells the page (`state`), with the IME's state read from `hImc` (none: an empty
    // state, as an 'end' carries). Window thread. Never throws: the window proc's SEH guard reads an escaping C++
    // exception as a fault and turns the overlay off for the session; a failure here only loses that op.
    static void ImeQueue(EditOp::Kind kind, const char* state, HIMC hImc)
    {
        try {
            EditOp op;
            op.kind = kind;
            op.imeState = state;
            if (hImc) ImeReadState(hImc, op);
            QueueEditOp(std::move(op));
        } catch (const std::exception& e) {
            SKSE::log::error("Magelight: IME '{}' dropped: {}", state, e.what());
        }
    }

    // Ends what the IME has open, the context still attached: the candidate list first (a cancel alone can leave it
    // open), then the composition (`always`: whether or not one is known to be open). The page's list is closed here
    // too, as the cancel need not end the composition by message (nor reach HandleFocusedMsg once UI mode is off).
    // Window thread.
    static void ImeCancelOpen(HIMC hImc, bool always)
    {
        if (hImc) {
            if (s_imeCandidatesOpen.load()) ImmNotifyIME(hImc, NI_CLOSECANDIDATE, 0, 0);
            if (always || s_imeComposing.load()) ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        }
        const bool wasComposing = s_imeComposing.exchange(false);
        if (s_imeCandidatesOpen.exchange(false) || wasComposing) ImeQueue(EditOp::Notify, "end", nullptr);
    }

    // The OS windows' place, for an IME that draws them although WM_IME_SETCONTEXT asked for none. Window thread.
    static void ImeRepositionCandidates()
    {
        const int x = s_imeCaretX.load(), y = s_imeCaretY.load(), h = s_imeCaretH.load();
        if (x < 0 || !s_hwnd) return;
        if (HIMC hImc = ImmGetContext(s_hwnd)) {
            CANDIDATEFORM cf{};
            cf.dwIndex = 0; cf.dwStyle = CFS_CANDIDATEPOS; cf.ptCurrentPos = { x, y + h };
            ImmSetCandidateWindow(hImc, &cf);
            COMPOSITIONFORM cpf{};
            cpf.dwStyle = CFS_POINT; cpf.ptCurrentPos = { x, y };
            ImmSetCompositionWindow(hImc, &cpf);
            ImmReleaseContext(s_hwnd, hImc);
        }
    }

    static void ImeAttach(bool on)   // window thread only (kImeCtlMsg)
    {
        if (!s_hwnd || s_imeAttached.load() == on) return;
        if (on) {
            const BOOL ok = ImmAssociateContextEx(s_hwnd, nullptr, IACE_DEFAULT);
            // Diagnostics: did the window actually get a context, is the IME open,
            // and is it in a native (hiragana/kana) conversion mode or alphanumeric?
            // Field 2026-09-06: keys reached the page as plain letters with no
            // composition — this line tells 'A mode' apart from 'no context'.
            if (HIMC hImc = ImmGetContext(s_hwnd)) {
                // OPEN the IME: the on/off state is per window, and a freshly
                // attached context starts CLOSED — every key passed through
                // untouched even in hiragana mode (field 2026-09-06 18:00:
                // open=false, conversion=native). Opening on focus means the
                // user never has to hit the IME on/off key inside the game.
                if (!ImmGetOpenStatus(hImc)) ImmSetOpenStatus(hImc, TRUE);
                DWORD conv = 0, sent = 0;
                ImmGetConversionStatus(hImc, &conv, &sent);
                SKSE::log::info("Magelight: IME attach ok={} context=yes open={} conversion=0x{:X} ({})",
                    ok != FALSE, ImmGetOpenStatus(hImc) != FALSE, conv,
                    (conv & IME_CMODE_NATIVE) ? "native/hiragana" : "alphanumeric — composition will not start");
                ImmReleaseContext(s_hwnd, hImc);
            } else {
                SKSE::log::warn("Magelight: IME attach ok={} but the window has NO context — IME disabled on this thread?", ok != FALSE);
            }
        } else {
            {
                // The page's 'end' is a message only: the field's composition is the drain's to end (it finishes
                // one in a view before that view loses View focus, ImeFinishComposition).
                ImcScope c;
                ImeCancelOpen(c.h, false);
            }
            ImmAssociateContext(s_hwnd, nullptr);
        }
        s_imeAttached.store(on);
        SKSE::log::info("Magelight: IME context {}", on ? "attached (text field focused)" : "detached");
    }

    // The window's IME context follows text focus in the UI-mode view (DrainEditableStates decides); on VR the VR
    // keyboards own text and the OS IME UI is invisible. Attach and detach are idempotent on the window thread.
    static void ImeOnTextFocus(bool focused)
    {
        if (VR::IsLive() || !s_hwnd) return;
        if (!focused) s_imeCaretX.store(-1);
        PostMessageW(s_hwnd, kImeCtlMsg, focused ? 1 : 0, 0);
    }

    // Ultralight 2.0 reports each View's focused field itself (iframes and contenteditable included). The report
    // runs inside whatever Ultralight call caused it, on the render thread, LoadViewLocked's LoadURL under
    // s_viewsMutex among them, so it only records, under the leaf s_editMutex; DrainEditableStates acts on it.
    class MlEditorListener final : public ultralight::EditorListener {
    public:
        void OnChangeEditableState(ultralight::View* caller, const ultralight::EditableState& st) override
        {
            std::lock_guard<std::mutex> lk(s_editMutex);
            if (s_editChanges.size() >= kEditChangeCap) s_editChanges.erase(s_editChanges.begin());
            s_editChanges.push_back({ caller, st, s_hostFocusCall });
        }
        void OnDiscardComposition(ultralight::View*) override
        {
            if (s_hwnd) PostMessageW(s_hwnd, kImeCtlMsg, 3, 0);
        }
    };
    static constinit NeverDestroyed<MlEditorListener> s_editorListener;

    // 0.28.0: does the UI-mode page own Escape right now? (SetEscapeCapture /
    // the '__escapecapture' channel — a modal or an editing control closes
    // itself instead of the whole UI.)
    static bool EscapeCapturedByUiView()
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        const MlView* v = FindViewLocked(static_cast<ViewId>(s_uiModeView.load()));
        return v && v->escapeCapture;
    }

    // Milliseconds since the IME's last message about its composition or list, -1 for never. Window thread.
    static long long MsSinceIme()
    {
        const std::uint64_t last = s_imeLastMs.load();
        return last ? static_cast<long long>(GetTickCount64() - last) : -1LL;
    }

    // Ime::ComposingForEscape over the host's state. Window thread.
    static bool ImeComposingForEscape()
    {
        return Ime::ComposingForEscape(s_imeComposing.load(), s_imeCandidatesOpen.load(), s_imeEditorComposing.load(),
                                       MsSinceIme(), s_imeEnd.EndedWithCommit());
    }

    // The IME's composition ended by committing it and nothing of it is open: an Escape now was passed on by the IME.
    static bool ImeCommittedForEscape()
    {
        return s_imeEnd.EndedWithCommit() && !s_imeComposing.load() && !s_imeCandidatesOpen.load();
    }

    // One line per Escape message while UI mode is on (20 a UI-mode session, 200 in all): what it was, the state the
    // decision read (composing: Ime::ComposingForEscape; endedOnThisKey; held: the press before it was the IME's, as
    // the guard held it when the message came) and the decision. Window thread.
    static void LogEscape(UINT msg, LPARAM lparam, bool composing, bool endedOnThisKey, bool held, const char* decision)
    {
        if (s_escLogSession.fetch_sub(1) <= 0 || s_escLogTotal.fetch_add(1) >= 200) return;
        const bool processKey = std::strstr(decision, "VK_PROCESSKEY") != nullptr;
        const char* what = msg == WM_KEYDOWN ? (processKey ? "WM_KEYDOWN VK_PROCESSKEY" : "WM_KEYDOWN VK_ESCAPE")
                         : msg == WM_KEYUP   ? (processKey ? "WM_KEYUP VK_PROCESSKEY" : "WM_KEYUP VK_ESCAPE")
                                             : "WM_CHAR 0x1B";
        SKSE::log::info("Magelight: Escape {} scan=0x{:02X}{} - IME composing={} list={} editor composition={} "
                        "last IME message {} ms ago; composing for Escape={} ended on this key={} IME's press held={} -> {}",
                        what, (lparam >> 16) & 0xFF, (msg == WM_KEYDOWN && (lparam & (1 << 30))) ? " repeat" : "",
                        s_imeComposing.load(), s_imeCandidatesOpen.load(), s_imeEditorComposing.load(), MsSinceIme(),
                        composing, endedOnThisKey, held, decision);
    }

    static bool HandleFocusedMsg(UINT msg, WPARAM wparam, LPARAM lparam)
    {
        switch (msg) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            // Swallow but do NOT queue: mouse rides the SKSE sink + MenuCursor
            // (0.3.5) — Skyrim's DirectInput never delivers WM mouse messages
            // here anyway (zero arrived across every field run), and queueing
            // both sources would double-fire if one ever did.
            return true;
        case WM_KEYDOWN: case WM_KEYUP:
        case WM_CHAR:
            if (msg == WM_KEYDOWN) s_gamepadKeyActivity.store(true);   // the input sink hands the keyboard the device
            if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && wparam == VK_PROCESSKEY) {   // the IME owns this key
                if (((lparam >> 16) & 0xFF) == 0x01) {
                    // An Escape the IME consumed: its plain key-up (and any character) is the IME's too.
                    const bool held = s_imeEscape.Held();
                    if (msg == WM_KEYDOWN) s_imeEscape.ImeTook();
                    else s_imeEscape.Swallow(Ime::EscapeGuard::Key::Up, false, false);
                    LogEscape(msg, lparam, ImeComposingForEscape(), false, held, "to the IME (VK_PROCESSKEY)");
                }
                return true;
            }
            if (msg == WM_CHAR ? wparam == 0x1B : wparam == VK_ESCAPE) {
                const auto k = msg == WM_KEYDOWN ? Ime::EscapeGuard::Key::Down
                             : msg == WM_CHAR    ? Ime::EscapeGuard::Key::Char : Ime::EscapeGuard::Key::Up;
                const bool committed = ImeCommittedForEscape();
                const bool endedOnThisKey = !committed && s_imeEndMsgTime != 0 && GetMessageTime() == s_imeEndMsgTime;
                const bool composing = ImeComposingForEscape();
                const bool held = s_imeEscape.Held();
                const bool repeat = msg == WM_KEYDOWN && (lparam & (1 << 30)) != 0;
                if (s_imeEscape.Swallow(k, composing, endedOnThisKey, repeat)) {
                    LogEscape(msg, lparam, composing, endedOnThisKey, held, "to the IME");
                    return true;
                }
                const bool captured = msg == WM_KEYDOWN && EscapeCapturedByUiView();
                LogEscape(msg, lparam, composing, endedOnThisKey, held,
                          msg != WM_KEYDOWN ? (committed ? "IME committed on this key -> to the page" : "to the page")
                          : committed ? (captured ? "IME committed on this key -> to the page (it captured Escape)"
                                                  : "IME committed on this key -> to the page, and UI mode closes")
                          : captured ? "to the page (it captured Escape)" : "to the page, and UI mode closes");
            }
            QueueInput(msg, wparam, lparam, s_handlingPassedKey ? s_passedBits.load() : CaptureKeyBits(true));
            // Editing-key trace (Backspace/Delete): a field report of
            // "Backspace does nothing in text fields" needs to know whether
            // the key reaches the window at all (rate-limited).
            if (msg == WM_KEYDOWN && (wparam == VK_BACK || wparam == VK_DELETE) && !(lparam & (1 << 30))) {
                static std::atomic<int> s_n{ 0 };
                if (s_n.fetch_add(1) < 8)
                    SKSE::log::info("Magelight: WM_KEYDOWN vk=0x{:02X} scan=0x{:02X} queued", wparam, (lparam >> 16) & 0x1FF);
            }
            if (msg == WM_KEYDOWN && wparam == VK_ESCAPE && !EscapeCapturedByUiView()) {   // captured: the page gets the key instead (queued above)
                GameTask::Post([]() { SetUIMode(false); });
            }
            if (msg == WM_KEYDOWN) PeekHotkey(lparam);
            // Dev-loop keys (manual reload / inspector toggle) are deliberately
            // UNBOUND as of 0.17.0 — the owner wants F5/F12 free for now. The
            // machinery stays (ReloadView, s_inspectorRequest -> the inspector
            // host); a binding comes back later, probably via Magelight.json.
            return true;
        case kOcCharMsg: {
            const wchar_t wch = static_cast<wchar_t>(wparam);
            if (lparam == 1) {   // a virtual key (Backspace, Enter, Tab, Escape)
                QueueVirtualKey(static_cast<int>(wch));
            } else {   // text, one UTF-16 unit a message: a surrogate pair arrives in two
                static wchar_t s_ocHigh = 0;   // window thread
                wchar_t units[2]{};
                if (const std::size_t n = EditState::PairUnit(s_ocHigh, wch, units)) QueueTypedText(units, n);
            }
            static std::atomic<int> s_ocLog{ 0 };
            if (s_ocLog.fetch_add(1) < 6)
                SKSE::log::info("Magelight: OpenComposite keyboard char 0x{:04X} (mode {}) delivered to view {}",
                    static_cast<unsigned>(wch), static_cast<int>(lparam),
                    static_cast<std::uint64_t>(s_uiModeView.load()));
            return true;   // ours now: don't let Scaleform also eat it
        }
        case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            // Alt, F10 and Alt+key: the page gets the key (a system key; WM_SYSCHAR is not passed on, so Alt+x types
            // nothing) and the system combo (Alt+F4 etc.) still proceeds. Bound hotkeys are peeked here too.
            QueueInput(msg, wparam, lparam, CaptureKeyBits(true));
            if (msg == WM_SYSKEYDOWN) PeekHotkey(lparam);
            return false;
        case WM_KILLFOCUS:
            // Alt+Tab and the like: no release comes for what is held now.
            QueueInput(kKeyReleaseMsg, 0, 0);
            return false;
        // ── IME (0.27.0) ──
        case WM_IME_STARTCOMPOSITION: {
            s_imeComposing.store(true);
            s_imeEnd.Started();
            s_imeLastMs.store(GetTickCount64());
            static std::atomic<int> s_n{ 0 };   // one per typed word: the first few say the IME composes at all
            if (s_n.fetch_add(1) < 6) SKSE::log::info("Magelight: IME composition started");
            {
                ImcScope c;
                ImeQueue(EditOp::Notify, "start", c.h);
            }
            ImeRepositionCandidates();
            return true;   // no DefWindowProc: it would open the OS composition window; the pre-edit lives in the page
        }
        case WM_IME_COMPOSITION: {
            s_imeLastMs.store(GetTickCount64());
            if (lparam) s_imeEnd.Composition((lparam & GCS_RESULTSTR) != 0);
            ImcScope c;
            if (!c.h) return true;
            if (lparam & GCS_RESULTSTR) {
                try {   // never out of here: see ImeQueue
                    EditOp op;
                    op.kind = EditOp::Commit;
                    op.imeState = "commit";
                    EditOp now;   // what the IME holds after the commit (a partial commit keeps composing)
                    ImeReadState(c.h, now);
                    op.ime = std::move(now.ime);
                    op.text = ImeGetString(c.h, GCS_RESULTSTR);
                    static std::atomic<int> s_n{ 0 };
                    if (s_n.fetch_add(1) < 6) SKSE::log::info("Magelight: IME commit ({} UTF-16 unit(s))", op.text.size());
                    QueueEditOp(std::move(op));
                } catch (const std::exception& e) {
                    SKSE::log::error("Magelight: IME commit dropped: {}", e.what());
                }
            }
            // Also when only the caret, the clauses or the target clause moved (the arrows, Shift+arrows).
            if ((lparam & GCS_COMPSTR) ||
                (!(lparam & GCS_RESULTSTR) && (lparam & (GCS_CURSORPOS | GCS_COMPATTR | GCS_COMPCLAUSE))))
                ImeQueue(EditOp::Compose, "compose", c.h);
            return true;   // never DefWindowProc: it would re-deliver the result as WM_CHARs (double insert)
        }
        case WM_IME_ENDCOMPOSITION: {
            s_imeComposing.store(false);
            s_imeCandidatesOpen.store(false);
            s_imeEndMsgTime = GetMessageTime();
            s_imeLastMs.store(GetTickCount64());
            s_imeEnd.Ended();
            // End: cancels a composition the IME closed without a result (Escape, the reading deleted). One it closes
            // while the game window has no keyboard focus (Alt+Tab, another window) is kept as typed instead, as a
            // blur keeps it: the player did not ask for it to go.
            EditOp op;
            op.kind = EditOp::End;
            op.imeState = "end";
            op.keep = !s_imeWindowFocused;
            QueueEditOp(std::move(op));
            return true;
        }
        case WM_IME_SETCONTEXT:
            // No OS IME window at all (lParam 0): none draws over the game. The IME reports its candidate list through
            // WM_IME_NOTIFY instead, and the page core draws it. Restoring an ISC_SHOW* bit brings back an OS window
            // the player cannot see, under the page's.
            DefWindowProcW(s_hwnd, msg, wparam, 0);
            return true;
        case WM_IME_NOTIFY:
            // The candidate list goes to the page, never to the OS window; every other notification goes on to the
            // game's proc.
            switch (wparam) {
            case IMN_OPENCANDIDATE:
            case IMN_CHANGECANDIDATE: s_imeCandidatesOpen.store(true); break;
            case IMN_CLOSECANDIDATE:
                s_imeCandidatesOpen.store(false);
                s_imeEndMsgTime = GetMessageTime();
                break;
            default: return false;
            }
            s_imeLastMs.store(GetTickCount64());
            {
                ImcScope c;
                ImeQueue(EditOp::Notify, "candidates", c.h);
            }
            return true;
        default:
            return false;
        }
    }

    // SEH wrapper — no locals with destructors allowed in here.
    static bool GuardedHandleFocusedMsg(UINT msg, WPARAM wparam, LPARAM lparam)
    {
        __try {
            return HandleFocusedMsg(msg, wparam, lparam);
        } __except (LogSehAndDisable("wndproc", GetExceptionCode(),
                        (GetExceptionInformation())->ExceptionRecord->ExceptionAddress)) {
        }
        return false;
    }

    // Lost keys: the window has no key messages, so try what an alt-tab does. Window thread.
    static void RestoreKeyFocus()
    {
        const HWND before = GetFocus();
        if (before == s_hwnd || GetForegroundWindow() != s_hwnd) return;
        SetFocus(s_hwnd);
        SKSE::log::info("Magelight: keyboard focus was {:#x}, given to the game's window: {}",
                        reinterpret_cast<std::uintptr_t>(before), GetFocus() == s_hwnd ? "took" : "refused");
    }

    // A key from the poll, through the path a window key takes, so Escape and bound hotkeys decide as usual. Window thread.
    static void HandlePassedKey(WPARAM vk, LPARAM lparam)
    {
        const bool down = !((lparam >> 31) & 1);
        static auto s_session = s_keySession.load() - 1;
        const auto session = s_keySession.load();
        const std::uint16_t was = session == s_session ? s_passedBits.load() : CaptureKeyBits(false);
        s_session = session;
        const std::uint16_t bits = Input::NotePassedKey(was, vk, lparam, down);
        s_passedBits.store(bits);
        if (vk == VK_MENU) return;   // only right Alt comes this far, for its state
        s_handlingPassedKey = true;
        GuardedHandleFocusedMsg(down ? WM_KEYDOWN : WM_KEYUP, vk, lparam);
        s_handlingPassedKey = false;
        if (down) QueueKeyText(static_cast<UINT>(vk), static_cast<std::uint32_t>((lparam >> 16) & 0x7F), lparam, &bits);
    }

    // Page thread. A key event reads this thread's keyboard layout, and Windows switches only the window thread's.
    static void SyncPageKeyboardLayout()
    {
        static HKL s_last = nullptr;
        if (!s_hwnd) return;
        const HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(s_hwnd, nullptr));
        if (!layout || layout == s_last) return;
        s_last = layout;
        ActivateKeyboardLayout(layout, 0);
    }

    static LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        GameTask::Scope pumpScope;   // posts from here go through the pump (MagelightGameTask.h)
        if (msg == kKeyFocusMsg) { RestoreKeyFocus(); return 0; }
        if (msg == kPassedKeyMsg) {
            if (s_focused.load() && !s_renderDead.load()) HandlePassedKey(wparam, lparam);
            return 0;
        }
        if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lparam & (1 << 30))) s_windowKeyPresses.fetch_add(1);
        if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) s_imeWindowFocused = msg == WM_SETFOCUS;
        // A game that stops presenting in the background would otherwise leave the clip on the desktop.
        if (msg == WM_KILLFOCUS || (msg == WM_ACTIVATEAPP && !wparam) || msg == WM_DESTROY)
            ReleaseCursorClip("the game window lost focus");
        if (msg == kImeCtlMsg) {   // IME control, marshalled to the window's thread
            if (wparam == 2) ImeRepositionCandidates();
            else if (wparam == 3) {   // the page kept the composition as final text: cancel the IME's copy of it
                ImcScope c;
                if (c.h) ImeCancelOpen(c.h, true);
            }
            else ImeAttach(wparam != 0);
            return 0;
        }
        if (s_focused.load() && !s_renderDead.load()) {
            if (GuardedHandleFocusedMsg(msg, wparam, lparam)) return 0;
        }
        if (!s_origWndProc) return DefWindowProcW(hwnd, msg, wparam, lparam);
        return CallWindowProcW(s_origWndProc, hwnd, msg, wparam, lparam);
    }

    // The game's window, its message loop and most SKSE plugins' subclasses are ANSI. A plugin that subclasses
    // after us with SetWindowLongPtrA and calls the proc it replaced directly, not through CallWindowProcA, is
    // handed a USER32 thunk handle for our W proc and executes 0xFFFFxxxx. Sitting on top as an ANSI proc gives it
    // a real address; CallWindowProcA converts A->W exactly as DispatchMessageA did, so HookWndProc sees no change.
    static LRESULT CALLBACK AnsiShimWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        const WNDPROC next = s_ansiShimPrev.load();
        if (!next) return DefWindowProcA(hwnd, msg, wparam, lparam);
        return CallWindowProcA(next, hwnd, msg, wparam, lparam);
    }

    // ── Runtime preload ─────────────────────────────────────────────────────

    static std::filesystem::path GameRoot()
    {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return std::filesystem::path(buf).parent_path();
    }

    // ── Crash telemetry (0.3.2 diagnostics) ─────────────────────────────────
    // A first-chance vectored handler that LOGS severe exceptions from ANY
    // thread — code, address, owning module + offset, then the caller stack —
    // and continues the search (log-only, never swallows). The field CTD
    // produces no crashlog and doesn't trip our SEH nets, so it lives on a
    // thread/path we don't guard; this names it. Once per fault site;
    // known-handled families skipped.
    // module+offset for an address, module BASENAME: one resolver and one
    // format for the VEH logger and the stall watchdog (review 2026-09-09).
    // `path` is scratch the returned pointer points into.
    static const char* ModuleOffsetOf(void* addr, char (&path)[MAX_PATH], std::uintptr_t& off)
    {
        HMODULE mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &mod);
        path[0] = '?'; path[1] = 0;
        off = 0;
        if (!mod) return path;
        GetModuleFileNameA(mod, path, MAX_PATH);
        off = reinterpret_cast<std::uintptr_t>(addr) - reinterpret_cast<std::uintptr_t>(mod);
        const char* base = path;
        for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
        return base;
    }

    // A stack walk is in progress on this thread (the watchdog's, or the VEH's own): VectoredLogger leaves its
    // caught faults alone.
    static thread_local bool t_walkingStack = false;

    // One report per distinct fault site for the life of the process, keyed on (code, raw address): a repeat costs
    // a table scan, and faults in module-less memory (JIT code, a call through null) stay distinct. Lock-free; two
    // threads racing on one new site may each claim a slot.
    //
    // The handler runs first for every severe exception in the process, the real crash included, before
    // CrashLogger, so it takes no lock of its own, touches no heap and never asks the loader (the unwinder uses the
    // same function-table lookups exception dispatch makes): it fills its slot (the fault and the faulting thread's
    // callers), wakes the watchdog thread (s_faultEvent) and returns. That thread formats and logs the slot
    // (DrainFaultReports). Logging from
    // the handler hung a thread that faulted inside Magelight's own logging (the log's lock is that thread's), and
    // a fault while logging on a damaged heap re-entered the handler.
    // A slot is claimed (addr, then code, release), filled, then marked ready (release); the drain reads it only
    // once ready.
    static constexpr int kVehStackFrames = 24;
    struct SeenFault {
        std::atomic<std::uint32_t>  code{ 0 };
        std::atomic<std::uintptr_t> addr{ 0 };
        std::atomic<bool>           ready{ false };
        bool                        logged = false;   // drain thread only
        DWORD                       tid = 0;
        bool                        overflow = false;
        bool                        callerFirst = false;   // a call through null: frames[0] is its caller
        int                         frameCount = 0;
        void*                       frames[kVehStackFrames]{};
    };
    static constexpr int kMaxSeenFaults = 64;
    static SeenFault         s_seenFaults[kMaxSeenFaults];
    static std::atomic<int>  s_seenFaultCount{ 0 };
    static std::atomic<bool> s_seenFaultsFull{ false };

    static PVOID             s_vehHandle = nullptr;
    static HANDLE            s_faultEvent = nullptr;   // auto-reset: a slot is ready, the watchdog drains at once
    // CrashLogger.dll's image, found by the drain thread: its symbolizer probes memory while it writes a crash
    // report, and those faults are its own. A range compare, so the handler never asks the loader; end is stored
    // before base (release) and base is tested first, so a half-stored range matches nothing.
    static std::atomic<std::uintptr_t> s_crashLoggerBase{ 0 };
    static std::atomic<std::uintptr_t> s_crashLoggerEnd{ 0 };

    // This thread is inside the handler: a fault there (a damaged heap, a bad stack) must not re-enter it.
    static thread_local bool t_inVeh = false;

    // Committed stack left below the caller on this thread. In a stack overflow that is all there is: the guard
    // page is spent, and a touch below it is an access violation the process does not survive.
    static constexpr std::size_t kStackToWalk = 16 * 1024;   // the walk's own frames, with room to spare
    static std::size_t StackLeft()
    {
        const auto low = reinterpret_cast<std::uintptr_t>(reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackLimit);
        const auto sp = reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
        return sp > low ? sp - low : 0;
    }

    // Stack left below the caller in the whole reservation of the stack it runs on (StackLeft counts only the
    // committed part), in KB. The reservation's base is the stack's end (GetCurrentThreadStackLimits needs the
    // Windows 8 headers).
    static std::size_t StackReserveLeftKb()
    {
        MEMORY_BASIC_INFORMATION mbi{};
        const auto sp = reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
        if (!VirtualQuery(reinterpret_cast<const void*>(sp), &mbi, sizeof(mbi))) return 0;
        const auto low = reinterpret_cast<std::uintptr_t>(mbi.AllocationBase);
        return sp > low ? (sp - low) / 1024 : 0;
    }

    static int  UnwindFromContext(CONTEXT* ctx, void** frames, int cap, std::uintptr_t stackLow, std::uintptr_t stackHigh);   // fwd (below)

    // The faulting thread's callers, from the exception's own CONTEXT: the thread is stopped in its handler, so
    // the stack is exactly the one that faulted. Bounded to this thread's committed stack (its TEB), capped in
    // depth, and the unwinder's own faults are caught (t_walkingStack). A call through null faults with Rip 0 and
    // the caller's return address on top of the stack: the walk starts there.
    static __declspec(noinline) int WalkFaultStack(EXCEPTION_POINTERS* ep, void** frames, bool& callerFirst)
    {
        callerFirst = false;
        if (!ep->ContextRecord) return 0;
        const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
        const auto stackLow = reinterpret_cast<std::uintptr_t>(tib->StackLimit);
        const auto stackHigh = reinterpret_cast<std::uintptr_t>(tib->StackBase);
        if (stackHigh <= stackLow) return 0;
        CONTEXT ctx = *ep->ContextRecord;
        if (ctx.Rip == 0 && ctx.Rsp >= stackLow && ctx.Rsp + 8 <= stackHigh) {
            ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
            callerFirst = true;
        }
        t_walkingStack = true;
        const int n = UnwindFromContext(&ctx, frames, kVehStackFrames, stackLow, stackHigh);
        t_walkingStack = false;
        return n > 0 ? n : 0;
    }

    static LONG CALLBACK VectoredLogger(EXCEPTION_POINTERS* ep)
    {
        if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
        const auto code = ep->ExceptionRecord->ExceptionCode;
        if (code < 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;                       // not severe
        if (code == 0xC06D007Eu || code == 0xC06D007Fu) return EXCEPTION_CONTINUE_SEARCH;  // delay-load internals
        if (code == 0xE06D7363u) return EXCEPTION_CONTINUE_SEARCH;                      // C++ EH in flight
        if (t_walkingStack || t_inVeh) return EXCEPTION_CONTINUE_SEARCH;               // our own walk, or a fault in here
        if (!s_exceptionTelemetry.load(std::memory_order_relaxed)) return EXCEPTION_CONTINUE_SEARCH;
        const auto key = reinterpret_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
        if (const auto clBase = s_crashLoggerBase.load(std::memory_order_acquire);
            clBase && key >= clBase && key < s_crashLoggerEnd.load(std::memory_order_relaxed))
            return EXCEPTION_CONTINUE_SEARCH;
        t_inVeh = true;
        const int seen = (std::min)(s_seenFaultCount.load(std::memory_order_acquire), kMaxSeenFaults);
        for (int i = 0; i < seen; ++i)
            if (s_seenFaults[i].code.load(std::memory_order_acquire) == code &&
                s_seenFaults[i].addr.load(std::memory_order_relaxed) == key) {
                t_inVeh = false;
                return EXCEPTION_CONTINUE_SEARCH;
            }
        // Full: no claim (an unbounded count would wrap and index outside the table).
        if (seen >= kMaxSeenFaults) {
            s_seenFaultsFull.store(true, std::memory_order_relaxed);
            t_inVeh = false;
            return EXCEPTION_CONTINUE_SEARCH;
        }
        const int slot = s_seenFaultCount.fetch_add(1, std::memory_order_acq_rel);
        if (slot >= kMaxSeenFaults) { t_inVeh = false; return EXCEPTION_CONTINUE_SEARCH; }   // lost the race for the last slot
        SeenFault& f = s_seenFaults[slot];
        f.addr.store(key, std::memory_order_relaxed);
        f.code.store(code, std::memory_order_release);
        f.tid = GetCurrentThreadId();
        // An overflow, or too little stack to walk safely, gets its line only.
        f.overflow = code == EXCEPTION_STACK_OVERFLOW;
        f.frameCount = (!f.overflow && StackLeft() >= kStackToWalk) ? WalkFaultStack(ep, f.frames, f.callerFirst) : 0;
        f.ready.store(true, std::memory_order_release);
        if (s_faultEvent) SetEvent(s_faultEvent);
        t_inVeh = false;
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // The watchdog thread's half: finds CrashLogger's image once it is loaded, then writes every ready slot not
    // yet logged. A crash usually still leaves time for it (CrashLogger writes its report first).
    static void DrainFaultReports()
    {
        if (!s_crashLoggerEnd.load(std::memory_order_relaxed)) {
            if (HMODULE cl = GetModuleHandleW(L"CrashLogger.dll")) {
                const auto base = reinterpret_cast<std::uintptr_t>(cl);
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(cl);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
                s_crashLoggerEnd.store(base + nt->OptionalHeader.SizeOfImage, std::memory_order_relaxed);
                s_crashLoggerBase.store(base, std::memory_order_release);
            }
        }
        static bool s_explained = false;
        const int seen = (std::min)(s_seenFaultCount.load(std::memory_order_acquire), kMaxSeenFaults);
        for (int i = 0; i < seen; ++i) {
            SeenFault& f = s_seenFaults[i];
            if (f.logged || !f.ready.load(std::memory_order_acquire)) continue;
            f.logged = true;
            void* addr = reinterpret_cast<void*>(f.addr.load(std::memory_order_relaxed));
            char modPath[MAX_PATH]; std::uintptr_t offset = 0;
            const char* modName = ModuleOffsetOf(addr, modPath, offset);
            if (_stricmp(modName, "CrashLogger.dll") == 0) continue;   // before its range was known
            if (!s_explained) {
                s_explained = true;
                SKSE::log::info("Magelight VEH: each line below is the first time a severe exception was raised at "
                                "that address, whoever raised it. Most are handled by the mod that raised them and "
                                "are not crashes; a crash is in CrashLogger's report. Magelight only records them.");
            }
            SKSE::log::error("Magelight VEH: exception 0x{:08X}{} at {} (module {} +0x{:X}) on thread {}",
                             f.code.load(std::memory_order_relaxed), f.overflow ? " (stack overflow)" : "",
                             addr, modName, offset, f.tid);
            // Frame 0 is the faulting address, on the line above, except after a call through null (its caller).
            for (int k = f.callerFirst ? 0 : 1; k < f.frameCount; ++k) {
                char path[MAX_PATH]; std::uintptr_t off = 0;
                const char* base = ModuleOffsetOf(f.frames[k], path, off);
                SKSE::log::error("Magelight VEH:   #{:02} {}+0x{:X}", k, base, off);
            }
        }
        static bool s_fullLogged = false;
        if (!s_fullLogged && s_seenFaultsFull.load(std::memory_order_relaxed)) {
            s_fullLogged = true;
            SKSE::log::error("Magelight VEH: {} distinct fault sites logged; later new ones are not", kMaxSeenFaults);
        }
    }

    // Magelight.json "exceptionTelemetry": false - take the handler out of the chain.
    static void ApplyExceptionTelemetrySetting()
    {
        if (s_exceptionTelemetry.load() || !s_vehHandle) return;
        RemoveVectoredExceptionHandler(s_vehHandle);
        s_vehHandle = nullptr;
        SKSE::log::info("Magelight: vectored exception telemetry off (Magelight.json \"exceptionTelemetry\": false)");
    }

    // ── Stall watchdog (0.28.3) ──────────────────────────────────────────
    // A hard freeze leaves no crash log and the tester cannot always produce
    // a dump; a soft stall (one frame every few seconds) leaves no trace at
    // all beyond our own cadence lines. Both reports of 2026-09-07 — the
    // outfit-page freeze and the quick wheel starving the loop in VR — ended
    // as "somewhere on the game thread, we do not know where". This thread
    // notices when the present hook has not stamped a frame for
    // stallThresholdMs while the world is live, suspends the thread that
    // presented the last frame (and the main thread when that is a different
    // one), walks its stack with the OS unwinder, resumes it, and logs the
    // frames as module+offset — what a process dump would show, captured
    // automatically. Rules that keep it safe:
    //   - the ONLY call made while the thread is suspended is
    //     GetThreadContext. The walk runs after ResumeThread: it is not
    //     lock-free (RtlLookupFunctionEntry takes ntdll's function-table
    //     locks and runs dynamic-table callbacks — JavaScriptCore registers
    //     JIT tables on the present thread), so a thread stalled inside one
    //     of those would have parked the watchdog with the target still
    //     suspended: a 1.5 s stall turned permanent by its own diagnostic
    //     (review 2026-09-09). Walking the LIVE stack after resume is sound
    //     for the case that matters — a truly stalled thread has not moved
    //     — and a thread that did move yields a garbage tail the SEH net
    //     absorbs. Module names are resolved after resume as before;
    //   - RtlLookupFunctionEntry/RtlVirtualUnwind are what exception
    //     dispatch itself uses on any thread, no dbghelp, no symbols;
    //   - the walk is SEH-netted in a function with no C++ objects;
    //   - rate-limited (one sample per 5 s, 12 per session) so a genuine
    //     freeze cannot flood the log, and a long stall is re-sampled so the
    //     log shows whether the stack MOVES (starvation) or not (deadlock).
    // Walks from a context captured under suspension; the thread is RUNNING
    // again by the time this is called (see the rules above).
    // NtQueryInformationThread's ThreadBasicInformation (class 0): enough to find a thread's TEB, whose NT_TIB holds
    // the committed part of its stack.
    struct MlThreadBasicInfo {
        LONG ExitStatus;
        PVOID TebBaseAddress;
        struct { HANDLE UniqueProcess; HANDLE UniqueThread; } ClientId;
        ULONG_PTR AffinityMask;
        LONG Priority;
        LONG BasePriority;
    };

    static bool ThreadStackRange(HANDLE thread, std::uintptr_t& low, std::uintptr_t& high)
    {
        using QueryFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
        static const auto query = reinterpret_cast<QueryFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
        MlThreadBasicInfo info{};
        if (!query || query(thread, 0, &info, sizeof(info), nullptr) < 0 || !info.TebBaseAddress) return false;
        const auto* tib = static_cast<const NT_TIB*>(info.TebBaseAddress);
        low  = reinterpret_cast<std::uintptr_t>(tib->StackLimit);
        high = reinterpret_cast<std::uintptr_t>(tib->StackBase);
        return high > low;
    }

    // Walks only inside [stackLow, stackHigh), the committed stack: a thread that ran on after its context was taken
    // leaves a stale one, and following it past the stack could clear another thread's guard page.
    static int UnwindFromContext(CONTEXT* ctx, void** frames, int cap, std::uintptr_t stackLow, std::uintptr_t stackHigh)
    {
        int n = 0;
        __try {
            while (n < cap && ctx->Rip) {
                frames[n++] = reinterpret_cast<void*>(ctx->Rip);
                if (ctx->Rsp < stackLow || ctx->Rsp + 8 > stackHigh) break;
                DWORD64 imageBase = 0;
                auto* fn = RtlLookupFunctionEntry(ctx->Rip, &imageBase, nullptr);
                if (!fn) {
                    // Leaf function: the return address sits at RSP.
                    ctx->Rip = *reinterpret_cast<DWORD64*>(ctx->Rsp);
                    ctx->Rsp += 8;
                    continue;
                }
                void*   handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx->Rip, fn, ctx, &handlerData, &establisher, nullptr);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return n > 0 ? n : -2;
        }
        return n;
    }

    static constexpr int kMaxStackFrames = 40;

    // `pageFrame`: the sample is the page thread's (CheckPageThreadStall), whose frame ran gapMs while the game went
    // on presenting.
    static void LogThreadStack(DWORD tid, const char* label, long long gapMs, int maxFrames = kMaxStackFrames,
                               bool pageFrame = false)
    {
        void* frames[kMaxStackFrames]{};
        int   n = -3;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!h) {
            SKSE::log::warn("Magelight stall: cannot open {} thread {} (error {})", label, tid, GetLastError());
            return;
        }
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        bool haveCtx = false;
        if (SuspendThread(h) != static_cast<DWORD>(-1)) {
            haveCtx = GetThreadContext(h, &ctx) != 0;   // the only call under suspension
            ResumeThread(h);
        }
        std::uintptr_t stackLow = 0, stackHigh = 0;
        const bool haveStack = ThreadStackRange(h, stackLow, stackHigh);
        CloseHandle(h);
        if (haveCtx) {
            // Without the stack range, only the frame the context names.
            t_walkingStack = true;
            n = UnwindFromContext(&ctx, frames, haveStack ? (std::min)(maxFrames, kMaxStackFrames) : 1, stackLow, stackHigh);
            t_walkingStack = false;
        } else {
            n = -1;
        }
        if (n <= 0) {
            SKSE::log::warn("Magelight stall: no stack for {} thread {} (code {})", label, tid, n);
            return;
        }
        SKSE::log::warn("Magelight stall: {} for {} ms — {} thread {} stack ({} frames):",
                        pageFrame ? "page frame running" : "no frame presented", gapMs, label, tid, n);
        for (int i = 0; i < n; ++i) {
            char path[MAX_PATH]; std::uintptr_t off = 0;
            const char* base = ModuleOffsetOf(frames[i], path, off);
            SKSE::log::warn("Magelight stall:   #{:02} {}+0x{:X}", i, base, off);
        }
    }

    // At most 64 threads, 24 frames each; the watchdog's own thread is skipped.
    static void LogOtherThreadStacks(DWORD presentTid, DWORD mainTid, long long gapMs)
    {
        const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) {
            SKSE::log::warn("Magelight stall: cannot list threads (error {})", GetLastError());
            return;
        }
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        int logged = 0;
        for (BOOL ok = Thread32First(snap, &te); ok && logged < 64; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid) continue;
            const DWORD tid = te.th32ThreadID;
            if (tid == self || tid == presentTid || tid == mainTid) continue;
            LogThreadStack(tid, "other", gapMs, 24);
            ++logged;
        }
        CloseHandle(snap);
        SKSE::log::warn("Magelight stall: {} other thread(s) sampled", logged);
    }

    static void CheckPageThreadStall();   // fwd (the page thread)

    static void StallWatchdogLoop()
    {
        using namespace std::chrono;
        long long lastReportMs = 0;
        int       reports      = 0;
        bool      away         = false;   // the last check found the game in the background
        long long awayStartMs  = 0;       // when the current stretch in the background began
        long long bgLastMs     = 0;       // the last check that found the game in the background
        long long bgPresentMs  = 0;       // the last present seen at that check
        for (;;) {
            if (s_faultEvent) WaitForSingleObject(s_faultEvent, 250);
            else std::this_thread::sleep_for(milliseconds(250));
            DrainFaultReports();
            if (s_stallWatchdog.load()) CheckPageThreadStall();
            if (!s_stallWatchdog.load() || reports >= 12) continue;
            const long long lastMs = s_lastPresentMs.load();
            if (!lastMs || !s_worldReady.load()) continue;   // nothing to watch yet
            const long long now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
            // A game in the background (alt-tab) stops presenting by design, so a frame gap counts only from its return
            // to the foreground. A window of the game's own process (a plugin's dialog) counts as the foreground, and so
            // does a hung game window (IsHungAppWindow: no messages pumped for 5 s, and a hung window never gets the
            // foreground back), so a hang is still sampled. Not on VR, where the desktop window is seldom the
            // foreground one while the headset plays.
            const bool hung = s_hwnd && IsHungAppWindow(s_hwnd);
            bool inBackground = false;
            if (!REL::Module::IsVR() && s_hwnd && !hung) {
                DWORD fgPid = 0;
                if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &fgPid);
                inBackground = fgPid != GetCurrentProcessId();
            }
            if (inBackground) {
                if (!away) {
                    away = true;
                    awayStartMs = now;
                }
                bgLastMs = now;
                bgPresentMs = lastMs;
                continue;
            }
            if (away) {
                away = false;
                if (bgLastMs - bgPresentMs >= s_stallThresholdMs.load()) {
                    SKSE::log::info("Magelight stall: none sampled - the game presented no frame for {} ms while its window "
                                    "was in the background ({} ms away)", bgLastMs - bgPresentMs, bgLastMs - awayStartMs);
                }
            }
            const long long gap = now - (hung ? lastMs : std::max(lastMs, bgLastMs));
            if (gap < s_stallThresholdMs.load()) continue;
            if (now - lastReportMs < 5000) continue;
            lastReportMs = now;
            ++reports;
            const DWORD presentTid = s_lastPresentTid.load();
            const DWORD mainTid    = s_mainThreadId;
            SKSE::log::warn("Magelight stall: sample {} — uiMode={} view={} (present thread {}, main thread {})",
                            reports, s_focused.load(), s_uiModeView.load(), presentTid, mainTid);
            if (presentTid) LogThreadStack(presentTid, "present", gap);
            if (mainTid && mainTid != presentTid) LogThreadStack(mainTid, "main", gap);
            // Once per session: every other thread too. A frame stuck on a lock names the waiter; these name the
            // owner (an SKSE task drain on a VR job thread, a mod's worker).
            if (reports == 1) LogOtherThreadStacks(presentTid, mainTid, gap);
        }
    }

    void InstallCrashTelemetry()
    {
        s_faultEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        s_vehHandle = AddVectoredExceptionHandler(1, &VectoredLogger);
        SKSE::log::info("Magelight: vectored exception telemetry armed");
        std::thread(&StallWatchdogLoop).detach();
        SKSE::log::info("Magelight: stall watchdog armed (threshold {} ms; Magelight.json \"stallWatchdog\": false disables)",
                        s_stallThresholdMs.load());
    }

    bool PreloadRuntime()
    {
        // SKSEPluginLoad runs on the game's MAIN thread — the same thread that
        // presents gameplay frames (menus/loading present from a separate
        // render thread). Recorded here so init can target it deterministically.
        s_mainThreadId = GetCurrentThreadId();
        SKSE::log::info("Magelight: main thread {}", s_mainThreadId);

        s_runtimeDir = GameRoot() / L"Data" / L"SKSE" / L"Plugins" / L"Magelight";
        // NAMESPACED runtime (coexistence with other Ultralight-based UI mods,
        // field-found 2026-08-31): another such host can ship the same four
        // Ultralight module names, and Windows binds imports by BASE NAME —
        // whoever loads "Ultralight.dll" first poisons the other host with the
        // wrong build (SKSE loads plugins alphabetically, so Magelight's preload
        // broke the other host's load). Our copies are renamed at stage time and
        // every import-name string inside them is patched to match (same-length
        // PE edit — see build.ps1), so both Ultralight runtimes coexist under
        // different module names.
        // Dependency order matters: core first, WebCore before Ultralight.
        const wchar_t* dlls[] = { L"Magelight1Core.dll", L"MgWCore.dll", L"Magelight1.dll" };
        for (const auto* name : dlls) {
            const auto full = s_runtimeDir / name;
            if (!LoadLibraryW(full.c_str())) {
                SKSE::log::error("Magelight: failed to load {} (GetLastError={})",
                    PathText::ToUtf8(name), GetLastError());
                return false;
            }
        }
        if (!std::filesystem::exists(s_runtimeDir / L"resources" / L"icudt67l.dat")) {
            SKSE::log::error("Magelight: resources/icudt67l.dat missing under {}",
                SysInfo::ForLog(s_runtimeDir));
            return false;
        }
        // The runtime must be the SDK this host was built against: a mod manager can pair Magelight.dll from
        // one mod with the runtime DLLs of another, and a different Ultralight version means different vtables
        // (a crash inside a frame), so a mismatch leaves the host inert instead.
        if (HMODULE ul = GetModuleHandleW(L"Magelight1.dll")) {
            using VersionFn = const char* (*)();
            const auto ver = reinterpret_cast<VersionFn>(GetProcAddress(ul, "ulVersionString"));
            const auto wk  = reinterpret_cast<VersionFn>(GetProcAddress(ul, "ulWebKitVersionString"));
            const std::string have = ver ? ver() : "";
            SKSE::log::info("Magelight: Ultralight {} (WebKit {})", have.empty() ? "?" : have, wk ? wk() : "?");
            if (have != ULTRALIGHT_VERSION) {
                SKSE::log::error("Magelight: the Ultralight runtime under {} is version '{}', but this Magelight.dll "
                    "was built for {} - another mod's copy of the runtime is installed over this one",
                    SysInfo::ForLog(s_runtimeDir), have, ULTRALIGHT_VERSION);
                return false;
            }
        }

        // Optional GPU backend. Loaded AFTER the runtime so its (namespaced)
        // Ultralight imports bind to the modules already in the process.
        // Missing/failed = CPU surface fallback, never fatal.
        if (HMODULE gpu = LoadLibraryW((s_runtimeDir / L"MagelightGPU.dll").c_str()); !gpu) {
            SKSE::log::info("Magelight: no GPU backend (MagelightGPU.dll not found, GetLastError={}) — CPU fallback",
                GetLastError());
        } else if (const std::string mismatch = GpuBuildMismatch(QueryGpuInfo(gpu)); !mismatch.empty()) {
            SKSE::log::error("Magelight: MagelightGPU.dll does not match this Magelight.dll - {}. Pages draw on "
                             "the CPU; another mod's copy of MagelightGPU.dll is installed over this one", mismatch);
        } else {
            s_gpuCreate    = reinterpret_cast<MgGpuCreateFn>(GetProcAddress(gpu, "MgGpu_Create"));
            s_gpuDestroy   = reinterpret_cast<MgGpuDestroyFn>(GetProcAddress(gpu, "MgGpu_Destroy"));
            s_gpuGetDriver = reinterpret_cast<MgGpuGetDrvFn>(GetProcAddress(gpu, "MgGpu_GetGPUDriver"));
            s_gpuHas       = reinterpret_cast<MgGpuHasFn>(GetProcAddress(gpu, "MgGpu_HasCommandsPending"));
            s_gpuDraw      = reinterpret_cast<MgGpuDrawFn>(GetProcAddress(gpu, "MgGpu_DrawCommandList"));
            s_gpuSrv       = reinterpret_cast<MgGpuSrvFn>(GetProcAddress(gpu, "MgGpu_GetTextureSRV"));
            s_gpuSamples   = reinterpret_cast<MgGpuSamplesFn>(GetProcAddress(gpu, "MgGpu_SetSampleCount"));
            s_gpuRegExt    = reinterpret_cast<MgGpuRegExtFn>(GetProcAddress(gpu, "MgGpu_RegisterExternalTexture"));
            s_gpuSetExtSrv = reinterpret_cast<MgGpuSetExtFn>(GetProcAddress(gpu, "MgGpu_SetExternalTextureSRV"));
            s_gpuUnregExt  = reinterpret_cast<MgGpuUnregExtFn>(GetProcAddress(gpu, "MgGpu_UnregisterExternalTexture"));
            s_gpuStats     = reinterpret_cast<MgGpuStatsFn>(GetProcAddress(gpu, "MgGpu_GetStats"));
            s_gpuTakeList  = reinterpret_cast<MgGpuTakeListFn>(GetProcAddress(gpu, "MgGpu_TakeCommandList"));
            // A backend of this contract exports all of them; one that does not is a broken build.
            if (s_gpuCreate && s_gpuDestroy && s_gpuGetDriver && s_gpuHas && s_gpuDraw && s_gpuSrv && s_gpuSamples
                && s_gpuRegExt && s_gpuSetExtSrv && s_gpuUnregExt && s_gpuStats && s_gpuTakeList) {
                SKSE::log::info("Magelight: GPU backend loaded (MagelightGPU.dll, contract {}, Ultralight {})",
                                MGGPU_CONTRACT, ULTRALIGHT_VERSION);
            } else {
                SKSE::log::error("Magelight: MagelightGPU.dll is missing exports — CPU fallback");
                s_gpuCreate = nullptr;
            }
        }

        s_runtimeReady.store(true);
        SKSE::log::info("Magelight: Ultralight runtime preloaded from {}", SysInfo::ForLog(s_runtimeDir));
        return true;
    }

    // ── Ultralight platform bridges ─────────────────────────────────────────

    class SpdLogger final : public ultralight::Logger {
    public:
        void LogMessage(ultralight::LogLevel level, const ultralight::String& message) override
        {
            const auto text = SysInfo::RedactProfile(message.utf8().data());
            if (level == ultralight::LogLevel::Fatal || level == ultralight::LogLevel::Error)
                SKSE::log::error("[UL] {}", text);
            else if (level == ultralight::LogLevel::Warning)
                SKSE::log::warn("[UL] {}", text);
            else
                SKSE::log::info("[UL] {}", text);
        }
    };
    static constinit NeverDestroyed<SpdLogger> s_ulLogger;

    // Our own FileSystem instead of AppCore's. The stock Windows one has no
    // entry for .mjs, resolves nothing behind a query string and serves any
    // path, and MODULE scripts hard-require a JavaScript MIME (classic
    // scripts don't care), so a Vite bundle loads as a fully transparent,
    // silently-empty page (the 0.5.1 field report). Ours also serves only
    // the page roots and injects each page's Content-Security-Policy.
    // Win32 clipboard for WebKit's editing commands (Ctrl+C/V/X, context
    // paste). Ultralight never sets one itself — without it every text field
    // silently ignores paste. Called on the render thread; OpenClipboard is
    // cheap and synchronous. Pages can only READ it through a user-initiated
    // paste (this WebKit build has no JS clipboard API).
    class MlClipboard final : public ultralight::Clipboard {
    public:
        void Clear() override
        {
            if (!OpenClipboard(s_hwnd)) return;
            EmptyClipboard();
            CloseClipboard();
        }
        ultralight::RefPtr<ultralight::ClipboardData> Read() override
        {
            return ultralight::ClipboardData::Create(ReadPlainText());
        }
        void Write(ultralight::RefPtr<ultralight::ClipboardData> data) override
        {
            if (data) WritePlainText(data->AsText());
        }
    private:
        ultralight::String ReadPlainText()
        {
            ultralight::String out;
            if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(s_hwnd)) return out;
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (auto* w = static_cast<const wchar_t*>(GlobalLock(h))) {
                    const std::size_t len = wcsnlen(w, GlobalSize(h) / sizeof(wchar_t));
                    ultralight::String16 s16(reinterpret_cast<const ultralight::Char16*>(w), len);
                    out = ultralight::String(s16);
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
            return out;
        }
        void WritePlainText(const ultralight::String& text)
        {
            const ultralight::String16 s16 = text.utf16();
            const std::size_t len = s16.length();
            if (!OpenClipboard(s_hwnd)) return;
            EmptyClipboard();
            if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(wchar_t))) {
                if (auto* dst = static_cast<wchar_t*>(GlobalLock(h))) {
                    std::memcpy(dst, s16.data(), len * sizeof(wchar_t));
                    dst[len] = L'\0';
                    GlobalUnlock(h);
                    if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
                } else {
                    GlobalFree(h);
                }
            }
            CloseClipboard();
        }
    };
    static constinit NeverDestroyed<MlClipboard> s_clipboard;

    // Network sandbox: a page's reach is its view's NetLevel, the owning mod's
    // NetworkPolicy (File unless the mod opted in). Tightening a default later
    // breaks mods, which is why the default is the tightest level.
    // The loopback hosts are the two the page's CSP (CspFor) admits: an exact
    // host match, never a prefix test (127.attacker.example is a legal name).
    static bool IsLoopbackUrl(const std::string& url)
    {
        const auto p = url.find("://");
        if (p == std::string::npos) return false;
        std::string host = url.substr(p + 3);
        const auto end = host.find_first_of("/?#");
        if (end != std::string::npos) host.resize(end);
        if (const auto at = host.rfind('@'); at != std::string::npos) host.erase(0, at + 1);
        if (!host.empty() && host[0] == '[') {
            const auto rb = host.find(']');
            host = (rb == std::string::npos) ? host : host.substr(0, rb + 1);
        } else if (const auto c = host.rfind(':'); c != std::string::npos) {
            host.resize(c);
        }
        for (auto& ch : host) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return host == "localhost" || host == "127.0.0.1";
    }

    // The network gate behind the page's CSP (CspFor): Ultralight calls it for http(s) loads only. It never
    // sees file:, data:, synchronous XHR or WebSocket requests, so the file gate is MlFileSystem's and the
    // policy is what refuses the other two. Render thread; it reads the view registry only under s_viewsMutex.
    class MlNetworkListener final : public ultralight::NetworkListener {
    public:
        bool OnNetworkRequest(ultralight::View* caller, ultralight::NetworkRequest& req) override
        {
            const std::string proto = req.urlProtocol().utf8().data();
            if (proto == "data" || proto == "about" || proto == "blob" || proto == "file") return true;
            // The owning mod decides (SetNetworkPolicy or the manifest's
            // "network"): Loopback lets a page talk to a server on this
            // machine, which can itself relay anywhere, so it is an opt-in
            // too; Any is for pages that talk to the internet, where a host
            // allowlist would break with every new host.
            NetLevel level = NetLevel::File;
            ViewId id = 0;
            {
                std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
                if (const MlView* v = FindViewByUlLocked(caller)) { level = v->netLevel; id = v->id; }
            }
            if (level == NetLevel::Any) return true;
            if (level == NetLevel::Loopback && (proto == "http" || proto == "https") &&
                IsLoopbackUrl(req.url().utf8().data()))
                return true;
            static std::atomic<int> s_logged{ 0 };
            if (s_logged.fetch_add(1) < 32) {
                SKSE::log::warn("Magelight: view {} blocked network request {} {} (protocol '{}') — its mod's network policy is {}",
                    id, req.httpMethod().utf8().data(), req.url().utf8().data(), proto,
                    level == NetLevel::Loopback ? "loopback (localhost and 127.0.0.1 only)" : "file only");
            }
            return false;
        }
    };
    static constinit NeverDestroyed<MlNetworkListener> s_networkListener;

    // Page roots outside Data\Magelight (an absolute page registers its own
    // directory — PageRootFor). Append-only and never pruned: a root is a
    // directory, and keeping a destroyed view's folder readable costs
    // nothing, whereas pruning would race a worker-thread read against the
    // destroy. Own mutex, not s_viewsMutex: MlFileSystem::Resolve runs on
    // WebKit's worker threads while the game thread may hold s_viewsMutex.
    static std::mutex s_pageRootsMutex;
    // Each root carries the network level of the views that load from it: the
    // page's CSP is chosen by the FILE's root, since the file system cannot
    // tell which view asked. The last view created or policy set for a root
    // wins (a mod's views all share its level). The runtime dir is never
    // noted: host pages stay file-only whatever a relative-path view asks.
    struct PageRoot { std::filesystem::path path; NetLevel net = NetLevel::File; };
    static std::vector<PageRoot> s_pageRoots;
    static void NotePageRoot(const std::filesystem::path& root, NetLevel net)
    {
        if (root.empty() || root == s_runtimeDir) return;
        std::lock_guard<std::mutex> lk(s_pageRootsMutex);
        for (auto& r : s_pageRoots) if (r.path == root) { r.net = net; return; }
        s_pageRoots.push_back({ root, net });
    }
    static bool IsUnderRegisteredPageRoot(const std::filesystem::path& p)
    {
        std::lock_guard<std::mutex> lk(s_pageRootsMutex);
        for (const auto& r : s_pageRoots) if (PathIsUnder(p, r.path)) return true;
        return false;
    }
    // The level a page at `p` gets: the deepest registered root containing it (a mod folder inside the mods
    // root beats the mods root itself); file-only when no root contains it.
    static NetLevel RootNetLevel(const std::filesystem::path& p)
    {
        std::lock_guard<std::mutex> lk(s_pageRootsMutex);
        NetLevel level = NetLevel::File;
        std::size_t best = 0;
        for (const auto& r : s_pageRoots) {
            const std::size_t len = r.path.native().size();
            if (len >= best && PathIsUnder(p, r.path)) { best = len; level = r.net; }
        }
        return level;
    }

    // The Content-Security-Policy MlFileSystem writes into every page, by its mod's NetworkPolicy. Ultralight
    // routes file:, data:, synchronous XHR and WebSocket requests past NetworkListener, so the listener alone
    // let a File page open a WebSocket, and a document's synchronous XHR read any file on disk; the policy is
    // what closes them. Rules the probe matrix settled:
    //  - connect-src never lists file: — a page cannot fetch its own files (Vite's modulepreload polyfill
    //    logs one refusal; @magelight/vite-plugin turns the polyfill off), and the sync-XHR disk read is gone.
    //  - frame-src never lists file: or http(s): every file:/// document shares one origin, so a frame of a
    //    looser mod's page would lend that page its policy; object-src 'none' closes the <object> route.
    //  - Loopback is http(s)/ws(s) to localhost and 127.0.0.1 only: CSP cannot express 127.0.0.0/8 or [::1].
    //  - 'unsafe-inline' and 'unsafe-eval' stay: mod pages carry inline scripts.
    // A top-level navigation is outside CSP (MlLoadListener::OnBeginLoading stops it), and <link rel=preload
    // as=fetch> still sends one GET to any host: no 1.4 API reaches it.
    static std::string CspFor(NetLevel level)
    {
        switch (level) {
        case NetLevel::Loopback:
            return "default-src file: data: blob: 'unsafe-inline' 'unsafe-eval'"
                   " http://localhost:* http://127.0.0.1:* https://localhost:* https://127.0.0.1:*;"
                   " connect-src data: blob: http://localhost:* http://127.0.0.1:* https://localhost:* https://127.0.0.1:*"
                   " ws://localhost:* ws://127.0.0.1:* wss://localhost:* wss://127.0.0.1:*;"
                   " frame-src data:; form-action file: http://localhost:* http://127.0.0.1:* https://localhost:* https://127.0.0.1:*;"
                   " object-src 'none'";
        case NetLevel::Any:
            return "default-src * file: data: blob: 'unsafe-inline' 'unsafe-eval';"
                   " connect-src http: https: ws: wss: data: blob:; frame-src * data:; form-action *; object-src 'none'";
        default:
            return "default-src file: data: blob: 'unsafe-inline' 'unsafe-eval'; connect-src data: blob:;"
                   " frame-src data:; form-action file:; object-src 'none'";
        }
    }
    static std::string CspMetaFor(NetLevel level)
    {
        return "<meta http-equiv=\"Content-Security-Policy\" content=\"" + CspFor(level) + "\">";
    }
    // Writes the policy into an HTML document right after its BOM, leading whitespace, comments, XML prolog
    // and doctype: no page byte that can run or load anything comes before it, and the doctype still decides
    // the mode (a tag ahead of the doctype would put the page in quirks mode). Without a doctype it goes
    // after the <html> start tag, or at the top of a document with none. Inserting after <head> was beaten
    // by a script written before it (and by "<head>" inside a comment).
    static void InjectCsp(std::string& html, NetLevel level)
    {
        const auto startsWith = [&](std::size_t at, const char* tok) {
            const std::size_t n = std::strlen(tok);
            return html.size() - at >= n && _strnicmp(html.data() + at, tok, n) == 0;
        };
        std::size_t at = 0;
        if (startsWith(0, "\xEF\xBB\xBF")) at = 3;
        for (;;) {
            while (at < html.size() && std::isspace(static_cast<unsigned char>(html[at]))) ++at;
            if (startsWith(at, "<!--")) {
                const std::size_t end = html.find("-->", at + 4);
                if (end == std::string::npos) break;
                at = end + 3;
            } else if (startsWith(at, "<?")) {
                const std::size_t end = html.find("?>", at + 2);
                if (end == std::string::npos) break;
                at = end + 2;
            } else {
                break;
            }
        }
        if (startsWith(at, "<!doctype")) {
            const std::size_t gt = html.find('>', at);
            if (gt != std::string::npos) at = gt + 1;
        } else if (startsWith(at, "<html")) {
            const std::size_t gt = html.find('>', at);
            if (gt != std::string::npos) at = gt + 1;
        }
        html.insert(at, CspMetaFor(level));
    }

    class MlFileSystem final : public ultralight::FileSystem {
    public:
        // The path Ultralight asks for (PathText::PathOfFileRequest) as a filesystem path, refused outside the page roots.
        std::filesystem::path Resolve(const ultralight::String& file_path)
        {
            const std::string p = PathText::PathOfFileRequest(file_path.utf8().data());
            std::filesystem::path out = (p.size() > 1 && p[1] == ':')
                ? PathText::FromUtf8(p).make_preferred()
                : (s_runtimeDir / PathText::FromUtf8(p).make_preferred());
            // The file gate: nothing outside the host runtime dir,
            // Data\Magelight or a registered page's own root is ever served,
            // and ".." is normalised away before the test so it cannot climb
            // out. This layer is view-blind (WebKit asks for files from a
            // worker thread with no View in hand), so it accepts the UNION of
            // page roots; per-page isolation is not enforced (file: requests
            // never reach NetworkListener, so no per-view check can exist).
            // 0.28.1: the union used to be just Data\Magelight, which refused
            // every absolute page living elsewhere — SkyrimNet's dashboard
            // under a folder outside Data\Magelight loaded as an empty view.
            out = out.lexically_normal();
            static const std::filesystem::path s_modsRoot = (GameRoot() / L"Data" / L"Magelight").lexically_normal();
            if (PathIsUnder(out, s_runtimeDir) || PathIsUnder(out, s_modsRoot) || IsUnderRegisteredPageRoot(out)) return out;
            static std::atomic<int> s_logged{ 0 };
            if (s_logged.fetch_add(1) < 32)
                SKSE::log::warn("Magelight: file read outside Data\\Magelight or any page root refused — {}", Magelight::SysInfo::ForLog(out));
            return {};
        }
        bool FileExists(const ultralight::String& file_path) override
        {
            const auto p = Resolve(file_path);
            if (p.empty()) return false;
            std::error_code ec;
            return std::filesystem::is_regular_file(p, ec);
        }
        static std::string LowerExt(const std::filesystem::path& p) { return HotkeyNames::Lower(PathText::ToUtf8(p.extension())); }
        static bool IsHtmlExt(const std::string& ext) { return ext == ".html" || ext == ".htm"; }
        ultralight::String GetFileMimeType(const ultralight::String& file_path) override
        {
            const std::string ext = LowerExt(Resolve(file_path));
            if (IsHtmlExt(ext)) return "text/html";
            if (ext == ".js" || ext == ".mjs") return "application/javascript";
            if (ext == ".css") return "text/css";
            if (ext == ".json" || ext == ".map") return "application/json";
            if (ext == ".svg") return "image/svg+xml";
            if (ext == ".png") return "image/png";
            if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
            if (ext == ".gif") return "image/gif";
            if (ext == ".webp") return "image/webp";
            if (ext == ".ico") return "image/x-icon";
            if (ext == ".woff") return "font/woff";
            if (ext == ".woff2") return "font/woff2";
            if (ext == ".ttf") return "font/ttf";
            if (ext == ".otf") return "font/otf";
            if (ext == ".txt") return "text/plain";
            if (ext == ".wasm") return "application/wasm";
            return "application/octet-stream";
        }
        ultralight::String GetFileCharset(const ultralight::String&) override
        {
            return "utf-8";
        }
        ultralight::RefPtr<ultralight::Buffer> OpenFile(const ultralight::String& file_path) override
        {
            const auto path = Resolve(file_path);
            if (path.empty()) return nullptr;   // refused by the file gate (already logged)
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            if (!f) {
                SKSE::log::warn("Magelight: OpenFile failed: {}", Magelight::SysInfo::ForLog(path));
                return nullptr;
            }
            const std::streamsize size = f.tellg();
            f.seekg(0, std::ios::beg);
            std::string data(static_cast<size_t>(size), '\0');
            if (size > 0 && !f.read(data.data(), size)) {
                SKSE::log::warn("Magelight: OpenFile read failed: {}", Magelight::SysInfo::ForLog(path));
                return nullptr;
            }
            // Every page gets its mod's CSP (CspFor). The hosted Web Inspector is left as shipped: it is a
            // dev surface served from the runtime dir, and its frontend was not written against a policy.
            if (IsHtmlExt(LowerExt(path)) && !PathIsUnder(path, s_runtimeDir / L"inspector"))
                InjectCsp(data, RootNetLevel(path));
            return ultralight::Buffer::CreateFromCopy(data.data(), data.size());
        }
    };
    static constinit NeverDestroyed<MlFileSystem> s_fileSystem;

    // Case-insensitive prefix test on normalised native paths (Windows). Pure
    // string work — safe on WebKit's thread, no filesystem calls.
    static bool PathIsUnder(const std::filesystem::path& p, const std::filesystem::path& base)
    {
        if (p.empty() || base.empty()) return false;
        std::wstring a = p.lexically_normal().native();
        std::wstring b = base.lexically_normal().native();
        while (!b.empty() && (b.back() == L'\\' || b.back() == L'/')) b.pop_back();
        if (a.size() < b.size()) return false;
        for (std::size_t i = 0; i < b.size(); ++i)
            if (std::towlower(a[i]) != std::towlower(b[i])) return false;
        return a.size() == b.size() || a[b.size()] == L'\\' || a[b.size()] == L'/';
    }

    // The folder a page may read from (MlView::root). A drive-letter page
    // under Data\Magelight\<Mod>\... pins to that mod folder; any other
    // absolute page pins to its own directory; a relative (host) page pins
    // to the runtime dir. Host resources are always readable on top.
    static std::filesystem::path PageRootFor(const std::string& htmlPath)
    {
        const bool absolute = htmlPath.size() > 1 && htmlPath[1] == ':';
        if (!absolute) return s_runtimeDir;
        const std::filesystem::path page = PathText::FromUtf8(htmlPath).lexically_normal();
        const std::filesystem::path modsRoot = (GameRoot() / L"Data" / L"Magelight").lexically_normal();
        if (PathIsUnder(page, modsRoot)) {
            // modsRoot / <Mod> — the first component below Data\Magelight
            const std::filesystem::path rel = page.lexically_relative(modsRoot);
            auto it = rel.begin();
            if (it != rel.end()) return modsRoot / *it;
        }
        return page.parent_path();
    }

    // Page console output into Magelight.log — a JS error, refused module, or
    // missing asset names itself instead of rendering as a blank panel.
    // 1.4: the virtual collapsed from 7 args to (View*, const ConsoleMessage&).
    // Without `override` the old signature would still compile as an unrelated
    // function and console logging would silently vanish — keep `override` on
    // every listener method, always. (MessageLevel/MessageSource also
    // RENUMBERED in 1.4 — compare by enum NAME only, never by stored int.)
    static ultralight::RefPtr<ultralight::View> HostInspectorView(ultralight::View* caller);

    static std::string MlJsonQuote(const std::string& in)
    {
        return JsonQuote(in);   // shared with the Api4 layer (Magelight.h)
    }

    // `in` cut to at most maxBytes on a UTF-8 character boundary, with an ellipsis and the original length.
    static std::string CapLogText(const std::string& in, std::size_t maxBytes)
    {
        if (in.size() <= maxBytes) return in;
        std::size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(in[cut]) & 0xC0) == 0x80) --cut;
        return in.substr(0, cut) + "\xE2\x80\xA6 [" + std::to_string(in.size()) + " bytes]";
    }

    // devMode: a JS error paints a banner on the page itself, so a broken
    // handler is seen where it happened instead of only in the log. Fixed,
    // click-through, self-removing; the page's own DOM is untouched.
    // Any thread but one holding s_viewsMutex. unnamed: a line with no source is one of the unnamed code that made the
    // error (JsBridge::ScriptLine). A console message's is not: it has no stack to tell a mod's script from a JSC
    // built-in that called the console method (a page file's forEach(console.error) reads line 1, no source).
    static void ShowDevErrorOverlay(ViewId view, const std::string& text, const std::string& source, std::uint32_t line,
                                    bool unnamed)
    {
        // The file name alone; the whole source when it ends in a separator (a directory URL), which names no file.
        std::string where = source;
        if (auto slash = where.find_last_of("/\\"); slash != std::string::npos && slash + 1 < where.size())
            where = where.substr(slash + 1);
        const std::string msg = !source.empty() ? text + "\n" + where + ":" + std::to_string(line)
                              : unnamed && line ? text + "\nline " + std::to_string(line) + " of an unnamed script" : text;
        std::string script =
            "(function(){var t=" + MlJsonQuote(msg) + ";var d=document.getElementById('__ml_err');"
            "if(!d){d=document.createElement('div');d.id='__ml_err';d.style.cssText='position:fixed;left:0;right:0;top:0;"
            "z-index:2147483647;background:#5a1414;color:#ffd9d9;font:12px/1.4 monospace;padding:6px 10px;"
            "border-bottom:2px solid #ff5c5c;white-space:pre-wrap;max-height:40%;overflow:auto;pointer-events:none';"
            "(document.body||document.documentElement).appendChild(d);}"
            "d.textContent='[Magelight devMode] JS error\\n'+t;clearTimeout(d.__t);"
            "d.__t=setTimeout(function(){d.remove();},12000);})();";
        // quiet: a banner that throws (a document with no root yet) must not report itself, and so queue another
        // banner, every frame.
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) QueueOutboundLocked(*v, { std::move(script), {}, true, true });
    }

    // Render thread, outside s_viewsMutex and outside the page's JS context lock. A script the host ran for a mod
    // threw. uncaught (InvokeJS, an InteropCall handler, the page core's dispatch): what an uncaught page error gets
    // (invariant 9), a log line when consoleLog records errors and the view's console budget allows, a level-2
    // ConsoleMessage event with the text and stack, and the devMode banner. !uncaught (EvalJS, which handed the
    // exception to the mod): the same log line, in devMode only.
    static void ReportScriptError(ViewId id, const std::string& what, const JsBridge::ErrorInfo& e, bool uncaught)
    {
        const bool dev = s_devMode.load();
        if (!uncaught && !dev) return;
        bool logIt = false;
        std::uint32_t dropped = 0;
        if (ConsoleLogLevel() != kConsoleLogNone) {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            if (MlView* v = FindViewLocked(id)) logIt = v->consoleRate.Admit(GetTickCount64(), dropped);
        }
        if (dropped)
            SKSE::log::info("Magelight: [view {} console] {} lines not logged (over {} a second)",
                id, dropped, kConsoleLinesPerSec);
        if (logIt)
            SKSE::log::info("Magelight: [view {} {}/{}] {}", id, what, uncaught ? "ERROR" : "caught",
                CapLogText(JsBridge::LogText(e), kConsoleLineMaxBytes));
        if (!uncaught) return;
        const std::string full = JsBridge::FullText(e);
        const unsigned line = JsBridge::ScriptLine(e);
        Emit(HostEvent::ConsoleMessage, id, 2, static_cast<int>(line), full.c_str());
        if (dev) ShowDevErrorOverlay(id, full, e.url, line, true);
    }

    class MlViewListener final : public ultralight::ViewListener {
    public:
        // Render thread, from inside a view's update: the CSS cursor under the pointer.
        void OnChangeCursor(ultralight::View* caller, ultralight::Cursor cursor) override
        {
            // try_lock: no path is known to fire this under s_viewsMutex, but the core is closed, and a missed cursor
            // change only waits for the next mouse move.
            PageCursor kind = PageCursor::Arrow;
            switch (cursor) {
            case ultralight::kCursor_Hand:
            case ultralight::kCursor_Grab:
            case ultralight::kCursor_Grabbing:
                kind = PageCursor::Clickable;
                break;
            case ultralight::kCursor_IBeam:
            case ultralight::kCursor_VerticalText:
                kind = PageCursor::Text;
                break;
            // Probed against the 1.4 core: cursor: none reports kCursor_None, and cursor: url(x) reports
            // kCursor_Custom only once its image loaded (a missing one falls to the next keyword in the list).
            case ultralight::kCursor_None:
                kind = PageCursor::None;
                break;
            case ultralight::kCursor_Custom:
                kind = PageCursor::Custom;
                break;
            default:
                break;
            }
            std::unique_lock<PageThread::OwnedMutex> lk(s_viewsMutex, std::try_to_lock);
            if (!lk) return;
            if (MlView* v = FindViewByUlLocked(caller)) v->pageCursor = static_cast<int>(kind);
        }
        // Render thread, inside a mouse event or an update: the title under the pointer ("" = none).
        void OnChangeTooltip(ultralight::View* caller, const ultralight::String& tooltip) override
        {
            std::lock_guard<std::mutex> lk(s_tooltipMutex);
            Tooltip::Coalesce(s_tooltipChanges, caller, std::string(tooltip.utf8().data()));
        }
        // Render thread, synchronously inside View::CreateLocalInspectorView.
        ultralight::RefPtr<ultralight::View> OnCreateInspectorView(ultralight::View* caller, bool,
                                                                   const ultralight::String&) override
        {
            return HostInspectorView(caller);
        }
    public:
        void OnAddConsoleMessage(ultralight::View* caller,
            const ultralight::ConsoleMessage& msg) override
        {
            const auto level = msg.level();
            const int lvlNum = (level == ultralight::kMessageLevel_Error) ? 2
                : (level == ultralight::kMessageLevel_Warning) ? 1 : 0;
            const int cfg = ConsoleLogLevel();
            const bool wanted = cfg == kConsoleLogAll || (cfg == kConsoleLogWarnings && lvlNum >= 1)
                || (cfg == kConsoleLogErrors && lvlNum == 2);
            ViewId id = 0;
            bool logIt = false;
            std::uint32_t dropped = 0;
            {
                std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
                MlView* v = FindViewByUlLocked(caller);
                if (v) id = v->id;
                if (wanted) {
                    // A page outside the registry (its View dropping after the erase) shares one budget.
                    static ConsoleRate s_unownedRate;
                    logIt = (v ? v->consoleRate : s_unownedRate).Admit(GetTickCount64(), dropped);
                }
            }
            // Every console argument, space-joined, each as JsBridge::ConsoleArgText
            // formats it. Outside s_viewsMutex: a toString, a getter, a toJSON or an
            // instanceof can run page script, which can call __mlNative.
            std::string text;
            if (JSContextRef ctx = msg.num_arguments() ? msg.argument_context() : nullptr) {
                for (std::uint32_t i = 0; i < msg.num_arguments(); ++i) {
                    if (i) text += ' ';
                    text += JsBridge::ConsoleArgText(ctx, msg.argument_at(i));
                }
            } else {
                text = msg.message().utf8().data();
            }
            const std::string source = msg.source_id().utf8().data();
            if (dropped)
                SKSE::log::info("Magelight: [view {} console] {} lines not logged (over {} a second)",
                    id, dropped, kConsoleLinesPerSec);
            if (logIt) {
                const char* lvl = level == ultralight::kMessageLevel_Error ? "ERROR"
                    : level == ultralight::kMessageLevel_Warning ? "warn"
                    : level == ultralight::kMessageLevel_Info ? "info"
                    : level == ultralight::kMessageLevel_Debug ? "debug" : "log";
                SKSE::log::info("Magelight: [view {} console/{}] {} ({}:{}:{})", id, lvl,
                    CapLogText(text, kConsoleLineMaxBytes), CapLogText(source, 512), msg.line_number(),
                    msg.column_number());
            }
            // The owning mod's event and the devMode banner get the whole message, whatever consoleLog says.
            Emit(HostEvent::ConsoleMessage, id, lvlNum, static_cast<int>(msg.line_number()), text.c_str());
            if (lvlNum == 2 && s_devMode.load() && id) ShowDevErrorOverlay(id, text, source, msg.line_number(), false);
        }
    };
    static constinit NeverDestroyed<MlViewListener> s_viewListener;

    // ── D3D helpers ─────────────────────────────────────────────────────────

    static constexpr const char* kShaderSrc = R"HLSL(
struct VSIn  { float2 pos : POS; float2 uv : TEX; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(VSIn i) { VSOut o; o.pos = float4(i.pos, 0.0, 1.0); o.uv = i.uv; return o; }
Texture2D    tex0 : register(t0);
SamplerState smp0 : register(s0);
cbuffer Cut : register(b0) { float4 cut; };   // uv rect (x0,y0,x1,y1); x1<=x0 = none
float4 ps_main(VSOut i) : SV_Target {
    if (cut.z > cut.x && i.uv.x >= cut.x && i.uv.x < cut.z && i.uv.y >= cut.y && i.uv.y < cut.w) discard;
    return tex0.Sample(smp0, i.uv);
}
// VR copy pass: Ultralight's targets are PREMULTIPLIED; an OpenVR overlay
// texture is composited as straight alpha (SteamVR's default without
// VROverlayFlags_IsPremultiplied, which IVROverlay_016 predates). Divide out.
float4 ps_straight(VSOut i) : SV_Target {
    if (cut.z > cut.x && i.uv.x >= cut.x && i.uv.x < cut.z && i.uv.y >= cut.y && i.uv.y < cut.w) discard;
    float4 c = tex0.Sample(smp0, i.uv);
    if (c.a > 0.0001) c.rgb /= c.a;
    return c;
}
)HLSL";

    struct Vertex { float x, y, u, v; };

    // Classic arrow, 12x19: 'X' = black outline, 'o' = white fill, '.' =
    // transparent. Baked into an immutable premultiplied-BGRA texture and
    // drawn as the topmost quad at the MenuCursor position (hotspot 0,0).
    static constexpr int kCursorW = 12;
    static constexpr int kCursorH = 19;
    static constexpr const char* kCursorMask[kCursorH] = {
        "X...........",
        "XX..........",
        "XoX.........",
        "XooX........",
        "XoooX.......",
        "XooooX......",
        "XoooooX.....",
        "XooooooX....",
        "XoooooooX...",
        "XooooooooX..",
        "XoooooXXXXX.",
        "XooXooX.....",
        "XoX.XooX....",
        "XX..XooX....",
        "X....XooX...",
        ".....XooX...",
        "......XooX..",
        "......XooX..",
        ".......XX...",
    };

    static bool CreateCursorTexture()
    {
        std::uint32_t px[kCursorW * kCursorH];
        for (int y = 0; y < kCursorH; ++y) {
            for (int x = 0; x < kCursorW; ++x) {
                const char c = kCursorMask[y][x];
                px[y * kCursorW + x] =
                    (c == 'X') ? 0xFF000000u : (c == 'o') ? 0xFFFFFFFFu : 0u;
            }
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kCursorW; td.Height = kCursorH;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sub{ px, kCursorW * sizeof(std::uint32_t), 0 };
        if (FAILED(s_device->CreateTexture2D(&td, &sub, &s_cursorTex))) return false;
        return SUCCEEDED(s_device->CreateShaderResourceView(s_cursorTex, nullptr, &s_cursorSrv));
    }

    // The VR laser-end pointer: a small white disc with a dark rim, alpha
    // antialiased, hotspot at the centre. An arrow made sense as a desktop
    // cursor; at the end of a laser it read as a big distracting sprite
    // (field 2026-09-06). 32x32 so it stays crisp when drawn at 8-16px.
    static constexpr int kDotSize = 32;
    // A core over a rim, both 0xRRGGBB; the default is white over black. Premultiplied, like every mark the VR copy
    // pass draws (ps_straight divides it out again).
    static bool MakeDotTexture(std::uint32_t coreRgb, std::uint32_t rimRgb, ID3D11Texture2D*& tex,
                               ID3D11ShaderResourceView*& srv)
    {
        std::uint32_t px[kDotSize * kDotSize];
        const float c = (kDotSize - 1) * 0.5f, rCore = 9.0f, rRim = 12.0f;
        const auto chan = [](std::uint32_t rgb, int shift) { return static_cast<float>((rgb >> shift) & 0xFF); };
        for (int y = 0; y < kDotSize; ++y) {
            for (int x = 0; x < kDotSize; ++x) {
                const float d = std::sqrt((x - c) * (x - c) + (y - c) * (y - c));
                float a = std::clamp(rRim + 0.5f - d, 0.0f, 1.0f);          // outer edge, antialiased
                const float core = std::clamp(rCore + 0.5f - d, 0.0f, 1.0f);  // the core over the rim (a is 1 here)
                const std::uint32_t A = static_cast<std::uint32_t>(a * 255.0f + 0.5f);
                std::uint32_t rgb = 0;
                for (int shift = 16; shift >= 0; shift -= 8) {
                    const float v = chan(rimRgb, shift) + (chan(coreRgb, shift) - chan(rimRgb, shift)) * core;
                    rgb |= static_cast<std::uint32_t>(v * a + 0.5f) << shift;
                }
                px[y * kDotSize + x] = (A << 24) | rgb;
            }
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kDotSize; td.Height = kDotSize;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sub{ px, kDotSize * sizeof(std::uint32_t), 0 };
        if (FAILED(s_device->CreateTexture2D(&td, &sub, &tex))) return false;
        if (FAILED(s_device->CreateShaderResourceView(tex, nullptr, &srv))) {
            tex->Release();
            tex = nullptr;
            return false;
        }
        return true;
    }
    static bool CreateDotTexture() { return MakeDotTexture(0xFFFFFF, 0x000000, s_dotTex, s_dotSrv); }

    // Decode an image file (any WIC-readable format) into premultiplied BGRA rows of at most maxPx x maxPx. No
    // device work, so any thread with COM initialized may call it.
    static bool DecodeImagePixels(const std::filesystem::path& path, UINT maxPx, std::vector<std::uint8_t>& px,
                                  int& outW, int& outH, std::string* why)
    {
        using Microsoft::WRL::ComPtr;
        const auto fail = [why](const std::string& w) { if (why) *why = w; return false; };
        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))))
            return fail("the image decoder is unavailable");
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnLoad, &decoder)))
            return fail("not an image Windows can read");
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) return fail("no image frame");
        ComPtr<IWICFormatConverter> conv;
        if (FAILED(factory->CreateFormatConverter(&conv))) return fail("the image decoder is unavailable");
        if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom)))
            return fail("the pixel format cannot be converted");
        UINT w = 0, h = 0;
        if (FAILED(conv->GetSize(&w, &h)) || !w || !h) return fail("the image is empty");
        if (w > maxPx || h > maxPx)
            return fail("larger than " + std::to_string(maxPx) + "x" + std::to_string(maxPx) + " (" + std::to_string(w) +
                        "x" + std::to_string(h) + ")");
        px.assign(static_cast<size_t>(w) * h * 4, 0);
        if (FAILED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data())))
            return fail("the pixels could not be read");
        outW = static_cast<int>(w);
        outH = static_cast<int>(h);
        return true;
    }

    // Premultiplied BGRA rows into an immutable texture: the encoding the baked arrow and every Ultralight surface
    // use, so the compositor's ONE/INV_SRC_ALPHA blend applies unchanged. Render thread (device work).
    static bool UploadPixels(const std::vector<std::uint8_t>& px, int w, int h,
                             ID3D11Texture2D** outTex, ID3D11ShaderResourceView** outSrv)
    {
        using Microsoft::WRL::ComPtr;
        if (!s_device || w <= 0 || h <= 0 || px.size() < static_cast<size_t>(w) * h * 4) return false;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(w); td.Height = static_cast<UINT>(h);
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sub{ px.data(), static_cast<UINT>(w) * 4, 0 };
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(s_device->CreateTexture2D(&td, &sub, &tex))) return false;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(s_device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return false;
        *outTex = tex.Detach();
        *outSrv = srv.Detach();
        return true;
    }

    // The player's cursorFile: decoded and uploaded at device init. Capped at 512² (a cursor, not a wallpaper).
    static bool LoadCursorImage(const std::filesystem::path& path,
                                ID3D11Texture2D** outTex, ID3D11ShaderResourceView** outSrv,
                                int& outW, int& outH)
    {
        // The game already initialized COM on this (main) thread; S_FALSE /
        // RPC_E_CHANGED_MODE are both fine and we never uninitialize.
        (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<std::uint8_t> px;
        int w = 0, h = 0;
        if (!DecodeImagePixels(path, 512, px, w, h, nullptr)) return false;
        if (!UploadPixels(px, w, h, outTex, outSrv)) return false;
        outW = w;
        outH = h;
        return true;
    }

    // "cursorFile": an image replaces the drawn cursor when it loads.
    static void TryLoadCustomCursor()
    {
        if (s_cursorFile.empty()) return;
        const bool absolute = s_cursorFile.size() > 1 && s_cursorFile[1] == ':';
        const std::filesystem::path p = absolute
            ? PathText::FromUtf8(s_cursorFile)
            : (s_runtimeDir / PathText::FromUtf8(s_cursorFile).make_preferred());
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) {
            SKSE::log::info("Magelight: no cursor art at {} — the drawn cursor stays", Magelight::SysInfo::ForLog(p));
            return;
        }
        ID3D11Texture2D* tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0, h = 0;
        if (!LoadCursorImage(p, &tex, &srv, w, h)) {
            SKSE::log::warn("Magelight: cursor art {} failed to load — the drawn cursor stays", Magelight::SysInfo::ForLog(p));
            return;
        }
        if (s_cursorSrv) s_cursorSrv->Release();
        if (s_cursorTex) s_cursorTex->Release();
        s_cursorTex = tex;
        s_cursorSrv = srv;
        s_cursorImgW = w;
        s_cursorImgH = h;
        s_cursorCustom = true;
        SKSE::log::info("Magelight: cursor art {} ({}x{}, hotspot {:.2f},{:.2f}, height {}px@1080p)",
            SysInfo::ForLog(p), w, h, s_cursorHotX, s_cursorHotY, s_cursorHeight);
    }

    // ── Per-view cursors (0.31.0) ───────────────────────────────────────────
    // A mod's own cursor images (Magelight.h). An image decodes on a thread-pool thread the first time the compositing
    // thread asks for it, uploads there the next time it is asked for, and lives as long as a view
    // (or a mod default) holds it. Never decoded inside Present: a mod may set its cursor from a JS listener, which
    // runs there. Until an image is ready, and if it fails, the host cursor shows.
    static constexpr UINT kViewCursorMaxPx = 256;

    struct CursorImage {
        enum : int { kIdle, kDecoding, kDecoded, kReady, kFailed };
        std::filesystem::path file;
        float hotX = 0, hotY = 0;   // image pixels
        float height = 0;           // px at 1080p; 0 = the image's own height
        bool press = false;
        std::atomic<int> state{ kIdle };
        // Written by the decode thread before it stores kDecoded; read by the render thread once it sees it.
        std::vector<std::uint8_t> px;
        int w = 0, h = 0;
        // Render thread from kReady on. Released with the last reference, on whichever thread drops it: D3D11
        // Release is free-threaded, and a quad deferred to the UI pass holds its own reference.
        ID3D11Texture2D* tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        ~CursorImage()
        {
            if (srv) srv->Release();
            if (tex) tex->Release();
        }
    };

    static constexpr const char* kCursorStateNames[kCursorStates] = { "arrow", "pointer", "text" };
    static std::mutex s_cursorImagesMutex;
    static std::map<std::string, std::weak_ptr<CursorImage>> s_cursorImages;   // by file, hotspot, height, press

    std::shared_ptr<CursorImage> MakeCursorImage(const std::filesystem::path& file, float hotX, float hotY,
                                                 float height, bool press, std::string* why)
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
            if (why) *why = "no file at " + SysInfo::ForLog(file);
            return nullptr;
        }
        const auto finite = [](float v) { return v == v && v > -1e30f && v < 1e30f; };
        hotX = finite(hotX) ? std::clamp(hotX, 0.0f, static_cast<float>(kViewCursorMaxPx)) : 0.0f;
        hotY = finite(hotY) ? std::clamp(hotY, 0.0f, static_cast<float>(kViewCursorMaxPx)) : 0.0f;
        height = (finite(height) && height > 0.0f) ? std::clamp(height, 8.0f, 256.0f) : 0.0f;
        const std::filesystem::path norm = file.lexically_normal();
        std::string key = PathText::ToUtf8(norm);
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        key += "|" + std::to_string(hotX) + "|" + std::to_string(hotY) + "|" + std::to_string(height) + (press ? "|p" : "|-");
        std::lock_guard<std::mutex> lk(s_cursorImagesMutex);
        if (auto it = s_cursorImages.find(key); it != s_cursorImages.end()) {
            if (auto live = it->second.lock()) return live;
        }
        for (auto it = s_cursorImages.begin(); it != s_cursorImages.end();)
            it = it->second.expired() ? s_cursorImages.erase(it) : std::next(it);
        auto img = std::make_shared<CursorImage>();
        img->file = norm;
        img->hotX = hotX;
        img->hotY = hotY;
        img->height = height;
        img->press = press;
        s_cursorImages[key] = img;
        return img;
    }

    // Thread pool. ctx is a heap shared_ptr that keeps the image alive for the decode.
    static void CALLBACK DecodeCursorImage(PTP_CALLBACK_INSTANCE, void* ctx)
    {
        const std::unique_ptr<std::shared_ptr<CursorImage>> hold(static_cast<std::shared_ptr<CursorImage>*>(ctx));
        CursorImage& img = **hold;
        std::string why;
        bool ok = false;
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            ok = DecodeImagePixels(img.file, kViewCursorMaxPx, img.px, img.w, img.h, &why);
        } catch (...) {
            why = "out of memory";
        }
        if (SUCCEEDED(co)) CoUninitialize();
        if (!ok) {
            std::vector<std::uint8_t>().swap(img.px);
            SKSE::log::warn("Magelight: cursor image {} - {}; the host cursor shows instead", Magelight::SysInfo::ForLog(img.file), why);
            img.state.store(CursorImage::kFailed);
            return;
        }
        img.state.store(CursorImage::kDecoded, std::memory_order_release);
    }

    // Render thread. The image's texture; nullptr while it decodes or after it failed (the host cursor shows).
    static ID3D11ShaderResourceView* CursorImageSrv(const std::shared_ptr<CursorImage>& img)
    {
        const int st = img->state.load(std::memory_order_acquire);
        if (st == CursorImage::kReady) return img->srv;
        if (st == CursorImage::kIdle) {
            img->state.store(CursorImage::kDecoding);   // only this thread leaves kIdle
            auto* hold = new std::shared_ptr<CursorImage>(img);
            if (!TrySubmitThreadpoolCallback(&DecodeCursorImage, hold, nullptr)) {
                delete hold;
                SKSE::log::warn("Magelight: cursor image {} - could not queue its decode; the host cursor shows instead",
                                SysInfo::ForLog(img->file));
                img->state.store(CursorImage::kFailed);
            }
            return nullptr;
        }
        if (st != CursorImage::kDecoded) return nullptr;
        const bool ok = UploadPixels(img->px, img->w, img->h, &img->tex, &img->srv);
        std::vector<std::uint8_t>().swap(img->px);
        if (!ok) {
            SKSE::log::warn("Magelight: cursor image {} - texture creation failed; the host cursor shows instead",
                            SysInfo::ForLog(img->file));
            img->state.store(CursorImage::kFailed);
            return nullptr;
        }
        SKSE::log::info("Magelight: cursor image {} ({}x{}, hotspot {:.0f},{:.0f}, {}{})", Magelight::SysInfo::ForLog(img->file), img->w,
                        img->h, img->hotX, img->hotY,
                        img->height > 0 ? std::to_string(static_cast<int>(img->height)) + "px at 1080p" : "own height",
                        img->press ? ", press shrink" : "");
        img->state.store(CursorImage::kReady);
        return img->srv;
    }

    static std::string DescribeCursorSet(const CursorSet& set)
    {
        if (set.none) return "none (the page draws its own)";
        std::string out;
        for (int i = 0; i < kCursorStates; ++i) {
            if (!set.image[i]) continue;
            if (!out.empty()) out += ", ";
            out += std::string(kCursorStateNames[i]) + "=" + SysInfo::ForLog(set.image[i]->file);
        }
        return out.empty() ? "the host cursor" : out;
    }

    bool ResolveViewFile(ViewId view, const std::string& path, bool absoluteOk, std::filesystem::path& out,
                         std::string* why)
    {
        const auto fail = [why](const char* w) { if (why) *why = w; return false; };
        if (path.empty() || path.size() > 260) return fail("the path is empty or longer than 260 characters");
        const bool absolute = path.size() > 1 && path[1] == ':';
        if (absolute) {
            if (!absoluteOk) return fail("the path must be relative to the mod folder");
            out = PathText::FromUtf8(path).lexically_normal();
            return true;
        }
        if (path[0] == '/' || path[0] == '\\' || path.find("..") != std::string::npos)
            return fail("the path must stay inside the mod folder (relative, no '..')");
        std::filesystem::path root;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            const MlView* v = FindViewLocked(view);
            if (!v) return fail("the view is gone");
            root = v->root;
        }
        if (root.empty()) root = s_runtimeDir;
        out = (root / PathText::FromUtf8(path).make_preferred()).lexically_normal();
        return true;
    }

    bool SetViewCursorSet(ViewId view, bool modDefault, const CursorSet& set)
    {
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(view);
            if (!v || v->destroyPending) return false;
            (modDefault ? v->cursorMod : v->cursorOwn) = set;
        }
        SKSE::log::info("Magelight: view {} {}cursor: {}", view, modDefault ? "mod default " : "", DescribeCursorSet(set));
        return true;
    }

    bool SetViewCursorState(ViewId view, int state, std::shared_ptr<CursorImage> image)
    {
        if (state < 0 || state >= kCursorStates) return false;
        std::string desc;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(view);
            if (!v || v->destroyPending) return false;
            if (image || state == 0) v->cursorOwn.none = false;   // "none" is set through the arrow state
            v->cursorOwn.image[state] = std::move(image);
            desc = DescribeCursorSet(v->cursorOwn);
        }
        SKSE::log::info("Magelight: view {} cursor: {}", view, desc);
        return true;
    }

    bool SetViewCursorNone(ViewId view)
    {
        CursorSet none;
        none.none = true;
        return SetViewCursorSet(view, false, none);
    }

    // The log lines of these two setters are built from the stored value inside the critical section: a page writes
    // the same slots (OnPageHostTheme), and a line printed from the argument could name a value another writer replaced.
    bool SetViewCursorTint(ViewId view, const CursorTintSet& tint)
    {
        std::string desc;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(view);
            if (!v || v->destroyPending) return false;
            v->cursorTint = tint;
            desc = DescribeCursorTint(v->cursorTint);
        }
        SKSE::log::info("Magelight: view {} cursor tint: {}", view, desc);
        return true;
    }

    bool SetViewKeyboardTheme(ViewId view, const HostTheme::KbTheme* theme)
    {
        if (!view || view == static_cast<ViewId>(s_kbViewId.load())) return false;
        const std::optional<HostTheme::KbTheme> next = theme ? std::optional(*theme) : std::nullopt;
        std::string desc;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(view);
            if (!v || v->destroyPending) return false;
            if (v->kbTheme == next) return true;   // unchanged: no message, no line
            v->kbTheme = next;
            if (view == static_cast<ViewId>(s_uiModeView.load())) QueueKeyboardThemeLocked();
            desc = v->kbTheme ? HostTheme::KeyboardJson(*v->kbTheme) : "none (the host's look)";
        }
        SKSE::log::info("Magelight: view {} keyboard theme (API): {}", view, desc);
        return true;
    }

    // What the flat cursor draws at the pointer.
    struct CursorPick {
        PageCursor kind = PageCursor::Arrow;   // for the host cursor (None and Custom read as Arrow)
        bool hide = false;                     // the page under the pointer draws its own
        std::shared_ptr<CursorImage> image;    // the view's image for the state; null = the host cursor
        CursorTintSet tint;                    // the drawn cursor's colours (0.31.1); empty = the host's
    };

    // The page at (x, y): the topmost visible interactive view there, else the UI-mode view (DrainInputQueue's hit
    // order, so it is the page the mouse events go to while no button is held). Hiding needs the pointer INSIDE that
    // page: outside every view the host cursor always draws, so a stale "none" never leaves the player without a
    // pointer. Precedence: cursorForce > the view's own set > its mod's default > the host cursor; modCursors false
    // drops the images and the tint.
    static CursorPick PickCursorAtLocked(int x, int y, float bw, float bh)
    {
        CursorPick pick;
        const MlView* view = nullptr;
        bool inside = false;
        for (auto it = s_views.rbegin(); it != s_views.rend(); ++it) {
            const MlView& v = **it;
            if (!v.ul || !v.visible || v.clickThrough || !ViewShows(v)) continue;
            float ex = 0, ey = 0;
            EffectivePos(v, bw, bh, ex, ey);
            if (x >= ex && y >= ey && x < ex + v.w && y < ey + v.h) { view = &v; inside = true; break; }
        }
        if (!view) view = FindViewLocked(static_cast<ViewId>(s_uiModeView.load()));
        if (!view) return pick;
        const PageCursor kind = static_cast<PageCursor>(view->pageCursor);
        pick.kind = (kind == PageCursor::None || kind == PageCursor::Custom) ? PageCursor::Arrow : kind;
        if (s_cursorForce.load()) return pick;
        const CursorSet& set = view->cursorOwn.Empty() ? view->cursorMod : view->cursorOwn;
        if (inside && (kind == PageCursor::None || set.none)) {
            pick.hide = true;
            return pick;
        }
        if (!s_modCursors.load()) return pick;
        pick.tint = view->cursorTint;
        if (set.none) return pick;
        const int state = kind == PageCursor::Clickable ? 1 : kind == PageCursor::Text ? 2 : 0;
        pick.image = set.image[state];
        if (!pick.image && state == 1) pick.image = set.image[0];   // text keeps the host I-beam: the caret hint matters
        return pick;
    }

    // Game thread. With the page thread, its registry walks can hold the lock across Ultralight work (a page's load or
    // resize, MaterializeViews and ApplyPendingResizes, or the views it collects): the cursor keeps the last frame's
    // pick for this frame rather than wait (invariant 27). The pick owns its image (shared_ptr), so a view freed meanwhile takes nothing from under it.
    static CursorPick PickCursorAt(int x, int y, float bw, float bh)
    {
        if (!PageThreadOn()) {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            return PickCursorAtLocked(x, y, bw, bh);
        }
        static CursorPick* const s_last = new CursorPick();   // never freed: no D3D release at exit
        std::unique_lock<PageThread::OwnedMutex> lk(s_viewsMutex, std::try_to_lock);
        if (lk.owns_lock()) *s_last = PickCursorAtLocked(x, y, bw, bh);
        return *s_last;
    }

    // VR laser end with vr.cursorDot false: the view's image for its page state, never "none" (the laser has to
    // show where it points). Text falls back to the arrow image: VR has no I-beam art. Any thread.
    static std::shared_ptr<CursorImage> VrCursorImageFor(ViewId id)
    {
        if (s_cursorForce.load() || !s_modCursors.load()) return nullptr;
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        const MlView* v = FindViewLocked(id);
        if (!v) return nullptr;
        const CursorSet& set = v->cursorOwn.Empty() ? v->cursorMod : v->cursorOwn;
        const PageCursor kind = static_cast<PageCursor>(v->pageCursor);
        const int state = kind == PageCursor::Clickable ? 1 : kind == PageCursor::Text ? 2 : 0;
        return set.image[state] ? set.image[state] : set.image[0];
    }

    // The VR laser dot's tint for a view (0.31.1): empty under "cursorForce" or "modCursors": false. Any thread.
    static CursorTintSet VrCursorTintFor(ViewId id)
    {
        if (s_cursorForce.load() || !s_modCursors.load()) return {};
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        const MlView* v = FindViewLocked(id);
        return v ? v->cursorTint : CursorTintSet{};
    }

    // ── The drawn cursor (MagelightCursorArt.h) ─────────────────────────────
    // Built on the compositing (game) thread at the pixel height it is drawn at, and rebuilt when that height changes (a
    // resolution change). kArtGlowSteps arrow textures share one size and hotspot: the hover glow fades by stepping
    // through them, with no blend state of its own.
    static constexpr int kArtGlowSteps = 4;
    struct CursorArtTex
    {
        ID3D11Texture2D*          tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0, h = 0;
        float hotX = 0, hotY = 0;
    };
    // One set per tint in use (the host's own colours are one), at most kArtSets: the set drawn least recently gives
    // its slot to a new tint. A height change (a resolution change) rebuilds a set when it is next drawn.
    struct CursorArtSet
    {
        std::uint64_t key = 0;               // TintKey; 0 = the host's colours
        int height = 0;                      // the pixel height the textures were built for; 0 = an empty slot
        std::uint64_t used = 0;              // s_artStamp at the last draw
        CursorArtTex arrow[kArtGlowSteps];
        CursorArtTex ibeam;
    };
    static constexpr int kArtSets = 4;
    static CursorArtSet  s_artSets[kArtSets];
    static std::uint64_t s_artStamp = 0;
    static bool          s_artFailed = false; // a texture creation failed: the baked arrow draws instead

    // The tint's identity: 0 for the host's colours, else a hash of the colours it sets (alpha 0 = unset).
    static std::uint64_t TintKey(const CursorTintSet& t)
    {
        if (t.Empty()) return 0;
        std::uint64_t h = 1469598103934665603ull;
        for (std::uint32_t c : { t.lit, t.shade, t.ink, t.glow, t.ibeam }) {
            const std::uint32_t v = (c & 0xFF000000u) ? (c | 0xFF000000u) : 0u;
            for (int i = 0; i < 4; ++i) {
                h ^= (v >> (8 * i)) & 0xFF;
                h *= 1099511628211ull;
            }
        }
        return h ? h : 1;
    }

    static CursorArt::Palette PaletteFor(const CursorTintSet& t)
    {
        CursorArt::Palette p;
        const auto apply = [](std::uint32_t argb, CursorArt::Rgb& c) {
            if (!(argb & 0xFF000000u)) return;
            c = { ((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f };
        };
        apply(t.lit, p.lit);
        apply(t.shade, p.shade);
        apply(t.ink, p.ink);
        apply(t.glow, p.glow);
        apply(t.ibeam, p.ibeam);
        return p;
    }

    static bool UploadCursorArt(const CursorArt::Image& img, CursorArtTex& out)
    {
        if (out.srv) { out.srv->Release(); out.srv = nullptr; }
        if (out.tex) { out.tex->Release(); out.tex = nullptr; }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(img.w); td.Height = static_cast<UINT>(img.h);
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sub{ img.px.data(), static_cast<UINT>(img.w) * sizeof(std::uint32_t), 0 };
        if (FAILED(s_device->CreateTexture2D(&td, &sub, &out.tex))) return false;
        if (FAILED(s_device->CreateShaderResourceView(out.tex, nullptr, &out.srv))) {
            out.tex->Release();
            out.tex = nullptr;
            return false;
        }
        out.w = img.w; out.h = img.h; out.hotX = img.hotX; out.hotY = img.hotY;
        return true;
    }

    // Render thread. The drawn cursor in `tint`'s colours at `height` pixels; nullptr when it cannot be built.
    static const CursorArtSet* EnsureCursorArt(int height, const CursorTintSet& tint)
    {
        if (s_artFailed || !s_device) return nullptr;
        const std::uint64_t key = TintKey(tint);
        CursorArtSet* slot = nullptr;
        for (auto& set : s_artSets) {
            if (set.height != 0 && set.key == key) { slot = &set; break; }
        }
        if (!slot) {
            slot = &s_artSets[0];
            for (auto& set : s_artSets) {
                if (set.height == 0) { slot = &set; break; }
                if (set.used < slot->used) slot = &set;
            }
            slot->height = 0;   // rebuilt below: UploadCursorArt releases the old textures
        }
        slot->used = ++s_artStamp;
        if (slot->height == height) return slot;
        const CursorArt::Palette pal = PaletteFor(tint);
        bool ok = UploadCursorArt(CursorArt::IBeam(height, pal), slot->ibeam);
        for (int i = 0; ok && i < kArtGlowSteps; ++i)
            ok = UploadCursorArt(CursorArt::Arrow(height, static_cast<float>(i) / (kArtGlowSteps - 1), pal), slot->arrow[i]);
        if (!ok) {
            s_artFailed = true;
            SKSE::log::error("Magelight: drawn cursor - texture creation failed at {}px; the baked arrow draws instead", height);
            return nullptr;
        }
        slot->key = key;
        slot->height = height;
        SKSE::log::info("Magelight: drawn cursor built at {}px{}", height, key ? " (tinted)" : "");
        return slot;
    }

    // Render thread. The VR laser dot in a tint's colours (lit core over an ink rim); the default dot for no tint.
    // Up to kDotSets tinted dots are kept, the least recently used replaced; the default dot when one cannot be made
    // (and for the rest of the session after a creation failure).
    static constexpr int kDotSets = 4;
    struct DotTex { std::uint64_t key = 0; std::uint64_t used = 0; ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr; };
    static DotTex s_dotSets[kDotSets];
    static bool   s_dotTintFailed = false;
    static ID3D11ShaderResourceView* DotSrvFor(const CursorTintSet& tint)
    {
        const bool core = (tint.lit & 0xFF000000u) != 0, rim = (tint.ink & 0xFF000000u) != 0;
        if ((!core && !rim) || s_dotTintFailed) return s_dotSrv;
        const std::uint32_t coreRgb = core ? (tint.lit & 0xFFFFFFu) : 0xFFFFFFu;
        const std::uint32_t rimRgb = rim ? (tint.ink & 0xFFFFFFu) : 0x000000u;
        const std::uint64_t key = (static_cast<std::uint64_t>(coreRgb) << 32) | rimRgb | (1ull << 63);
        DotTex* slot = nullptr;
        for (auto& d : s_dotSets) {
            if (d.srv && d.key == key) { d.used = ++s_artStamp; return d.srv; }
        }
        slot = &s_dotSets[0];
        for (auto& d : s_dotSets) {
            if (!d.srv) { slot = &d; break; }
            if (d.used < slot->used) slot = &d;
        }
        if (slot->srv) { slot->srv->Release(); slot->srv = nullptr; }
        if (slot->tex) { slot->tex->Release(); slot->tex = nullptr; }
        if (!MakeDotTexture(coreRgb, rimRgb, slot->tex, slot->srv)) {
            s_dotTintFailed = true;
            SKSE::log::error("Magelight: tinted VR dot - texture creation failed; the default dot draws instead");
            return s_dotSrv;
        }
        slot->key = key;
        slot->used = ++s_artStamp;
        return slot->srv;
    }

    // The device the game renders with (BSGraphics renderer data), for a swapchain that will not hand one out.
    static ID3D11Device* EngineDevice()
    {
        auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
        return data ? reinterpret_cast<ID3D11Device*>(data->forwarder) : nullptr;
    }

    // Logs a failed device-resource step and records it as the render-death reason.
    static bool DeviceStepFailed(const char* step, HRESULT hr)
    {
        char detail[64];
        std::snprintf(detail, sizeof(detail), "HRESULT 0x%08X", static_cast<unsigned>(hr));
        SKSE::log::error("Magelight: {} failed (0x{:08X})", step, static_cast<unsigned>(hr));
        NoteRenderFailure(step, detail);
        return false;
    }

    // Made once: a refused page thread (StartPageThread) brings render init back a frame later, on the page fiber.
    static bool s_deviceResourcesReady = false;

    static bool CreateDeviceResources(IDXGISwapChain* sc)
    {
        if (s_deviceResourcesReady) return true;
        // The swapchain first: on every setup that works it answers, and the device it gives is the one its
        // back buffer lives on. The engine's device only stands in when it refuses (a Streamline wrapper over a
        // Direct3D 12 frame-generation swapchain answers E_NOINTERFACE). Never Released: a wrapper may not AddRef.
        ID3D11Device* dev = nullptr;
        HRESULT devHr = E_NOINTERFACE;
        if (s_engineMode && EngineDevice()) {
            dev = EngineDevice();
            dev->AddRef();
            devHr = S_OK;
            s_deviceFromEngine.store(true);
        } else {
            devHr = sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev));
        }
        if (SUCCEEDED(devHr) && dev) {
            s_device = dev;
        } else if (ID3D11Device* engine = EngineDevice()) {
            engine->AddRef();
            s_device = engine;
            s_deviceFromEngine.store(true);
            s_untrustedChain.store(true);
            s_lateComposite.store(false);
            SKSE::log::warn("Magelight: the game swapchain refused its device (0x{:08X}) - using the engine's device",
                static_cast<unsigned>(devHr));
        } else {
            return DeviceStepFailed("the game swapchain refused its device and the engine has none", devHr);
        }
        s_device->GetImmediateContext(&s_context);
        SKSE::log::info("Magelight: device from the {}{}", s_deviceFromEngine.load() ? "engine" : "swapchain",
            SmoothMotionLoaded() ? "; NVIDIA Smooth Motion (NvPresent64.dll) is loaded" : "");

        ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
        if (FAILED(D3DCompile(kShaderSrc, std::strlen(kShaderSrc), nullptr, nullptr, nullptr,
                "vs_main", "vs_4_0", 0, 0, &vsb, &err))) {
            SKSE::log::error("Magelight: VS compile failed: {}",
                err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            if (err) err->Release();
            NoteRenderFailure("the vertex shader did not compile");
            return false;
        }
        if (FAILED(D3DCompile(kShaderSrc, std::strlen(kShaderSrc), nullptr, nullptr, nullptr,
                "ps_main", "ps_4_0", 0, 0, &psb, &err))) {
            SKSE::log::error("Magelight: PS compile failed: {}",
                err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            if (err) err->Release();
            vsb->Release();
            NoteRenderFailure("the pixel shader did not compile");
            return false;
        }
        HRESULT hr = s_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &s_vs);
        bool ok = SUCCEEDED(hr) || DeviceStepFailed("CreateVertexShader", hr);
        if (ok) {
            hr = s_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &s_ps);
            ok = SUCCEEDED(hr) || DeviceStepFailed("CreatePixelShader", hr);
        }
        // The straight-alpha variant is only needed by the VR presenter; a
        // compile failure here degrades to the premultiplied copy, never to
        // a dead overlay.
        {
            ID3DBlob *sb = nullptr, *serr = nullptr;
            if (SUCCEEDED(D3DCompile(kShaderSrc, std::strlen(kShaderSrc), nullptr, nullptr, nullptr,
                    "ps_straight", "ps_4_0", 0, 0, &sb, &serr))) {
                s_device->CreatePixelShader(sb->GetBufferPointer(), sb->GetBufferSize(), nullptr, &s_psStraight);
                sb->Release();
            } else {
                SKSE::log::warn("Magelight: ps_straight compile failed: {}",
                    serr ? static_cast<const char*>(serr->GetBufferPointer()) : "?");
            }
            if (serr) serr->Release();
        }
        if (ok) {
            const D3D11_INPUT_ELEMENT_DESC il[] = {
                { "POS", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEX", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            hr = s_device->CreateInputLayout(il, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &s_layout);
            ok = SUCCEEDED(hr) || DeviceStepFailed("CreateInputLayout", hr);
        }
        vsb->Release();
        psb->Release();
        if (!ok) return false;

        // CPU-surface textures are per-view now, created lazily on first
        // upload (see EnsureCpuTexture).
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(Vertex) * 4;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(hr = s_device->CreateBuffer(&bd, nullptr, &s_vb))) return DeviceStepFailed("CreateBuffer (vertices)", hr);
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = 16;
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(hr = s_device->CreateBuffer(&cbd, nullptr, &s_cb))) return DeviceStepFailed("CreateBuffer (constants)", hr);

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        if (FAILED(hr = s_device->CreateSamplerState(&sd, &s_sampler))) return DeviceStepFailed("CreateSamplerState", hr);

        // Ultralight surfaces are premultiplied BGRA: ONE / INV_SRC_ALPHA.
        D3D11_BLEND_DESC bld{};
        bld.RenderTarget[0].BlendEnable = TRUE;
        bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(hr = s_device->CreateBlendState(&bld, &s_blend))) return DeviceStepFailed("CreateBlendState", hr);
        // Straight-alpha 'over' for compositing INTO the VR copy target (whose
        // pixels are straight after ps_straight): rgb = src*a + dst*(1-a).
        {
            D3D11_BLEND_DESC sb = bld;
            sb.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            sb.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            if (FAILED(s_device->CreateBlendState(&sb, &s_blendStraight))) s_blendStraight = nullptr;
        }

        if (!CreateCursorTexture()) {
            NoteRenderFailure("the cursor texture could not be created");
            return false;
        }
        if (!CreateDotTexture()) SKSE::log::warn("Magelight: VR pointer dot texture failed — the arrow art stands in");
        TryLoadCustomCursor();  // optional; keeps the drawn cursor on any failure
        s_deviceResourcesReady = true;
        return true;
    }

    static void FontLog(int level, const char* message)
    {
        if (level) SKSE::log::warn("Magelight: {}", message);
        else SKSE::log::info("Magelight: {}", message);
    }

    // ViewConfig::enable_compositor for every View: on, the 2.0 default (1.4's was off). With it off, 2.0 paints a
    // device scale below 1 only in the page's top-left scale-squared part, and needs_paint() stays true on an idle
    // page, so PresentedView::repainted would make the VR presenter upload every frame. On costs the tiles of the
    // layers a page composites (the memory line's "page tiles").
    static constexpr bool kEnableCompositor = true;

    // ── What keeps a page error from ending the overlay (PageStackCore.h, invariant 1) ──
    // On the page fiber, the page VM's lock whenever the thread is off it (UlFiberMain); on the game's stack,
    // JavaScriptCore's stack zones sized past the thread's stack guarantee (ApplyGameStackReserve). Main thread only.
    static PageStack::Exports s_pageStackExports;
    static bool               s_pageStackResolved = false;
    static bool               s_pagesOnFiber = false;           // decided by EnsureUlFiber before the renderer exists
    static std::size_t        s_reserveGuarantee = SIZE_MAX;    // the guarantee the zones were sized for; SIZE_MAX = none

    static const PageStack::Exports& PageStackExports()
    {
        if (!s_pageStackResolved) {
            s_pageStackResolved = true;
            s_pageStackExports = PageStack::Resolve(GetModuleHandleW(L"MgWCore.dll"));
        }
        return s_pageStackExports;
    }

    // Before Renderer::Create: the VM reads the zones when WebKit makes it.
    static void ApplyGameStackReserve()
    {
        ULONG held = 0;   // 0 reads the thread's guarantee without changing it
        const bool known = SetThreadStackGuarantee(&held) != 0;
        // Unknown: assume CrashLogger's 64 KB, the most any plugin measured sets on this thread.
        const PageStack::Reserve r = PageStack::ReserveFor(known ? held : 64 * 1024);
        const PageStack::Exports& e = PageStackExports();
        if (!e.OptionsReady()) {
            SKSE::log::error("Magelight: this Ultralight runtime does not export {} - pages on the game's stack keep "
                             "JavaScriptCore's own stack zones, so with a stack guarantee on it (CrashLogger 1.23 and "
                             "later sets one) the first error a page leaves uncaught ends the overlay",
                             e.missingOptions);
            return;
        }
        char refused[96];
        if (!PageStack::ApplyReserve(e, r, refused, sizeof refused))
            SKSE::log::error("Magelight: JavaScriptCore refused the stack option {}", refused);
        s_reserveGuarantee = r.guarantee;
        SKSE::log::info("Magelight: JavaScript stack zones for pages on the game's stack, past a {} KB stack "
                        "guarantee{}: a page error commits the stack no lower than {} KB above its end, scripts stop at "
                        "least {} KB above it (reservedZoneSize {}, softReservedZoneSize {}, maxPerThreadStackUsage {})",
                        r.guarantee / 1024, known ? "" : " (assumed: the guarantee could not be read)", r.hard / 1024,
                        r.soft / 1024, r.hard, r.soft, r.maxUsage);
    }

    // `ctx` is what the GPU driver draws through: the game's immediate context, or with pageThread the page thread's
    // deferred one (MgGpu_Create, contract 3). Ultralight's thread: whichever thread calls this owns WebKit from here on.
    static bool CreateUltralight(ID3D11DeviceContext* ctx)
    {
        ultralight::Config cfg;
        // resource_path_prefix defaults to "resources/" resolved against the
        // platform file system's base dir — our runtime dir carries the ICU
        // data + cacert there.
        //
        // cache_path: without one, WebKit writes localStorage and IndexedDB
        // under <cwd>\default\, the game root. SA's frontend stores real user
        // data there (margin notes, UI scale, pins), so park it beside the
        // SKSE logs — a writable, mod-manager-free location. Failure = empty
        // path = the game-root fallback, never fatal.
        if (const auto cache = CacheDirPath(); !cache.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(cache, ec);
            if (!ec) {
                const auto u8 = cache.u8string();
                cfg.cache_path = ultralight::String(reinterpret_cast<const char*>(u8.c_str()));
            }
        }
        cfg.font_hinting = s_fontHinting == "smooth"     ? ultralight::FontHinting::Smooth
                         : s_fontHinting == "monochrome" ? ultralight::FontHinting::Monochrome
                         : s_fontHinting == "none"       ? ultralight::FontHinting::None
                                                         : ultralight::FontHinting::Normal;
        if (s_fontGammaSet) cfg.font_gamma = s_fontGamma;
        SKSE::log::info("Magelight: font hinting '{}', gamma {}",
            s_fontHinting, s_fontGammaSet ? fmt::format("{:.2f}", s_fontGamma) : std::string("from the font profile"));
        // The edition caps every View's frame loop (requestAnimationFrame, CSS and Web animations, smooth scrolling,
        // painting) at ULTRALIGHT_EDITION_MAX_FPS (0 = none) whatever rate FrameWork pumps it; input, a resize and a
        // show still paint at once. ViewConfig::max_render_fps can only lower it.
        if constexpr (ULTRALIGHT_EDITION_MAX_FPS > 0)
            SKSE::log::info("Magelight: pages animate and repaint at most {} times a second (Ultralight edition cap)",
                ULTRALIGHT_EDITION_MAX_FPS);
        // The SDK's defaults, pinned so a later SDK changing them is a decision here. The host never calls
        // Renderer::Recycle(Full) or PurgeMemory: on Free both drop the compiled script of every live page (its next
        // run costs about as much as the first) and stall this thread, and the automatic reclaim (the recycler,
        // idle GC, the compositor's pool sweep) reaches the same footprint after a View's release: its script heap
        // and process memory within seconds, its video memory (tiles, textures, targets) within about 20 s (freed
        // inside Render, which FrameWork runs at least once a second, a view shown or not).
        // LowMemory saved nothing on SA's pages and slowed their script. Idle GC needs Update() every frame
        // (FrameWork pumps it).
        cfg.memory_profile = ultralight::MemoryProfile::Balanced;
        cfg.idle_gc_enabled = true;
        ultralight::Platform::instance().set_config(cfg);
        SKSE::log::info("Magelight: memory - profile balanced, idle GC after {:.1f} s without input, recycler every "
                        "{:.2f} s, resource cache {} MB, compositor {}; the host never calls Recycle or PurgeMemory",
                        cfg.idle_gc_idle_time, cfg.recycle_delay, cfg.memory_cache_size >> 20,
                        kEnableCompositor ? "on" : "off");
        // The SDK's Windows loader (compiled in, so no AppCore.dll) with a last resort that always loads
        // (MagelightFonts.h): with Arial missing or unreadable, the bare loader takes down the first page that needs
        // it (invariant 19). Built here, never at namespace scope: its base constructor is a delay-loaded runtime
        // import, which would run in DllMain before PreloadRuntime and fail the plugin's load (invariant 3). Never
        // freed, like the wrapper; stateless, so the renderer and Web Worker threads may share it.
        static auto* const platformFonts = new ultralight::platform::FontLoaderWin();
        ultralight::Platform::instance().set_font_loader(MagelightFonts::CreateLoader(platformFonts, &FontLog));
        // Our MlFileSystem, not AppCore's — see the class comment (.mjs, query
        // strings, page roots, the CSP).
        ultralight::Platform::instance().set_file_system(&s_fileSystem.value);
        ultralight::Platform::instance().set_logger(&s_ulLogger.value);
        ultralight::Platform::instance().set_clipboard(&s_clipboard.value);

        // Milestone 2: GPU rasterization. The driver must be registered BEFORE
        // Renderer::Create. Created against the GAME's device (fetched from
        // the swapchain in CreateDeviceResources) and `ctx`. Any failure
        // falls back to the CPU surface path.
        if (s_forceCpu.load()) {
            SKSE::log::info("Magelight: forceCpu set — GPU driver skipped, CPU rasterizer in use");
        } else if (s_gpuCreate && s_device && ctx) {
            s_gpu = s_gpuCreate(s_device, ctx, &GpuLogBridge);
            if (s_gpu) {
                if (auto* drv = static_cast<ultralight::GPUDriver*>(s_gpuGetDriver(s_gpu))) {
                    // The sample count applies to targets created from now on, so it is set before the renderer
                    // exists: no view (staggered or not) can have made one yet.
                    const int got = s_gpuSamples(s_gpu, s_msaa);
                    s_msaaInEffect = got > 1 ? got : 1;
                    // An offer: Ultralight 2.0 asks for an antialiased target only for content it does not draw
                    // analytically, and the memory lines say whether any did (TickMemoryReport).
                    if (got <= 1) SKSE::log::info("Magelight: MSAA off");
                    else if (got == s_msaa) SKSE::log::info("Magelight: MSAA {}x for the page targets Ultralight asks to antialias", got);
                    else SKSE::log::info("Magelight: MSAA {}x for the page targets Ultralight asks to antialias (Magelight.json asks {}x, which this graphics card cannot do for page targets)", got, s_msaa);
                    ultralight::Platform::instance().set_gpu_driver(drv);
                    s_gpuActive = true;
                }
            }
            if (!s_gpuActive) {
                SKSE::log::error("Magelight: GPU driver creation failed — CPU fallback");
            }
        }

        if (!s_pagesOnFiber && !PageThreadOn()) ApplyGameStackReserve();   // pages on the game's own stack only
        s_ulRenderer = ultralight::Renderer::Create();
        if (!s_ulRenderer) {
            SKSE::log::error("Magelight: the Ultralight renderer could not be created");
            NoteRenderFailure("the Ultralight renderer could not be created");
            return false;
        }
        SKSE::log::info("Magelight: renderer created ({} path)", s_gpuActive ? "GPU" : "CPU");
        return true;
    }

    // Absolute path → file:/// URL (PathText::ToFileUrl: UTF-8, percent-encoded to plain ASCII). Inverse of
    // PathText::PathOfFileUrl.
    static std::string PathToFileUrl(const std::filesystem::path& p) { return PathText::ToFileUrl(p); }

    // Render thread, per frame: give every registry entry that lacks one an
    // Ultralight view. Views registered from the game thread before the
    // renderer existed materialize here on the correct (owning) thread.
    // Render thread. Persistent Ultralight Sessions by name — one per mod
    // (localStorage/IndexedDB/cookies isolated under cache_path/<name>).
    static std::map<std::string, ultralight::RefPtr<ultralight::Session>> s_sessions;
    static ultralight::RefPtr<ultralight::Session> GetOrCreateSession(const std::string& name)
    {
        if (name.empty()) return nullptr;   // the renderer's default session
        auto it = s_sessions.find(name);
        if (it != s_sessions.end()) return it->second;
        auto session = s_ulRenderer->CreateSession(true, name.c_str());
        if (session) SKSE::log::info("Magelight: session '{}' created (persistent, isolated storage)", name);
        else SKSE::log::error("Magelight: session '{}' could not be created — falling back to default", name);
        s_sessions[name] = session;
        return session;
    }

    // A view's first load (or its wake from hibernation): create the View and start the page. s_viewsMutex held:
    // LoadURL runs OnBeginLoading synchronously inside it (invariant 16). False = CreateView failed.
    static bool LoadViewLocked(MlView& v)
    {
        const bool waking = v.dormant;
        v.dormant = false;
        v.domReady = false;
        v.docDomReady = false;
        v.hostLoad.pending = false;
        v.loadFailed = false;
        v.prepainted = false;
        v.paintableAt = 0;
        v.settleRepaint = false;
        v.settleRepainted = false;
        ultralight::ViewConfig vc;
        vc.is_accelerated = s_gpuActive;
        vc.is_transparent = true;
        vc.clipboard_read_policy = ultralight::ClipboardReadPolicy::Deny;
        vc.initial_device_scale = v.deviceScale;
        vc.enable_compositor = kEnableCompositor;
        // Only the UI-mode view has View focus (DrainInputQueue's focus marker), so a background page's autofocus
        // field never reports that it wants a keyboard or the IME. Unfocused, a page still animates and runs its
        // timers, and its focused field still takes keys (tools/editor-test).
        vc.initial_focus = false;
        v.pageCursor = 0;   // a new View has reported nothing yet
        v.edit = {};
        v.ul = s_ulRenderer->CreateView(
            static_cast<std::uint32_t>(v.w), static_cast<std::uint32_t>(v.h), vc,
            GetOrCreateSession(v.sessionName));
        if (!v.ul) {
            SKSE::log::error("Magelight: CreateView failed for view {}", v.id);
            return false;
        }
        // Created at w/h and deviceScale: a SetViewBounds or SetViewScale made while there was no View is applied.
        v.boundsDirty = false;
        v.scaleDirty = false;
        // A new View is visible. One that loads hidden (the load queue, a hidden rebuild) is hidden before it has a
        // page, so the page starts 'hidden' and paints nothing or runs no requestAnimationFrame until shown.
        // set_visible runs no page code and this View has no page yet, so the lock is fine here.
        if (!v.visible) v.ul->set_visible(false);
        v.loadNow = false;
        v.ul->set_load_listener(&s_loadListener.value);
        v.ul->set_view_listener(&s_viewListener.value);
        v.ul->set_editor_listener(&s_editorListener.value);
        v.ul->set_network_listener(&s_networkListener.value);
        // Prefer the on-disk page (the path the React frontend ships
        // through). Relative paths resolve against s_runtimeDir via the
        // platform file system; a drive-letter path loads as-is (SA's
        // HTML lives in SA's own mod folder). Inline banner fallback. A view
        // a host navigation moved loads that URL again, fragment included.
        const bool absolute = v.htmlPath.size() > 1 && v.htmlPath[1] == ':';
        const std::filesystem::path onDisk =
            absolute ? PathText::FromUtf8(v.htmlPath) : (s_runtimeDir / PathText::FromUtf8(v.htmlPath));
        if (!v.htmlPath.empty() && std::filesystem::exists(onDisk)) {
            const std::string url = !v.pageUrl.empty() ? v.pageUrl
                : absolute ? PathToFileUrl(onDisk) : PathText::ToRelativeFileUrl(v.htmlPath);
            SKSE::log::info("Magelight: view {} loading from disk ({})", v.id, v.pageUrl.empty() ? v.htmlPath : v.pageUrl);
            v.ul->LoadURL(url.c_str());
        } else {
            SKSE::log::warn("Magelight: view {} page '{}' missing — inline banner fallback",
                v.id, v.htmlPath);
            std::string banner = kBannerHTML;
            InjectCsp(banner, NetLevel::File);
            v.ul->LoadHTML(banner.c_str());
        }
        v.liveScale = v.deviceScale;
        if (v.rebuilt) SKSE::log::info("Magelight: view {} rebuilt at device scale {:.3f}", v.id, v.deviceScale);
        else if (waking) SKSE::log::info("Magelight: view {} woke from hibernation", v.id);
        v.rebuilt = false;
        // The UI-mode target loading late (a wake, or an entry while it waited in the queue): the focus marker ran
        // against a null View, so re-run it for keys to reach the new page.
        if (s_focused.load() && v.id == static_cast<ViewId>(s_uiModeView.load())) QueueInput(0, 1, 0);
        return true;
    }

    // Staggered first loads (render thread only). A page's parse and first script run land in the following frames'
    // Renderer::Update, so loading every registered view at world ready stacked them all into the first frames after a
    // save loads. The queue starts one waiting view per frame, and the next only once the previous one reached DOM
    // ready or failed (or kStaggerMaxWaitMs passed) and the last frame's Update ran under loadBudgetMs. Both caps are
    // wall time, not frames, so a slow post-load frame rate cannot stretch the queue: a batch older than
    // kStaggerBatchMaxMs starts everything still waiting, which keeps consumers that wait for DOM ready before they
    // show a view (SeverActions' popups and wheel) close to the 0.30.x readiness.
    static constexpr double kStaggerMaxWaitMs = 100.0;
    static constexpr double kStaggerBatchMaxMs = 1000.0;
    struct LoadStagger {
        ViewId last = 0;                 // the latest view loaded; the queue waits for it to settle
        std::chrono::steady_clock::time_point lastLoadAt{};   // epoch = nothing loaded yet (never waits)
        bool   batch = false;            // views have been waiting since batchStart
        int    batchQueued = 0, batchAtOnce = 0, batchFrames = 0, batchAtCap = 0;
        double batchWorstUpdateMs = 0.0;
        std::chrono::steady_clock::time_point batchStart{};
    };
    static LoadStagger s_stagger;
    static double s_lastUlUpdateMs = 0.0;   // the previous frame's Renderer::Update, ms (FrameWork)

    // The hidden view to prepaint this frame (nullptr: none), marked painted. One per frame, and not on a frame
    // whose Update (updateMs) already ran over the load budget: the paint is the cost this moves off the first show,
    // so it must not land on an already heavy frame. Render thread. FrameWork shows its View where it syncs a show
    // (between Update and RefreshDisplay) and hides it again after Render, so that one Render paints it as a show
    // would, requestAnimationFrame included; its page gets one round of rAF callbacks and, in the next Update, two
    // visibilitychange events that both read 'hidden'.
    // A view is prepainted kPrepaintSettleMs after it became paintable (MlView::paintableAt: a DOM ready, a resize, or
    // a host Navigate or Reload that kept its page), then once more (again = true) when OnPageSettled marks it, at most
    // once until the next DOM ready or kept host request (settleRepainted): 2.0 starts decoding a page's images only
    // once it is painted, so the first paint drew none, and this one moves their paint off the first show. Not gated
    // on needs_paint, which is always false while hidden; a show of an unchanged page paints next to nothing.
    static constexpr std::uint64_t kPrepaintSettleMs = 1500;
    static ultralight::View* TakePrepaintView(ViewId& id, bool& again, double updateMs)
    {
        // Not in a headset: the paint is one long frame wherever it lands, and in VR that is a dropped frame the
        // player sees, which the first open (a menu the player chose to raise) hides better.
        if (VR::IsLive()) return nullptr;
        if (updateMs > static_cast<double>(s_loadBudgetMs.load())) return nullptr;
        const std::uint64_t now = GetTickCount64();
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        for (auto& vp : s_views) {
            MlView& v = *vp;
            if (!v.prepaint || v.visible || !v.ul || !v.domReady) continue;
            if (v.dormant || v.destroyPending || v.rebuildPending || v.boundsDirty || v.isInspector) continue;
            if (v.settleRepaint) {
                v.settleRepaint = false;
                v.settleRepainted = true;
                id = v.id;
                again = true;
                return v.ul.get();
            }
            if (v.prepainted || !v.paintableAt) continue;
            if (now - v.paintableAt < kPrepaintSettleMs) continue;
            v.prepainted = true;
            id = v.id;
            return v.ul.get();
        }
        return nullptr;
    }

    // Render thread: give every view that should have a page its View. Without staggering (Magelight.json
    // "loadStagger": false) every waiting view loads in this frame, as before 0.31.0. With it, a view loads at once
    // when it is shown before its load (startVisible or ShowView(true)), is the UI-mode target or wakes from
    // hibernation; the others wait in a queue in registration order, at most kStaggerBatchMaxMs. Hibernated and
    // load-on-show views wait for ShowView(true). Pre-load calls (listeners, InteropCall, EvalJS, InvokeJS) stay queued on the view and
    // are delivered after its DOM ready, exactly as for a view created before the world loaded.
    static void MaterializeViews()
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        const bool stagger = s_loadStagger.load();
        const ViewId uiView = s_focused.load() ? static_cast<ViewId>(s_uiModeView.load()) : 0;
        const auto nowT = std::chrono::steady_clock::now();
        std::vector<MlView*> queued;
        int atOnce = 0;
        for (auto& vp : s_views) {
            MlView& v = *vp;
            if (v.ul || v.w <= 0 || v.h <= 0 || v.destroyPending) continue;
            if (!v.mainMenu && !s_viewsReleased.load()) continue;      // loads at the first in-game frame
            if ((v.dormant || v.loadOnShow) && !v.visible) continue;   // wakes / first loads on ShowView(true)
            const bool now = !stagger || v.dormant || v.loadNow || v.loadOnShow || v.id == uiView;
            if (!now) { queued.push_back(&v); continue; }
            if (LoadViewLocked(v)) {
                ++atOnce;
                s_stagger.last = v.id;
                s_stagger.lastLoadAt = nowT;
            }
        }
        if (queued.empty() && !s_stagger.batch) return;
        if (!s_stagger.batch) {
            s_stagger.batch = true;
            s_stagger.batchQueued = 0;
            s_stagger.batchAtOnce = 0;
            s_stagger.batchFrames = 0;
            s_stagger.batchAtCap = 0;
            s_stagger.batchWorstUpdateMs = 0.0;
            s_stagger.batchStart = nowT;
        }
        ++s_stagger.batchFrames;
        s_stagger.batchAtOnce += atOnce;
        s_stagger.batchWorstUpdateMs = std::max(s_stagger.batchWorstUpdateMs, s_lastUlUpdateMs);
        const auto msSince = [&](std::chrono::steady_clock::time_point t) {
            return std::chrono::duration<double, std::milli>(nowT - t).count();
        };
        if (!queued.empty() && msSince(s_stagger.batchStart) >= kStaggerBatchMaxMs) {
            // The batch ran past its wall-time cap: start everything still waiting now.
            std::vector<MlView*> retry;
            for (MlView* v : queued) {
                if (!LoadViewLocked(*v)) { retry.push_back(v); continue; }   // CreateView failed: a later frame
                ++s_stagger.batchAtCap;
                s_stagger.last = v->id;
                s_stagger.lastLoadAt = nowT;
            }
            queued.swap(retry);
        } else if (!queued.empty() && atOnce == 0) {
            bool settled = true;   // the latest load reached DOM ready or failed, or its view is gone or released
            if (MlView* prev = FindViewLocked(s_stagger.last))
                settled = prev->domReady || prev->loadFailed || !prev->ul || prev->destroyPending;
            const bool due = msSince(s_stagger.lastLoadAt) >= kStaggerMaxWaitMs;
            if (due || (settled && s_lastUlUpdateMs < static_cast<double>(s_loadBudgetMs.load()))) {
                std::sort(queued.begin(), queued.end(), [](const MlView* a, const MlView* b) { return a->id < b->id; });
                for (auto it = queued.begin(); it != queued.end(); ++it) {
                    if (!LoadViewLocked(**it)) continue;   // CreateView failed: retried on a later frame, the next goes now
                    ++s_stagger.batchQueued;
                    s_stagger.last = (*it)->id;
                    s_stagger.lastLoadAt = nowT;
                    queued.erase(it);
                    break;
                }
            }
        }
        if (!queued.empty()) return;
        const int views = s_stagger.batchQueued + s_stagger.batchAtOnce + s_stagger.batchAtCap;
        if (views > 1)
            SKSE::log::info("Magelight: staggered load - {} views over {} frames in {:.0f} ms ({} at once for a show or "
                            "UI mode, {} together at the {:.0f} ms cap; worst Update {:.1f} ms)",
                views, s_stagger.batchFrames, msSince(s_stagger.batchStart), s_stagger.batchAtOnce,
                s_stagger.batchAtCap, kStaggerBatchMaxMs, s_stagger.batchWorstUpdateMs);
        s_stagger.batch = false;
    }

    // Render thread, s_viewsMutex held: drop the View and its texture but keep the record, marked dormant, so
    // MaterializeViews / ShowView(true) load the page again in a new View. The View goes into `released`, which the
    // caller clears after the lock: WebKit's teardown runs page handlers that re-enter the locking listeners.
    static void ReleaseViewKeepRecordLocked(MlView& v, std::vector<ultralight::RefPtr<ultralight::View>>& released)
    {
        ForgetEditStateLocked(v, true);   // while v.ul still names the View its waiting reports came from
        // overlay + host copy go BEFORE the texture (lifetime rule);
        // hibernating=true keeps the view's controller binding (0.28.4)
        VR::ReleaseView(v.id, /*hibernating=*/true);
        if (v.srv) { v.srv->Release(); v.srv = nullptr; }
        if (v.tex) { v.tex->Release(); v.tex = nullptr; }
        v.texW = v.texH = 0;
        v.dormant = true;
        v.domReady = false;
        v.docDomReady = false;
        v.hostLoad.pending = false;
        v.prepainted = false;
        v.paintableAt = 0;
        v.settleRepaint = false;
        v.settleRepainted = false;
        v.liveScale = 0.0f;
        released.push_back(std::move(v.ul));
        v.ul = nullptr;
    }

    // Render thread, before MaterializeViews: release the View + texture of
    // any view hidden longer than its hibernate budget. The record stays;
    // everything the page needs to come back (path, listeners, session,
    // layer, cutout) is on it. Never the UI-mode view, never an inspector.
    static void HibernateIdleViews()
    {
        const std::uint64_t now = GetTickCount64();
        std::vector<ultralight::RefPtr<ultralight::View>> released;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            const ViewId uiView = static_cast<ViewId>(s_uiModeView.load());
            for (auto& vp : s_views) {
                MlView& v = *vp;
                if (!v.ul || v.visible || !v.hibernateMs || !v.hiddenSince || v.destroyPending || v.isInspector) continue;
                if (v.id == uiView && s_focused.load()) continue;
                if (now - v.hiddenSince < v.hibernateMs) continue;
                ReleaseViewKeepRecordLocked(v, released);
                SKSE::log::info("Magelight: view {} hibernated after {} ms hidden (texture released)", v.id, v.hibernateMs);
            }
        }
        released.clear();   // WebKit teardown outside the lock, like a destroy
    }

    // Render thread, after HibernateIdleViews and before MaterializeViews: RebuildViewAtScale. The View is released
    // as hibernation releases it and the record loads again in a new View created with initial_device_scale = the new
    // deviceScale: a visible view in this same frame (MaterializeViews loads a dormant visible view at once), a hidden
    // one at once too unless it hibernates or loads on show (then it stays dormant until shown). The page is never
    // rescaled live here (SetViewScale does that). A page hosting a Web Inspector is not rebuilt (the inspector is
    // bound to its View): it keeps its live scale.
    static void ApplyPendingRebuilds()
    {
        std::vector<ultralight::RefPtr<ultralight::View>> released;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            for (auto& vp : s_views) {
                MlView& v = *vp;
                if (!v.rebuildPending) continue;
                v.rebuildPending = false;
                if (!v.ul || v.destroyPending) continue;   // released meanwhile: the next load uses deviceScale
                if (s_inspectorOf.count(v.id)) {
                    v.deviceScale = v.liveScale;   // hit tests, the IME caret and GetViewInfo keep the View's scale
                    SKSE::log::warn("Magelight: view {} not rebuilt - it hosts a Web Inspector; it stays at device "
                                    "scale {:.3f}", v.id, v.liveScale);
                    continue;
                }
                ReleaseViewKeepRecordLocked(v, released);
                v.scaleDirty = false;     // the new View is created at deviceScale
                v.boundsDirty = false;    // and at the current w/h
                v.reloadedFlag = true;    // ViewReloaded follows the new page's DOM ready
                v.rebuilt = true;
                const bool waitForShow = !v.visible && (v.hibernateMs || v.loadOnShow);
                if (!v.visible && !waitForShow) {   // never hibernates: reload now, hidden
                    v.dormant = false;
                    v.loadNow = true;
                }
                SKSE::log::info("Magelight: view {} released for a rebuild at device scale {:.3f}{}", v.id,
                    v.deviceScale, waitForShow ? " (hidden: it loads when shown)" : "");
            }
        }
        released.clear();   // WebKit teardown outside the lock, like a destroy
    }

    // Render thread: apply a pending SetViewBounds resize or SetViewScale. Under s_viewsMutex because neither call
    // runs page script: the page lays out in the next Update and its resize, matchMedia and ResizeObserver handlers
    // run in Render (a hidden View's in the Render of its next show or prepaint, whichever comes first). The header
    // does not promise that; re-check it at every SDK bump (invariant 16), and move the calls out of the lock as
    // ApplyPendingLifecycle does if it ever changes.
    static void ApplyPendingResizes()
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        for (auto& vp : s_views) {
            MlView& v = *vp;
            if (!v.ul || !v.boundsDirty) continue;
            v.boundsDirty = false;
            const bool sized = v.w > 0 && v.h > 0;
            const auto w = static_cast<std::uint32_t>(v.w), h = static_cast<std::uint32_t>(v.h);
            const float scale = v.deviceScale > 0.f ? v.deviceScale : 1.f;
            const bool newScale = v.scaleDirty && std::fabs(scale - v.liveScale) >= 0.0005f;
            v.scaleDirty = false;
            const bool newSize = sized && (w != v.ul->width() || h != v.ul->height());
            // Nothing changed (a size set back before this frame, a SetViewScale with the scale the View has): no
            // call. On 2.0 a Resize at a fractional scale lays the page out again with its CSS size rounded up even
            // at the same size and scale (innerWidth 1423 for 1920 px at 1.35, where the View created so has 1422).
            if (!newScale && !newSize) continue;
            // A resize repaints the whole page: prepaint it again kPrepaintSettleMs from now. A resize does not reset
            // the page's settle (only a navigation does): a page that already settled gets only this prepaint (a
            // settle repaint still due is folded into it), and one not painted yet settles after it and gets its
            // settle repaint then. The per-load latch (settleRepainted) is left as it is.
            if (v.domReady) {
                v.prepainted = false;
                v.paintableAt = GetTickCount64();
                v.settleRepaint = false;
            }
            if (newScale) {
                // A new scale and the current size in one call, as the SDK asks when both may change.
                if (sized) v.ul->Resize(w, h, scale);
                else v.ul->set_device_scale(scale);
                v.liveScale = scale;
            } else {
                v.ul->Resize(w, h);
            }
            if (sized) {
                // CPU-path texture no longer matches — recreate lazily.
                if (v.srv) { v.srv->Release(); v.srv = nullptr; }
                if (v.tex) { v.tex->Release(); v.tex = nullptr; }
                v.texW = v.texH = 0;
            }
        }
    }

    // Render thread: reloads, navigations and destroys queued by the API.
    // Destroy erases the registry entry and releases the Ultralight view -
    // the listeners key on View*, so erase BEFORE the RefPtr drops (WebKit
    // may recycle the address). Runs outside Update()/Render(), where no
    // listener can be mid-call.
    static void ApplyPendingLifecycle()
    {
        // Collect under the lock, ACT OUTSIDE IT: LoadURL/Reload on a live
        // page and dropping a View's last RefPtr run WebKit synchronously
        // (beforeunload/unload handlers, cancelled provisional loads), which
        // re-enters the console / load-failed listeners — and those take
        // s_viewsMutex. Same shape as DrainBridgeQueues.
        struct Work {
            ultralight::RefPtr<ultralight::View> ul;
            ViewId id;
            std::string url;
            bool reload = false;
            bool alsoReload = false;   // a ReloadView came with the navigation: it stands for it only if it loads
            MlView::HostLoad before;   // put back if the call keeps the page
        };
        std::vector<Work> work;
        std::vector<std::pair<ViewId, ultralight::RefPtr<ultralight::View>>> destroyed;
        // EvalJS requests still queued on a view being destroyed: their fn is
        // the EvalCtx-owning trampoline, so if we drop them the EvalCtx leaks
        // and the mod's callback never fires. Flush them (with a failure)
        // OUTSIDE the lock (review 2026-09-05, finding 2).
        std::vector<std::pair<ViewId, PendingEval>> orphanEvals;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            for (auto it = s_views.begin(); it != s_views.end();) {
                MlView& v = **it;
                if (v.destroyPending) {
                    const ViewId id = v.id;
                    VR::ReleaseView(id);   // overlay destroyed before the View RefPtr drops
                    if (v.srv) { v.srv->Release(); v.srv = nullptr; }
                    if (v.tex) { v.tex->Release(); v.tex = nullptr; }
                    for (auto& e : v.evals) orphanEvals.emplace_back(id, std::move(e));
                    v.evals.clear();
                    if (v.consoleRate.suppressed)
                        SKSE::log::info("Magelight: [view {} console] {} more lines not logged (over {} a second)",
                            id, v.consoleRate.suppressed, kConsoleLinesPerSec);
                    ForgetEditStateLocked(v, true);
                    destroyed.emplace_back(id, std::move(v.ul));   // released after the erase, outside the lock
                    it = s_views.erase(it);
                    continue;
                }
                if (v.ul && (!v.navigateUrl.empty() || v.reloadPending)) {
                    // Set before the call, so a failure fired inside it is caught too.
                    if (!v.hostLoad.pending)
                        v.hostLoad = { true, v.docDomReady, v.reloadedFlag, v.htmlPath, v.pageUrl };
                    Work w{ v.ul, v.id };
                    w.before = v.hostLoad;
                    if (!v.navigateUrl.empty()) {
                        w.alsoReload = v.reloadPending;
                        // Any other URL is stopped as it begins (OnBeginLoading) and leaves the page as it is.
                        if (IsLocalPageUrl(v.navigateUrl)) {
                            v.htmlPath = PathText::PathOfFileUrl(v.navigateUrl);
                            v.pageUrl = v.navigateUrl;
                        }
                        w.url = std::move(v.navigateUrl);
                        v.navigateUrl.clear();
                    } else {
                        w.reload = true;
                    }
                    v.reloadedFlag = true;
                    v.docDomReady = false;
                    work.push_back(std::move(w));
                }
                v.reloadPending = false;
                ++it;
            }
        }
        // A call that begins no main-frame load (s_mainLoadsBegun) keeps the document: a LoadURL that only added or
        // changed the page's fragment (the page scrolls and gets a hashchange), or one OnBeginLoading stopped. No DOM
        // ready follows, so what the request cleared for a new page is put back below. A load that begins can still
        // fail and keep the page, which OnFailLoading puts back the same way (MlView::hostLoad, pending until then).
        // A page that is replaced reports no loss of its text focus, before or after (tools/editor-test). In a View
        // that has had a window object, the new document's comes as it begins, before its reports, and drops the old
        // page's state (OnWindowObjectReady); a failed load brings none, so the page keeps its focused field. In one
        // that has had none, nothing tells the new document from the old before the new one reports, so the state is
        // dropped once the call has begun the load.
        std::vector<const Work*> kept, began;
        for (auto& w : work) {
            const std::uint64_t begun = s_mainLoadsBegun;
            if (w.reload) {
                SKSE::log::info("Magelight: view {} reloading", w.id);
                w.ul->Reload();
            } else {
                SKSE::log::info("Magelight: view {} navigating to {}", w.id, w.url);
                w.ul->LoadURL(w.url.c_str());
                if (s_mainLoadsBegun == begun && w.alsoReload) {
                    SKSE::log::info("Magelight: view {} kept its page on that navigation - reloading it, as asked", w.id);
                    w.ul->Reload();
                }
            }
            if (s_mainLoadsBegun != begun) {
                began.push_back(&w);
                continue;
            }
            SKSE::log::info("Magelight: view {} kept its page on that {} (no DOM ready follows)", w.id,
                w.reload ? "reload" : "navigation");
            kept.push_back(&w);
        }
        if (!began.empty()) {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            for (const Work* w : began) {
                MlView* v = FindViewLocked(w->id);
                if (!v || v->ul.get() != w->ul.get()) continue;
                bool windowed = false;
                {
                    std::lock_guard<std::mutex> ek(s_editMutex);
                    windowed = s_docLoads.HasWindow(v->ul.get());
                }
                if (!windowed) ForgetEditStateLocked(*v);
            }
        }
        if (!kept.empty()) {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            for (const Work* w : kept) {
                MlView* v = FindViewLocked(w->id);
                if (!v || v->ul.get() != w->ul.get()) continue;
                v->hostLoad.pending = false;
                v->docDomReady = w->before.docDomReady;
                v->reloadedFlag = w->before.reloadedFlag;
                // A page still before its DOM ready gets it as usual; a request made since cleared the rest for its
                // own load.
                if (!v->docDomReady || !v->navigateUrl.empty() || v->reloadPending) continue;
                v->domReady = true;
                v->prepainted = false;   // the page scrolled: prepainted again, as after a DOM ready
                v->paintableAt = GetTickCount64();
                v->settleRepaint = false;
                v->settleRepainted = false;
            }
        }
        for (auto& [id, e] : orphanEvals)
            if (e.fn) e.fn(id, "", "Magelight: view destroyed before its eval ran", e.user);
        for (auto& [id, ul] : destroyed) {
            ul = nullptr;   // the registry entry is already gone; a recycled View* can't alias a live view
            SKSE::log::info("Magelight: view {} destroyed", id);
            Emit(HostEvent::ViewDestroyed, id);
        }
    }

    // Render thread: each View's Ultralight visibility follows its record (ShowView only changes the record, from any
    // thread). A hidden View paints nothing in Render, runs no requestAnimationFrame or CSS animation and its page
    // reads document.visibilityState 'hidden'; its timers, messages, InteropCalls and EvalJS still run. FrameWork
    // calls this before the input and again between Update and RefreshDisplay (invariant 23). A view DestroyView
    // marked is hidden here too, until ApplyPendingLifecycle erases it. set_visible runs no page code on 2.0.0-beta.2
    // (the visibilitychange handler runs in the next Update); it is called outside the lock anyway, like the calls
    // that do run page code (invariant 13), so an SDK whose set_visible does cannot re-enter a locking listener.
    static void SyncViewVisibility()
    {
        std::vector<std::pair<ultralight::RefPtr<ultralight::View>, bool>> changes;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            for (auto& vp : s_views)
                if (vp->ul && vp->ul->is_visible() != vp->visible) changes.emplace_back(vp->ul, vp->visible);
        }
        for (auto& [ul, show] : changes) ul->set_visible(show);
    }

    // ── Frame work (render thread) ──────────────────────────────────────────

    // Render thread, each frame after the input: the titles WebCore reported, one per page, as '__tooltip' calls.
    // A change from Renderer::Update reaches the page a frame later; one that lands after a leave is kept by the page
    // and shown only once the pointer is back.
    static void DrainTooltips(float bh)
    {
        std::vector<std::pair<ultralight::View*, std::string>> changes;
        {
            std::lock_guard<std::mutex> lk(s_tooltipMutex);
            changes.swap(s_tooltipChanges);
        }
        const bool leaveAll = s_tooltipLeaveAll.exchange(false);
        if (changes.empty() && !leaveAll) return;
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        for (auto& [ul, text] : changes) {
            MlView* v = FindViewByUlLocked(ul);
            if (!v || (text.empty() && !v->tooltipText)) continue;
            v->tooltipText = !text.empty();
            const float c = Tooltip::CursorCss(s_cursorHeight, bh, v->deviceScale);
            QueueOutboundLocked(*v, { "__tooltip", Tooltip::Payload(text, c), false });
        }
        if (leaveAll)
            for (auto& vp : s_views) QueueTooltipLeaveLocked(*vp);
    }

    // ── Pointer and keys as the pages were sent them (render thread, DrainInputQueue) ──
    // A press keeps its view until every button of its source is up, as in a browser: that source's moves and
    // releases go there wherever it points (Ultralight takes coordinates outside the view and ends the press), so a
    // drag released over another view or the game still ends. Each source has its own press and hover
    // (Input::PointerRoutes); views are named by id and looked up before each use. Positions are the view's CSS px.
    static Input::PointerRoutes s_pointer;
    static ViewId        s_focusedView = 0;   // the view the focus marker last gave View focus (0 = none)
    static ViewId        s_sentKeyView = 0;   // the view the last key event went to
    static std::uint16_t s_sentKeys = 0;      // the Input::KeyBits it went with: the modifiers that page holds down

    // Where the IME's window opens, in the view's CSS px: the composition character Ime::AnchorChar names (it does
    // not jump as the candidates change length), else the caret. Empty when there is neither. Render thread.
    static ultralight::Rect ImeAnchor(ultralight::Editor* ed, const Ime::Snapshot& snap)
    {
        if (!ed) return {};
        if (!ed->HasComposition()) return ed->GetCaretBounds();
        const ultralight::Rect r = ed->GetCompositionCharacterBounds(Ime::AnchorChar(snap));
        return r.IsEmpty() ? ed->GetCompositionCharacterBounds(0) : r;
    }

    // A view about to lose View focus (UI mode ending or moving) keeps its reading as typed, as a browser's blur does:
    // Ultralight 2.0's Unfocus leaves a composition open (no compositionend, no OnDiscardComposition), so the next IME
    // change would replace it, in that view or in the next one. The IME drops its copy (kImeCtlMsg 3), which ends the
    // page's list too. Render thread, outside s_viewsMutex (compositionend runs page script).
    static void ImeFinishComposition(ultralight::View* v)
    {
        ultralight::Editor* ed = v ? v->editor() : nullptr;
        if (!ed || !ed->HasComposition()) return;
        ed->FinishComposition();
        if (s_hwnd) PostMessageW(s_hwnd, kImeCtlMsg, 3, 0);
    }

    // s_imeEditorComposing, once a frame after the input and the focus reports: whether the UI-mode view's Editor holds
    // a composition (false outside UI mode). Render thread, on the fiber; the view is looked up under s_viewsMutex and
    // asked outside it.
    static void NoteEditorComposition()
    {
        if (!s_focused.load()) {
            s_imeEditorComposing.store(false);
            return;
        }
        ultralight::RefPtr<ultralight::View> ul;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            if (MlView* v = FindViewLocked(static_cast<ViewId>(s_uiModeView.load()))) ul = v->ul;
        }
        s_imeEditorComposing.store(ul && ul->editor()->HasComposition());
    }

    // The view the last '__ime' message other than an 'end' went to (render thread): an 'end' goes there, and a
    // change for another view first ends it there, so no page keeps a list the IME has closed.
    static ViewId s_imePageView = 0;

    // An IME op's '__ime' message, once the op has run: its state and text, the IME's state and the anchor. Render
    // thread, outside s_viewsMutex (InteropCall takes it).
    static void ImeTellPage(const EditOp& op, ViewId uiId, const ultralight::Rect& anchor)
    {
        const bool end = std::strcmp(op.imeState, "end") == 0;
        if (s_imePageView && (end || s_imePageView != uiId)) InteropCall(s_imePageView, "__ime", Ime::Payload("end", "", {}));
        if (end) {
            if (!s_imePageView && uiId) InteropCall(uiId, "__ime", Ime::Payload("end", "", {}));
            s_imePageView = 0;
            return;
        }
        if (!uiId) return;
        s_imePageView = uiId;
        Ime::Snapshot snap = op.ime;
        if (!anchor.IsEmpty()) {
            snap.hasAnchor = true;
            snap.anchorX = anchor.left;
            snap.anchorY = anchor.top;
            snap.anchorH = anchor.bottom - anchor.top;
        }
        // text: the committed string on 'commit', the composition on 'compose'.
        const std::string text = op.kind == EditOp::Commit ? WideToUtf8(op.text)
                               : std::strcmp(op.imeState, "compose") == 0 ? snap.composition : std::string();
        InteropCall(uiId, "__ime", Ime::Payload(op.imeState, text, snap));
    }

    // Translate queued Windows messages into Ultralight events. Render thread only (the sole thread that touches
    // views); no Ultralight call is made under s_viewsMutex.
    // Routing: keys and text -> the UI-mode view. A pointer event -> the view a button its source holds was pressed
    // on, else the topmost visible interactive view under the point, else the UI-mode view (a point outside every
    // view reaches it as a move outside its page). The wheel always goes where it points.
    static void DrainInputQueue(float bw, float bh)
    {
        std::vector<InputMsg> msgs;
        std::vector<EditOp> editOps;
        {
            std::lock_guard<std::mutex> lk(s_inputMutex);
            msgs.swap(s_inputQueue);
            editOps.swap(s_editOps);
        }
        if (msgs.empty()) return;

        ultralight::RefPtr<ultralight::View> uiUl;
        ViewId uiId = 0;
        int uiX = 0, uiY = 0;
        float uiDs = 1.f;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            if (MlView* v = FindViewLocked(static_cast<ViewId>(s_uiModeView.load()))) {
                uiUl = v->ul;
                uiId = v->id;
                float ex = 0, ey = 0;
                EffectivePos(*v, bw, bh, ex, ey);
                uiX = static_cast<int>(ex);
                uiY = static_cast<int>(ey);
                uiDs = v->deviceScale > 0.f ? v->deviceScale : 1.f;
            }
        }

        // Ultralight takes mouse coordinates in LOGICAL (CSS) pixels: AppCore
        // converts window pixels with its DPI scale before FireMouseEvent, so
        // a view rendered at deviceScale S needs (px / S). Backbuffer-space px
        // reach here from both the Win32 sink and the VR laser (it posts into
        // the same queue), so this one spot covers both.
        using UL = ultralight::MouseEvent;

        // Where a pointer event goes: the view, its screen origin, device scale and wheel step. inside = the point
        // is on that view (false for the UI-mode fallback and the held view).
        struct PointerTarget {
            ultralight::RefPtr<ultralight::View> ul;
            ViewId id = 0;
            int tx = 0, ty = 0;
            float ds = 1.f;
            int step = 40;
            bool inside = false;
            int X(int mx) const { return static_cast<int>((mx - tx) / ds); }
            int Y(int my) const { return static_cast<int>((my - ty) / ds); }
        };
        auto targetOf = [&](const MlView& v) {   // s_viewsMutex held
            PointerTarget t;
            float ex = 0, ey = 0;
            EffectivePos(v, bw, bh, ex, ey);
            t.ul = v.ul;
            t.id = v.id;
            t.tx = static_cast<int>(ex);
            t.ty = static_cast<int>(ey);
            t.ds = v.deviceScale > 0.f ? v.deviceScale : 1.f;
            t.step = v.scrollStep > 0 ? v.scrollStep : 40;   // 0.28.0 per-view wheel step
            return t;
        };
        // The topmost visible interactive view containing (mx, my), else the UI-mode view.
        auto hitTest = [&](int mx, int my) {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            for (auto it = s_views.rbegin(); it != s_views.rend(); ++it) {
                const MlView& v = **it;
                if (!v.ul || !v.visible || v.clickThrough || !ViewShows(v)) continue;
                float ex = 0, ey = 0;
                EffectivePos(v, bw, bh, ex, ey);
                if (mx >= ex && my >= ey && mx < ex + v.w && my < ey + v.h) {
                    PointerTarget t = targetOf(v);
                    t.inside = true;
                    return t;
                }
            }
            const MlView* u = FindViewLocked(static_cast<ViewId>(s_uiModeView.load()));
            return (u && u->ul) ? targetOf(*u) : PointerTarget{};
        };
        // The view `src`'s held buttons were pressed on, while it keeps its page; otherwise that hold is over.
        auto captured = [&](unsigned src) {
            const ViewId id = s_pointer.Captured(src);
            if (!id) return PointerTarget{};
            {
                std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
                const MlView* v = FindViewLocked(id);
                if (v && v->ul && !v->destroyPending) return targetOf(*v);
            }
            s_pointer.Drop(src);
            return PointerTarget{};
        };
        auto pointerTarget = [&](unsigned src, int mx, int my) {
            PointerTarget t = captured(src);
            return t.ul ? t : hitTest(mx, my);
        };
        // `src`'s pointer went to view `id`. The view it left hides its tooltip and gets a move outside itself (its
        // mouseout and mouseleave fire and its :hover clears), each when PointerRoutes::Hover says so.
        auto noteHover = [&](unsigned src, ViewId id) {
            const Input::PointerRoutes::Left gone = s_pointer.Hover(src, id);
            if (!gone.tooltip) return;
            ultralight::RefPtr<ultralight::View> left;
            {
                std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
                if (MlView* prev = FindViewLocked(gone.tooltip)) {
                    QueueTooltipLeaveLocked(*prev);
                    if (gone.leave && !prev->destroyPending) left = prev->ul;
                }
            }
            if (left) {
                UL out{ UL::kType_MouseMoved, -1, -1, UL::kButton_None };
                left->FireMouseEvent(out);
            }
        };
        auto buttonOf = [](const InputMsg& e) {
            switch (e.msg) {
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: return UL::kButton_Right;
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: return UL::kButton_Middle;
            case WM_XBUTTONDOWN: case WM_XBUTTONUP:
                return GET_XBUTTON_WPARAM(e.wparam) == XBUTTON2 ? UL::kButton_Forward : UL::kButton_Back;
            default: return UL::kButton_Left;
            }
        };

        // Every key event is made by Input::MakeKeyEvent (never the bare constructor: on VR it would lose every
        // modifier), with the state the key was queued with.
        auto keyEvent = [](ultralight::KeyEvent::Type type, WPARAM w, LPARAM l, bool sys, std::uint16_t keys) {
            if (PageThreadOn()) SyncPageKeyboardLayout();
            bool applied = true;
            ultralight::KeyEvent evt = Input::MakeKeyEvent(type, w, l, sys, keys, &applied);
            static bool s_warned = false;
            if (!applied && !s_warned) {
                s_warned = true;
                SKSE::log::warn("Magelight: the render thread's keyboard state could not be read; keys reach pages "
                                "with that thread's own modifiers");
            }
            return evt;
        };
        // A character as a Char key event to the UI-mode view: one UTF-16 unit, or a surrogate pair, which is built by
        // hand (the constructor takes one unit; the public modifiers of a hand-built event count). The Windows keys are
        // left out: no event the constructor makes reports metaKey (invariant 24).
        auto fireChar = [&](const ultralight::Char16* units, unsigned n, LPARAM l, std::uint16_t keys) {
            if (n == 1) {
                uiUl->FireKeyEvent(keyEvent(ultralight::KeyEvent::kType_Char, units[0], l, false, keys));
                return;
            }
            ultralight::KeyEvent evt;
            evt.type = ultralight::KeyEvent::kType_Char;
            evt.modifiers = Input::ModsFromKeyBits(static_cast<std::uint16_t>(keys & ~(Input::kKbLWin | Input::kKbRWin)));
            evt.text = ultralight::String(ultralight::String16(units, n));
            evt.unmodified_text = evt.text;
            uiUl->FireKeyEvent(evt);
        };
        // Releases no message will bring (focus lost, UI mode left or moved): a key-up for each modifier the page
        // that last got a key holds down, so a later click reports none (mouse events take their modifiers from the
        // last key event), and a release of each held button at the last position its view was sent.
        auto releaseKeys = [&]() {
            const unsigned held = s_sentKeys & Input::kKbModifiers;
            if (!held) return;
            std::uint16_t keys = s_sentKeys;
            s_sentKeys = static_cast<std::uint16_t>(s_sentKeys & ~Input::kKbModifiers);
            ultralight::RefPtr<ultralight::View> ul;
            {
                std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
                if (MlView* v = s_sentKeyView ? FindViewLocked(s_sentKeyView) : nullptr) ul = v->ul;
            }
            if (!ul) return;
            for (int i = 0; i < 8; ++i) {
                if (!(held & (1u << i))) continue;
                keys = static_cast<std::uint16_t>(keys & ~(1u << i));
                const ultralight::KeyEvent evt = keyEvent(ultralight::KeyEvent::kType_KeyUp,
                    static_cast<WPARAM>(Input::kModifierMsgVk[i]),
                    Input::SyntheticKeyLParam(static_cast<UINT>(Input::kModifierVk[i]), true), false, keys);
                ul->FireKeyEvent(evt);
            }
        };
        auto releaseButtons = [&]() {
            for (unsigned src = 0; src < Input::kPointerSources; ++src) {
                const PointerTarget t = captured(src);
                const Input::PointerRoutes::Held held = s_pointer.TakeHeld(src);
                if (!t.ul) continue;
                for (int b = UL::kButton_Left; b <= UL::kButton_Forward; ++b) {
                    if (!(held.mask & (1u << b))) continue;
                    UL up{ UL::kType_MouseUp, held.x, held.y, static_cast<UL::Button>(b) };
                    t.ul->FireMouseEvent(up);
                }
            }
        };

        for (const auto& e : msgs) {
            switch (e.msg) {
            case kEditOpMsg: {   // the UI-mode page's own editor
                if (e.wparam >= editOps.size()) break;
                const EditOp& op = editOps[e.wparam];
                if (!uiUl || op.kind == EditOp::Notify) {   // nothing to edit: the page only hears of it
                    if (*op.imeState) ImeTellPage(op, uiId, uiUl ? ImeAnchor(uiUl->editor(), op.ime) : ultralight::Rect{});
                    break;
                }
                ultralight::Editor* ed = uiUl->editor();
                if (op.kind == EditOp::Command) {
                    const bool ran = ed->Execute(op.cmd);
                    static std::atomic<int> s_n{ 0 };
                    if (s_n.fetch_add(1) < 8)
                        SKSE::log::info("Magelight: keyboard edit command {} {} in view {}", EditState::CommandName(op.cmd),
                                        ran ? "ran" : "did nothing", uiId);
                    break;
                }
                const ultralight::String16 t16(reinterpret_cast<const ultralight::Char16*>(op.text.data()), op.text.size());
                const ultralight::String text(t16);
                if (op.kind == EditOp::Insert) {
                    if (op.text.empty() || ed->InsertText(text)) break;
                    // No field takes text (a button, or nothing, has the focus): the characters go as Char key events,
                    // as all VR typing did before, so a page that reads keypress still gets them. InsertText dispatched
                    // nothing then (tools/editor-test), so nothing is sent twice.
                    for (std::size_t i = 0; i < op.text.size(); ++i) {
                        ultralight::Char16 u[2] = { static_cast<ultralight::Char16>(op.text[i]), 0 };
                        unsigned n = 1;
                        if (u[0] >= 0xD800 && u[0] <= 0xDBFF && i + 1 < op.text.size())   // SplitTyped keeps pairs whole
                            u[n++] = static_cast<ultralight::Char16>(op.text[++i]);
                        fireChar(u, n, 0, 0);
                    }
                    static std::atomic<int> s_n{ 0 };
                    if (s_n.fetch_add(1) < 4)
                        SKSE::log::info("Magelight: typed text with no editable field focused in view {} - sent as {} "
                                        "key character(s)", uiId, op.text.size());
                    break;
                }
                if (op.kind == EditOp::Compose && !op.text.empty()) {
                    const ultralight::CompositionRange caret{ op.cursor, op.cursor };
                    ed->SetComposition(text, op.segs.empty() ? nullptr : op.segs.data(), op.segs.size(), caret);
                } else if (op.kind == EditOp::Commit) {
                    ed->CommitComposition(text);
                } else if (ed->HasComposition()) {   // an emptied composition, or the IME closed without a result
                    if (op.kind == EditOp::End && op.keep) ed->FinishComposition();
                    else ed->CancelComposition();
                }
                const ultralight::Rect r = ImeAnchor(ed, op.ime);
                if (!r.IsEmpty() && s_hwnd) {
                    s_imeCaretX.store(uiX + static_cast<int>(r.left * uiDs));
                    s_imeCaretY.store(uiY + static_cast<int>(r.top * uiDs));
                    s_imeCaretH.store(static_cast<int>((r.bottom - r.top) * uiDs));
                    PostMessageW(s_hwnd, kImeCtlMsg, 2, 0);
                }
                if (*op.imeState) ImeTellPage(op, uiId, r);
                break;
            }
            case 0: {   // focus marker (SetUIMode, SwitchUIModeView, the UI-mode view's late load): the UI-mode view
                if (!e.wparam) {   // while the page still has key focus
                    releaseKeys();
                    releaseButtons();
                } else if (s_sentKeyView != uiId) {
                    releaseKeys();   // UI mode moved to another view: the page it left holds no modifier
                }
                // Only the UI-mode view has View focus (views are created without it): the one focused before loses
                // it when UI mode moves. The reports these calls fire are marked (s_hostFocusCall): never a raise.
                ultralight::RefPtr<ultralight::View> before;
                if (s_focusedView && (s_focusedView != uiId || !e.wparam)) {
                    std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
                    if (MlView* v = FindViewLocked(s_focusedView); v && v->ul && v->ul != uiUl) before = v->ul;
                }
                s_hostFocusCall = true;
                if (before) {
                    ImeFinishComposition(before.get());
                    before->Unfocus();
                }
                if (uiUl) {
                    if (e.wparam) uiUl->Focus();
                    else {
                        ImeFinishComposition(uiUl.get());
                        uiUl->Unfocus();
                    }
                }
                s_hostFocusCall = false;
                s_focusedView = (e.wparam && uiUl) ? uiId : 0;
                if (uiUl) SKSE::log::info("Magelight: view {} {}", uiId, e.wparam ? "focused" : "unfocused");
                break;
            }
            case kKeyReleaseMsg:
                releaseKeys();
                releaseButtons();
                break;
            case WM_MOUSEMOVE: {
                const int mx = GET_X_LPARAM(e.lparam), my = GET_Y_LPARAM(e.lparam);
                const PointerTarget t = pointerTarget(e.source, mx, my);
                noteHover(e.source, t.id);
                if (!t.ul) break;
                UL evt{ UL::kType_MouseMoved, t.X(mx), t.Y(my), UL::kButton_None };
                t.ul->FireMouseEvent(evt);
                s_pointer.Moved(e.source, t.id, evt.x, evt.y);
                break;
            }
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN: {
                const UL::Button btn = buttonOf(e);
                const int mx = GET_X_LPARAM(e.lparam), my = GET_Y_LPARAM(e.lparam);
                const PointerTarget t = pointerTarget(e.source, mx, my);
                noteHover(e.source, t.id);
                if (!t.ul) break;
                UL evt{ UL::kType_MouseDown, t.X(mx), t.Y(my), btn };
                t.ul->FireMouseEvent(evt);
                // A press on the field that already has the focus reports nothing: DrainEditableStates looks for it.
                if (btn == UL::kButton_Left && t.inside && t.id == uiId && s_uiClicks.size() < 16)
                    s_uiClicks.push_back({ static_cast<float>(evt.x), static_cast<float>(evt.y) });
                s_pointer.Press(e.source, t.id, btn, evt.x, evt.y);
                break;
            }
            case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP: {
                const UL::Button btn = buttonOf(e);
                const int mx = GET_X_LPARAM(e.lparam), my = GET_Y_LPARAM(e.lparam);
                const PointerTarget t = pointerTarget(e.source, mx, my);
                const UL evt{ UL::kType_MouseUp, t.X(mx), t.Y(my), btn };
                if (t.ul) t.ul->FireMouseEvent(evt);
                s_pointer.Release(e.source, t.ul ? t.id : 0, btn, evt.x, evt.y);
                break;
            }
            case kPadScrollMsg: {
                const int mx = GET_X_LPARAM(e.lparam), my = GET_Y_LPARAM(e.lparam);
                const PointerTarget t = hitTest(mx, my);
                if (!t.ul) break;
                ultralight::ScrollEvent evt{ ultralight::ScrollEvent::kType_ScrollByPixel, 0,
                                             static_cast<int>(static_cast<std::intptr_t>(e.wparam)) };
                if (t.inside) {
                    evt.x = t.X(mx);
                    evt.y = t.Y(my);
                }
                t.ul->FireScrollEvent(evt);
                break;
            }
            case WM_MOUSEWHEEL: {
                // ~40 px per notch (WHEEL_DELTA = 120), at the point: the element under the pointer scrolls even on
                // a page that has had no move. Outside every view the UI-mode page scrolls where its pointer was.
                const int mx = GET_X_LPARAM(e.lparam), my = GET_Y_LPARAM(e.lparam);
                const PointerTarget t = hitTest(mx, my);
                if (!t.ul) break;
                const int delta = GET_WHEEL_DELTA_WPARAM(e.wparam) * t.step / WHEEL_DELTA;   // default 40px per notch
                ultralight::ScrollEvent evt{ ultralight::ScrollEvent::kType_ScrollByPixel, 0, delta };
                if (t.inside) {
                    evt.x = t.X(mx);
                    evt.y = t.Y(my);
                }
                t.ul->FireScrollEvent(evt);
                break;
            }
            case WM_KEYDOWN: case WM_SYSKEYDOWN:
            case WM_KEYUP: case WM_SYSKEYUP: {
                if (!uiUl) break;
                if (s_sentKeyView != uiId) releaseKeys();   // UI mode moved: the page it left holds no modifier
                const bool down = e.msg == WM_KEYDOWN || e.msg == WM_SYSKEYDOWN;
                const bool sys = e.msg == WM_SYSKEYDOWN || e.msg == WM_SYSKEYUP;
                const ultralight::KeyEvent evt = keyEvent(
                    down ? ultralight::KeyEvent::kType_RawKeyDown : ultralight::KeyEvent::kType_KeyUp,
                    e.wparam, e.lparam, sys, e.keys);
                uiUl->FireKeyEvent(evt);
                s_sentKeyView = uiId;
                s_sentKeys = e.keys;
                if (down && (e.wparam == VK_BACK || e.wparam == VK_DELETE) && !(e.lparam & (1 << 30))) {
                    static std::atomic<int> s_n{ 0 };
                    if (s_n.fetch_add(1) < 8)
                        SKSE::log::info("Magelight: RawKeyDown vk=0x{:02X} scan=0x{:03X} fired at view {}",
                            e.wparam, (e.lparam >> 16) & 0x1FF, uiId);
                }
                break;
            }
            case WM_CHAR: {
                if (!uiUl) break;
                const ultralight::Char16 wch = static_cast<ultralight::Char16>(e.wparam);
                // Control characters (Ctrl+C = 0x03, Ctrl+V = 0x16, ...) are
                // editing commands already delivered by the RawKeyDown with
                // its Ctrl; as text they would insert garbage. Tab and
                // Enter stay text (textarea newline / tab).
                if (wch < 0x20 && wch != L'\t' && wch != L'\r' && wch != L'\n') break;
                // A character outside the BMP (emoji, rare ideographs) arrives as two
                // WM_CHARs; a lone half fired on its own is U+FFFD in the page. Hold the
                // high half and fire the pair together.
                static ultralight::Char16 s_pendingHigh = 0;
                ultralight::Char16 units[2]; unsigned n = 0;
                if (wch >= 0xD800 && wch <= 0xDBFF) { s_pendingHigh = wch; break; }
                if (wch >= 0xDC00 && wch <= 0xDFFF) { if (!s_pendingHigh) break; units[n++] = s_pendingHigh; s_pendingHigh = 0; }
                units[n++] = wch;
                if (s_sentKeyView != uiId) releaseKeys();
                fireChar(units, n, e.lparam, e.keys);
                s_sentKeyView = uiId;
                s_sentKeys = e.keys;
                break;
            }
            default:
                break;
            }
        }
    }

    // ── Text focus: the VR keyboards and the IME (render thread) ──
    // The reports the EditorListener recorded since the last call are applied to their views, and EditState::Tracker
    // decides once what the keyboards and the IME do (the rules are in EditStateCore.h). FrameWork calls this right
    // after DrainInputQueue, before the bridge drain and the second visibility sync: a keyboard raised by a click,
    // a key or the focus marker gets its message in this frame's DrainBridgeQueues and is composited in this frame.
    // A report fired later in the frame (a page's focus() from an InteropCall or a timer, a new page's autofocus in
    // Render) is acted on in the next one.
    static void DrainEditableStates()
    {
        std::vector<EditChange> raw;
        {
            std::lock_guard<std::mutex> lk(s_editMutex);
            raw.swap(s_editChanges);
        }
        std::vector<EditState::Click> clicks;
        clicks.swap(s_uiClicks);
        std::vector<ViewId> resets;
        resets.swap(s_editResets);
        const bool resync = !resets.empty();
        ViewId ui = IsUIModeActive() ? static_cast<ViewId>(s_uiModeView.load()) : 0;
        if (ui == static_cast<ViewId>(s_kbViewId.load())) ui = 0;
        static EditState::Tracker s_tracker;
        if (s_tracker.Idle(ui, !raw.empty(), !clicks.empty(), resync)) return;

        std::vector<EditState::Change> changes;
        changes.reserve(raw.size());
        ultralight::EditableState st{};
        ultralight::RefPtr<ultralight::View> uiUl;
        bool uiReported = false;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            for (const EditChange& c : raw) {
                MlView* v = FindViewByUlLocked(c.ul);
                if (!v) continue;
                changes.push_back({ v->id, v->edit, c.st, c.hostFocus });
                v->edit = c.st;
                if (v->id == ui) uiReported = true;
            }
            if (MlView* u = ui ? FindViewLocked(ui) : nullptr) {
                st = u->edit;
                uiUl = u->ul;
            }
        }
        // Where the focused field is now, for a press on it (EditState::FieldNow): the caret when the field was
        // reported, and now. Ultralight calls, made outside the lock. The caret at the report is forgotten when UI mode
        // moves or the UI-mode view's own state was reset, never for another view's reset (a load elsewhere).
        static ultralight::Rect s_caretAtReport{ 0.f, 0.f, 0.f, 0.f };
        static ViewId s_caretView = 0;
        if (ui != s_caretView || std::find(resets.begin(), resets.end(), ui) != resets.end()) {
            s_caretView = ui;
            s_caretAtReport = { 0.f, 0.f, 0.f, 0.f };
        }
        const bool wants = uiUl && EditState::WantsKeyboard(st);
        if (uiReported) s_caretAtReport = wants ? uiUl->editor()->GetCaretBounds() : ultralight::Rect{ 0.f, 0.f, 0.f, 0.f };
        ultralight::Rect field{ 0.f, 0.f, 0.f, 0.f };
        const ultralight::Rect* fieldNow = nullptr;
        if (wants && !clicks.empty()) {
            field = EditState::FieldNow(st, s_caretAtReport, uiUl->editor()->GetCaretBounds());
            fieldNow = &field;
        }
        // A report on the UI-mode view's single-line field: the page says whether it is a number field, which takes no
        // IME (EditState::kNumberFieldScript). Asked only in a View that has had a window object, where a script brings
        // no new document's (EditState::DocLoads); outside s_viewsMutex, as every host script runs (invariant 16).
        bool number = false;
        if (uiUl && EditState::MayBeNumberField(st)) {
            bool asked = false;
            if (uiReported) {
                bool windowed = false;
                {
                    std::lock_guard<std::mutex> lk(s_editMutex);
                    windowed = s_docLoads.HasWindow(uiUl.get());
                }
                if (windowed) {
                    if (auto ctxScope = uiUl->LockJSContext()) {
                        std::string result;
                        JsBridge::ErrorInfo err;
                        number = JsBridge::Evaluate(*ctxScope, EditState::kNumberFieldScript, "magelight:numberfield", result,
                                                    err) && result == "1";
                    }
                }
                asked = true;
            }
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            if (MlView* u = FindViewLocked(ui)) {
                if (asked) u->editNumber = number;
                else number = u->editNumber;
            }
        } else if (uiUl) {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            if (MlView* u = FindViewLocked(ui)) u->editNumber = false;
        }
        const EditState::Verdict vd = s_tracker.Step(ui, changes, st, clicks, resync, fieldNow, number);
        if (!vd.touched) return;

        ImeOnTextFocus(vd.ime);
        static ViewId s_kbClaim = 0;   // the view the runtime keyboard was raised for (VR::NoteTextFocus)
        if (vd.keyboard == EditState::Keyboard::Raise) {
            if (s_kbClaim && s_kbClaim != ui) VR::NoteTextFocus(s_kbClaim, false);
            s_kbClaim = ui;
            VR::TextField f;
            f.password = st.type == ultralight::EditableType::Password;
            f.multiline = st.type == ultralight::EditableType::TextArea || st.type == ultralight::EditableType::ContentEditable;
            VR::NoteTextFocus(ui, true, f);
            ShowOwnKeyboard(true);
        } else if (vd.keyboard == EditState::Keyboard::Hide) {
            if (s_kbClaim) VR::NoteTextFocus(s_kbClaim, false);
            s_kbClaim = 0;
            ShowOwnKeyboard(false);
        } else {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            QueueKeyboardThemeLocked();   // the field changed under a keyboard that stays as it is: its layout follows
        }
        // A line for each change the player can see, the keyboard in VR and the IME on flat (32 a session).
        static int s_lines = 0, s_lastKb = -1, s_lastIme = -1;
        const bool vr = VR::IsLive();
        const int kbNow = vd.keyboard == EditState::Keyboard::Raise ? 1 : vd.keyboard == EditState::Keyboard::Hide ? 0 : s_lastKb;
        const bool seen = vr ? (vd.keyboard == EditState::Keyboard::Raise || kbNow != s_lastKb)
                             : static_cast<int>(vd.ime) != s_lastIme;
        s_lastKb = kbNow;
        s_lastIme = vd.ime ? 1 : 0;
        if (seen && s_lines <= 32) {
            if (s_lines++ == 32)
                SKSE::log::info("Magelight: text focus lines capped for this session (32 logged)");
            else if (vr)
                SKSE::log::info("Magelight: text focus view {} {} -> keyboard {}", ui, EditState::FieldJson(st, number),
                                kbNow == 1 ? "up" : "down");
            else
                SKSE::log::info("Magelight: text focus view {} {} -> IME {}", ui, EditState::FieldJson(st, number),
                                vd.ime ? "on" : "off");
        }
    }

    // CPU-surface path: (re)create the view's dynamic texture to match its
    // current size. Render thread.
    static bool EnsureCpuTexture(MlView& v)
    {
        if (v.tex && v.texW == v.w && v.texH == v.h) return true;
        if (v.srv) { v.srv->Release(); v.srv = nullptr; }
        if (v.tex) { v.tex->Release(); v.tex = nullptr; }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(v.w);
        td.Height = static_cast<UINT>(v.h);
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(s_device->CreateTexture2D(&td, nullptr, &v.tex))) return false;
        if (FAILED(s_device->CreateShaderResourceView(v.tex, nullptr, &v.srv))) {
            v.tex->Release(); v.tex = nullptr;
            return false;
        }
        v.texW = v.w; v.texH = v.h;
        return true;
    }

    // `ctx`: the game's immediate context in the frame, the page thread's deferred one with pageThread (a dynamic
    // texture maps WRITE_DISCARD on either).
    static void UploadSurfaceIfDirty(MlView& v, ID3D11DeviceContext* ctx)
    {
        auto* surface = static_cast<ultralight::BitmapSurface*>(v.ul->surface());
        if (!surface) return;
        if (surface->dirty_bounds().IsEmpty()) return;
        if (!EnsureCpuTexture(v)) return;

        ultralight::RefPtr<ultralight::Bitmap> bmp = surface->bitmap();
        if (!bmp) return;
        const void* px = bmp->LockPixels();
        if (px) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(ctx->Map(v.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                const auto rowBytes = bmp->row_bytes();
                const auto rows = bmp->height();
                const auto copyBytes = (rowBytes < mapped.RowPitch) ? rowBytes : mapped.RowPitch;
                auto* src = static_cast<const std::uint8_t*>(px);
                auto* dst = static_cast<std::uint8_t*>(mapped.pData);
                for (std::uint32_t y = 0; y < rows; ++y) {
                    std::memcpy(dst + y * mapped.RowPitch, src + y * rowBytes, copyBytes);
                }
                ctx->Unmap(v.tex, 0);
            }
        }
        bmp->UnlockPixels();
        surface->ClearDirtyBounds();
    }

    // Everything we touch gets saved and restored — the game's renderer must
    // never notice we were here. The set is measured, not guessed: a
    // sentinel-state harness run reported the slots that stayed wrong after
    // Restore (the index buffer, PS SRVs 1-2, VS CB0, the input layout when
    // Render created geometry before the capture, and the UAVs an 8-slot
    // OMSetRenderTargets unbinds). Capture BEFORE Renderer::Render.
    struct StateBackup {
        static constexpr UINT kSrvSlots = 4;   // the driver binds PS t0-t2 (t3 in later SDKs); VS slots are a belt
        D3D11_PRIMITIVE_TOPOLOGY topo{};
        ID3D11InputLayout* layout = nullptr;
        ID3D11Buffer* vb = nullptr; UINT vbStride = 0, vbOffset = 0;
        ID3D11Buffer* ib = nullptr; DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN; UINT ibOffset = 0;
        ID3D11VertexShader* vs = nullptr;
        ID3D11PixelShader* ps = nullptr;
        ID3D11GeometryShader* gs = nullptr;
        ID3D11HullShader* hs = nullptr;
        ID3D11DomainShader* ds = nullptr;
        ID3D11ShaderResourceView* psSrvs[kSrvSlots]{};
        ID3D11ShaderResourceView* vsSrvs[kSrvSlots]{};
        ID3D11SamplerState* sampler = nullptr;
        ID3D11Buffer* psCb = nullptr;
        ID3D11Buffer* vsCb = nullptr;
        ID3D11BlendState* blend = nullptr; FLOAT blendFactor[4]{}; UINT sampleMask = 0;
        ID3D11DepthStencilState* depth = nullptr; UINT stencilRef = 0;
        ID3D11RasterizerState* rs = nullptr;
        ID3D11RenderTargetView* rtvs[8]{}; ID3D11DepthStencilView* dsv = nullptr;
        ID3D11UnorderedAccessView* uavs[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
        ID3D11ComputeShader* cs = nullptr;
        ID3D11Buffer* soTargets[4]{};
        ID3D11Predicate* predicate = nullptr; BOOL predicateValue = FALSE;
        UINT vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT scCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_RECT scs[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};

        void Capture(ID3D11DeviceContext* c)
        {
            c->IAGetPrimitiveTopology(&topo);
            c->IAGetInputLayout(&layout);
            c->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
            c->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
            c->VSGetShader(&vs, nullptr, nullptr);
            c->PSGetShader(&ps, nullptr, nullptr);
            c->GSGetShader(&gs, nullptr, nullptr);
            c->HSGetShader(&hs, nullptr, nullptr);
            c->DSGetShader(&ds, nullptr, nullptr);
            c->PSGetShaderResources(0, kSrvSlots, psSrvs);
            c->VSGetShaderResources(0, kSrvSlots, vsSrvs);
            c->PSGetSamplers(0, 1, &sampler);
            c->PSGetConstantBuffers(0, 1, &psCb);
            c->VSGetConstantBuffers(0, 1, &vsCb);
            c->OMGetBlendState(&blend, blendFactor, &sampleMask);
            c->OMGetDepthStencilState(&depth, &stencilRef);
            c->RSGetState(&rs);
            c->OMGetRenderTargets(8, rtvs, &dsv);
            c->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, D3D11_PS_CS_UAV_REGISTER_COUNT, uavs);
            c->RSGetViewports(&vpCount, vps);
            c->RSGetScissorRects(&scCount, scs);
            c->CSGetShader(&cs, nullptr, nullptr);
            c->SOGetTargets(4, soTargets);
            c->GetPredication(&predicate, &predicateValue);
        }
        // Put the pipeline in the state Ultralight's reference driver takes
        // for granted (AppCore owns a pristine context; we borrow the game's
        // mid-frame). The driver rebinds VS/PS/IA/RT/blend/rasterizer per
        // draw but never touches GS/HS/DS, depth-stencil, stream-output or
        // predication — any of which the game may leave armed. Field
        // symptom (2026-09-01): path-tessellated borders (a box whose sides
        // differ) drew as huge wedge triangles in-game while the same page
        // rendered correctly through AppCore on the same SDK.
        static void Neutralize(ID3D11DeviceContext* c)
        {
            c->GSSetShader(nullptr, nullptr, 0);
            c->HSSetShader(nullptr, nullptr, 0);
            c->DSSetShader(nullptr, nullptr, 0);
            c->CSSetShader(nullptr, nullptr, 0);
            c->OMSetDepthStencilState(nullptr, 0);
            ID3D11Buffer* noSO[4] = {};
            UINT          soOff[4] = {};
            c->SOSetTargets(4, noSO, soOff);
            c->SetPredication(nullptr, FALSE);
        }
        void Restore(ID3D11DeviceContext* c)
        {
            c->IASetPrimitiveTopology(topo);
            c->IASetInputLayout(layout);
            c->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
            c->IASetIndexBuffer(ib, ibFormat, ibOffset);
            c->VSSetShader(vs, nullptr, 0);
            c->PSSetShader(ps, nullptr, 0);
            c->GSSetShader(gs, nullptr, 0);
            c->HSSetShader(hs, nullptr, 0);
            c->DSSetShader(ds, nullptr, 0);
            c->PSSetShaderResources(0, kSrvSlots, psSrvs);
            c->VSSetShaderResources(0, kSrvSlots, vsSrvs);
            c->PSSetSamplers(0, 1, &sampler);
            c->PSSetConstantBuffers(0, 1, &psCb);
            c->VSSetConstantBuffers(0, 1, &vsCb);
            c->OMSetBlendState(blend, blendFactor, sampleMask);
            c->OMSetDepthStencilState(depth, stencilRef);
            c->RSSetState(rs);
            c->CSSetShader(cs, nullptr, 0);
            UINT soOff[4] = {};
            c->SOSetTargets(4, soTargets, soOff);
            c->SetPredication(predicate, predicateValue);
            // A UAV sits in an OM slot above the render targets, and the 8-slot OMSetRenderTargets clears
            // every UAV: with one bound, set the targets and the UAVs together (the UAV range must start past
            // the last target, and the counters keep their values with -1).
            UINT rtCount = 0, uavEnd = 0;
            for (UINT i = 0; i < 8; ++i) if (rtvs[i]) rtCount = i + 1;
            for (UINT i = 0; i < D3D11_PS_CS_UAV_REGISTER_COUNT; ++i) if (uavs[i]) uavEnd = i + 1;
            if (uavEnd > rtCount) {
                UINT keep[D3D11_PS_CS_UAV_REGISTER_COUNT];
                for (auto& k : keep) k = static_cast<UINT>(-1);
                c->OMSetRenderTargetsAndUnorderedAccessViews(rtCount, rtvs, dsv, rtCount, uavEnd - rtCount,
                                                             uavs + rtCount, keep);
            } else {
                c->OMSetRenderTargets(8, rtvs, dsv);
            }
            // A count of 0 restores "none bound" (the driver binds one scissor rect per scissored draw).
            c->RSSetViewports(vpCount, vpCount ? vps : nullptr);
            c->RSSetScissorRects(scCount, scCount ? scs : nullptr);
            // Release the refs the Get* calls added.
            if (layout) layout->Release();
            if (vb) vb->Release();
            if (ib) ib->Release();
            if (vs) vs->Release();
            if (ps) ps->Release();
            if (gs) gs->Release();
            if (hs) hs->Release();
            if (ds) ds->Release();
            for (auto* r : psSrvs) if (r) r->Release();
            for (auto* r : vsSrvs) if (r) r->Release();
            if (sampler) sampler->Release();
            if (psCb) psCb->Release();
            if (vsCb) vsCb->Release();
            if (blend) blend->Release();
            if (depth) depth->Release();
            if (rs) rs->Release();
            for (auto* r : rtvs) if (r) r->Release();
            if (dsv) dsv->Release();
            for (auto* u : uavs) if (u) u->Release();
            if (cs) cs->Release();
            for (auto* t : soTargets) if (t) t->Release();
            if (predicate) predicate->Release();
        }
    };

    // Composite one textured quad over the backbuffer. `srv` is the view's
    // pixels — the CPU path's uploaded texture (uv 0..1) or the GPU path's
    // render-target texture (uv from RenderTarget::uv_coords, since the target
    // may be padded). Pipeline state save/restore is the CALLER's job.
    static void DrawQuadTo(ID3D11RenderTargetView* rtv, float bw, float bh, ID3D11ShaderResourceView* srv,
                           float u0, float v0, float u1, float v1,
                           float dx, float dy, float dw, float dh,
                           const float* cutUV);

    // Whether a D3D object lives on the device we draw with (ours, or the engine's, which it stands in for).
    static bool OnOurDeviceObject(ID3D11DeviceChild* obj)
    {
        ID3D11Device* dev = nullptr;
        obj->GetDevice(&dev);
        if (!dev) return false;
        bool same = dev == s_device || dev == EngineDevice();
        if (!same && s_device) {
            IUnknown *a = nullptr, *b = nullptr;
            dev->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&a));
            s_device->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&b));
            same = a && a == b;
            if (a) a->Release();
            if (b) b->Release();
        }
        dev->Release();
        return same;
    }

    // The engine's framebuffer render target and its size, or null when it is missing or not on our device.
    // Engine-owned: never Released.
    static ID3D11RenderTargetView* EngineFramebufferRtv(float& w, float& h)
    {
        auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
        if (!data) return nullptr;
        const auto& fb = data->renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
        auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(fb.RTV);
        if (!rtv || !OnOurDeviceObject(rtv)) return nullptr;
        // The view's own resource, not fb.texture: a plugin may point the view elsewhere and leave the texture.
        ID3D11Resource* res = nullptr;
        rtv->GetResource(&res);
        ID3D11Texture2D* tex = nullptr;
        if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
        if (res) res->Release();
        if (!tex) return nullptr;
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        tex->Release();
        if (!td.Width || !td.Height) return nullptr;
        w = static_cast<float>(td.Width);
        h = static_cast<float>(td.Height);
        return rtv;
    }

    // Whether GetBuffer added a reference to `first`: a second GetBuffer that raises the count by one did.
    // The second call's reference is dropped here when there is one. Measured once per swapchain (render thread):
    // a wrapper's GetBuffer does not change its semantics, and fewer reads leave less room for a race.
    static bool GetBufferGaveReference(IDXGISwapChain* sc, ID3D11Texture2D* first)
    {
        static IDXGISwapChain* s_probedChain = nullptr;
        static bool            s_probedOwned = false;
        if (sc == s_probedChain) return s_probedOwned;
        first->AddRef();
        const ULONG before = first->Release();
        ID3D11Texture2D* second = nullptr;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&second))) || !second)
            return false;   // cannot tell: leaking a reference beats freeing the game's buffer
        bool owned = true;
        if (second != first) {
            second->Release();   // a fresh object per call is a fresh reference
        } else {
            second->AddRef();
            const ULONG after = second->Release();
            owned = after == before + 1;
            if (owned) second->Release();
        }
        s_probedChain = sc;
        s_probedOwned = owned;
        SKSE::log::info("Magelight: the swapchain's GetBuffer {} a reference", owned ? "adds" : "does not add");
        return owned;
    }

    static void WarnNothingToDrawOn(const char* why)
    {
        static std::atomic<bool> s_warned{ false };
        if (!s_warned.exchange(true))
            SKSE::log::warn("Magelight: nothing to draw the overlay on ({}) - pages run but are not shown", why);
    }

    static void DrawOverlay(IDXGISwapChain* sc, ID3D11ShaderResourceView* srv,
                            float u0, float v0, float u1, float v1,
                            float dx, float dy, float dw, float dh,
                            const float* cutUV = nullptr)   // {x0,y0,x1,y1} in this quad's uv space
    {
        DXGI_SWAP_CHAIN_DESC scd{};
        if (FAILED(sc->GetDesc(&scd))) return;
        const float bw = static_cast<float>(scd.BufferDesc.Width);
        const float bh = static_cast<float>(scd.BufferDesc.Height);
        if (bw <= 0.0f || bh <= 0.0f || !srv) return;
        const bool untrusted = s_untrustedChain.load();
        if (untrusted && !REL::Module::IsVR()) {
            // The engine's own framebuffer view: what the game's UI draws into this frame (Community Shaders points
            // it at its UI buffer while frame generation composites), so the overlay is UI to whatever presents it.
            float tw = 0.0f, th = 0.0f;
            if (ID3D11RenderTargetView* engineRtv = EngineFramebufferRtv(tw, th)) {
                const float sx = tw / bw, sy = th / bh;
                DrawQuadTo(engineRtv, tw, th, srv, u0, v0, u1, v1, dx * sx, dy * sy, dw * sx, dh * sy, cutUV);
                return;
            }
        }
        ID3D11Texture2D* back = nullptr;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) {
            WarnNothingToDrawOn("the swapchain gave no back buffer");
            return;
        }
        // A wrapper may hand its buffer out without a reference (Community Shaders before 1.8.4): Release only
        // what was given.
        const bool owned = !untrusted || GetBufferGaveReference(sc, back);
        if (untrusted && !OnOurDeviceObject(back)) {
            if (owned) back->Release();
            WarnNothingToDrawOn("the back buffer is on another device");
            return;
        }
        ID3D11RenderTargetView* rtv = nullptr;
        const HRESULT rtvHr = s_device->CreateRenderTargetView(back, nullptr, &rtv);
        if (owned) back->Release();
        if (FAILED(rtvHr)) return;
        DrawQuadTo(rtv, bw, bh, srv, u0, v0, u1, v1, dx, dy, dw, dh, cutUV);
        rtv->Release();
    }

    // One textured quad into `rtv`, a bw x bh target. Pipeline state save/restore is the CALLER's job.
    static void DrawQuadTo(ID3D11RenderTargetView* rtv, float bw, float bh, ID3D11ShaderResourceView* srv,
                           float u0, float v0, float u1, float v1,
                           float dx, float dy, float dw, float dh,
                           const float* cutUV)
    {
        if (!rtv || bw <= 0.0f || bh <= 0.0f || !srv) return;

        // Quad in NDC at (dx, dy), dw x dh pixels.
        const float x0 = -1.0f + 2.0f * (dx / bw);
        const float y0 =  1.0f - 2.0f * (dy / bh);
        const float x1 = -1.0f + 2.0f * ((dx + dw) / bw);
        const float y1 =  1.0f - 2.0f * ((dy + dh) / bh);
        const Vertex quad[4] = {
            { x0, y0, u0, v0 },
            { x1, y0, u1, v0 },
            { x0, y1, u0, v1 },
            { x1, y1, u1, v1 },
        };
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(s_context->Map(s_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
        std::memcpy(mapped.pData, quad, sizeof(quad));
        s_context->Unmap(s_vb, 0);

        const UINT stride = sizeof(Vertex), offset = 0;
        const FLOAT bf[4] = { 0, 0, 0, 0 };
        D3D11_VIEWPORT vp{ 0, 0, bw, bh, 0.0f, 1.0f };
        s_context->OMSetRenderTargets(1, &rtv, nullptr);
        s_context->RSSetViewports(1, &vp);
        s_context->RSSetState(nullptr);
        s_context->OMSetBlendState(s_blend, bf, 0xFFFFFFFF);
        s_context->OMSetDepthStencilState(nullptr, 0);
        s_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        s_context->IASetInputLayout(s_layout);
        s_context->IASetVertexBuffers(0, 1, &s_vb, &stride, &offset);
        s_context->VSSetShader(s_vs, nullptr, 0);
        s_context->PSSetShader(s_ps, nullptr, 0);
        s_context->GSSetShader(nullptr, nullptr, 0);
        s_context->HSSetShader(nullptr, nullptr, 0);
        s_context->DSSetShader(nullptr, nullptr, 0);
        s_context->PSSetShaderResources(0, 1, &srv);
        s_context->PSSetSamplers(0, 1, &s_sampler);
        {
            const float none[4] = { 0, 0, 0, 0 };
            D3D11_MAPPED_SUBRESOURCE cm{};
            if (SUCCEEDED(s_context->Map(s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
                std::memcpy(cm.pData, cutUV ? cutUV : none, sizeof(none));
                s_context->Unmap(s_cb, 0);
            }
            s_context->PSSetConstantBuffers(0, 1, &s_cb);
        }
        s_context->Draw(4, 0);
    }

    // VR copy pass (docs/VR_PRESENTER.md §2): draw one presented view into a
    // HOST-OWNED render target at 1:1 — resolves the driver target's uv
    // padding, applies the cutout, and (straightAlpha) divides the
    // premultiplied colour out. No blending: the target is cleared first and
    // the quad covers it. Brackets its own pipeline state; Ultralight thread.
    void CopyViewToTarget(const PresentedView& pv, ID3D11RenderTargetView* rtv, int w, int h, bool straightAlpha,
                          const CursorMark* marks, int markCount)
    {
        if (!rtv || !pv.srv || w <= 0 || h <= 0 || !s_context) return;
        StateBackup backup;
        backup.Capture(s_context);
        StateBackup::Neutralize(s_context);

        const float clear[4] = { 0, 0, 0, 0 };
        s_context->ClearRenderTargetView(rtv, clear);
        const Vertex quad[4] = {
            { -1.0f,  1.0f, pv.u0, pv.v0 },
            {  1.0f,  1.0f, pv.u1, pv.v0 },
            { -1.0f, -1.0f, pv.u0, pv.v1 },
            {  1.0f, -1.0f, pv.u1, pv.v1 },
        };
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(s_context->Map(s_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, quad, sizeof(quad));
            s_context->Unmap(s_vb, 0);
            const UINT stride = sizeof(Vertex), offset = 0;
            D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
            s_context->OMSetRenderTargets(1, &rtv, nullptr);
            s_context->RSSetViewports(1, &vp);
            s_context->RSSetState(nullptr);
            s_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);   // overwrite, no blend
            s_context->OMSetDepthStencilState(nullptr, 0);
            s_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            s_context->IASetInputLayout(s_layout);
            s_context->IASetVertexBuffers(0, 1, &s_vb, &stride, &offset);
            s_context->VSSetShader(s_vs, nullptr, 0);
            s_context->PSSetShader((straightAlpha && s_psStraight) ? s_psStraight : s_ps, nullptr, 0);
            s_context->GSSetShader(nullptr, nullptr, 0);
            s_context->HSSetShader(nullptr, nullptr, 0);
            s_context->DSSetShader(nullptr, nullptr, 0);
            ID3D11ShaderResourceView* srv = pv.srv;
            s_context->PSSetShaderResources(0, 1, &srv);
            s_context->PSSetSamplers(0, 1, &s_sampler);
            const float none[4] = { 0, 0, 0, 0 };
            D3D11_MAPPED_SUBRESOURCE cm{};
            if (SUCCEEDED(s_context->Map(s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
                std::memcpy(cm.pData, pv.hasCutout ? pv.cutUV : none, sizeof(none));
                s_context->Unmap(s_cb, 0);
            }
            s_context->PSSetConstantBuffers(0, 1, &s_cb);
            s_context->Draw(4, 0);

            // Cursor marks (VR laser hits): the host's own cursor sprite, hotspot
            // on the hit pixel, composited over the (now straight-alpha) page.
            if (marks && markCount > 0 && s_cursorSrv) {
                // VR pointer sprite: a FRACTION of the panel, not the flat
                // cursor's 1080p-relative size. The old 24px floor was ~10% of a
                // 240px panel and read as a huge arrow stuck on the beam.
                // The laser-end pointer: a centred dot by default (0.26.12),
                // the cursor art (arrow / custom PNG, its own hotspot) if
                // "cursorDot": false or the dot texture failed.
                const bool dot = VR::CursorDot() && s_dotSrv;
                const float ch = std::clamp(static_cast<float>(h) * VR::CursorScale(), dot ? 4.0f : 8.0f, 32.0f);
                float cw = ch, hotX = 0.0f, hotY = 0.0f;
                ID3D11ShaderResourceView* markSrv = dot ? DotSrvFor(VrCursorTintFor(pv.id)) : s_cursorSrv;
                // 0.31.0: the view's own image (sized like the arrow, from the panel; its hotspot kept).
                const std::shared_ptr<CursorImage> viewImg = dot ? nullptr : VrCursorImageFor(pv.id);
                ID3D11ShaderResourceView* viewSrv = viewImg ? CursorImageSrv(viewImg) : nullptr;
                if (dot) {
                    hotX = hotY = 0.5f;
                } else if (viewSrv) {
                    cw = ch * (static_cast<float>(viewImg->w) / viewImg->h);
                    hotX = viewImg->hotX / viewImg->w;
                    hotY = viewImg->hotY / viewImg->h;
                    markSrv = viewSrv;
                } else if (s_cursorCustom && s_cursorImgH > 0) {
                    cw = ch * (static_cast<float>(s_cursorImgW) / s_cursorImgH);
                    hotX = s_cursorHotX; hotY = s_cursorHotY;
                } else {
                    cw = ch * (static_cast<float>(kCursorW) / kCursorH);
                }
                const FLOAT bf[4] = { 0, 0, 0, 0 };
                s_context->OMSetBlendState((straightAlpha && s_blendStraight) ? s_blendStraight : s_blend, bf, 0xFFFFFFFF);
                s_context->PSSetShaderResources(0, 1, &markSrv);
                if (SUCCEEDED(s_context->Map(s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
                    std::memcpy(cm.pData, none, sizeof(none));
                    s_context->Unmap(s_cb, 0);
                }
                for (int i = 0; i < markCount; ++i) {
                    const float px = marks[i].u * w - hotX * cw;
                    const float py = marks[i].v * h - hotY * ch;
                    const float x0 = -1.0f + 2.0f * (px / w), y0 = 1.0f - 2.0f * (py / h);
                    const float x1 = -1.0f + 2.0f * ((px + cw) / w), y1 = 1.0f - 2.0f * ((py + ch) / h);
                    const Vertex cq[4] = { { x0, y0, 0, 0 }, { x1, y0, 1, 0 }, { x0, y1, 0, 1 }, { x1, y1, 1, 1 } };
                    if (SUCCEEDED(s_context->Map(s_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                        std::memcpy(mapped.pData, cq, sizeof(cq));
                        s_context->Unmap(s_vb, 0);
                        s_context->Draw(4, 0);
                    }
                }
            }
            // Unbind the target so the driver never samples a bound RT.
            ID3D11RenderTargetView* nullRtv = nullptr;
            s_context->OMSetRenderTargets(1, &nullRtv, nullptr);
        }
        backup.Restore(s_context);
    }

    // ── Texture-backed images: render-thread materialization ────────────────
    // Runs every frame right before Renderer::Update(): registers pending
    // images with the driver + ImageSourceProvider, applies SRV re-points,
    // tombstones unregistered ones, and Invalidate()s dirty ones (Ultralight
    // only repaints an image that was invalidated).
    static void MaterializeImages()
    {
        if (!s_gpuActive || !s_gpuRegExt) return;
        std::lock_guard<PageThread::OwnedMutex> lk(s_imagesMutex);
        for (auto& ip : s_images) {
            MlImage& im = *ip;
            if (im.removed) continue;
            if (!im.texId) {
                im.texId = s_gpuRegExt(s_gpu, im.srv);
                if (!im.texId) {
                    SKSE::log::error("Magelight: image '{}' — driver refused the external texture", im.name);
                    im.removed = true;
                    continue;
                }
                im.src = ultralight::ImageSource::CreateFromTexture(
                    im.w, im.h, im.texId, ultralight::Rect{ 0.0f, 0.0f, 1.0f, 1.0f });
                ultralight::ImageSourceProvider::instance().AddImageSource(im.name.c_str(), im.src);
                SKSE::log::info("Magelight: image '{}' registered (driver texture {}, {}x{}) -> {}",
                    im.name, im.texId, im.w, im.h, im.url);
            }
            if (im.pendingSrv) {
                if (s_gpuSetExtSrv(s_gpu, im.texId, im.pendingSrv)) {
                    im.srv->Release();
                    im.srv = im.pendingSrv;
                } else {
                    im.pendingSrv->Release();
                }
                im.pendingSrv = nullptr;
                im.dirty = true;
            }
            if (im.unregister) {
                ultralight::ImageSourceProvider::instance().RemoveImageSource(im.name.c_str());
                im.src = nullptr;
                // Tombstone: the driver entry and our SRV ref stay — a page
                // element may still resolve to this id, and a missing id leaves
                // whatever SRV was last bound in place.
                im.removed = true;
                SKSE::log::info("Magelight: image '{}' unregistered", im.name);
                continue;
            }
            if (im.dirty && im.src) {
                im.src->Invalidate();
                im.dirty = false;
            }
        }
    }

    // ── ImageSource probe (Magelight.json "imageProbe": true) ───────────────
    // A 512x512 dynamic texture the host repaints on the CPU every frame
    // (moving stripes + a red disc), registered as image "probe" and placed
    // eight ways by views/probe/index.html. Proves the whole chain without a
    // consumer: driver id, .imgsrc resolution, per-frame Invalidate, and
    // which CSS features the GPU path composites (clip/scroll/transform/
    // opacity) versus which fall back to a bitmap it doesn't have (filter,
    // canvas). Render thread.
    static ID3D11Texture2D*          s_probeTex = nullptr;
    static ID3D11ShaderResourceView* s_probeSrv = nullptr;
    static ImageId                   s_probeImage = 0;
    static std::uint32_t             s_probeFrame = 0;
    static constexpr std::uint32_t   kProbeSize = 512;

    static void TickImageProbe()
    {
        if (!s_imageProbe || !s_gpuActive || !s_device) return;
        if (!s_probeTex) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = kProbeSize;
            td.Height = kProbeSize;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(s_device->CreateTexture2D(&td, nullptr, &s_probeTex)) ||
                FAILED(s_device->CreateShaderResourceView(s_probeTex, nullptr, &s_probeSrv))) {
                SKSE::log::error("Magelight: image probe — texture creation failed; probe disabled");
                s_imageProbe = false;
                return;
            }
            s_probeImage = RegisterTextureImage("probe", s_probeSrv, kProbeSize, kProbeSize);
            SKSE::log::info("Magelight: image probe texture queued as image {}", s_probeImage);
            if (!s_probeImage) {
                s_imageProbe = false;
                return;
            }
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(s_context->Map(s_probeTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            const std::uint32_t f = s_probeFrame++;
            const float cx = kProbeSize * (0.5f + 0.35f * std::cos(f * 0.020f));
            const float cy = kProbeSize * (0.5f + 0.35f * std::sin(f * 0.031f));
            for (std::uint32_t y = 0; y < kProbeSize; ++y) {
                auto* row = reinterpret_cast<std::uint32_t*>(
                    static_cast<std::uint8_t*>(m.pData) + y * m.RowPitch);
                for (std::uint32_t x = 0; x < kProbeSize; ++x) {
                    const bool stripe = (((x + y + f * 2) / 32) & 1) != 0;
                    std::uint32_t c = stripe ? 0xFF3A2D17u : 0xFFEFE3C8u;  // ink / parchment, opaque
                    const float dx = static_cast<float>(x) - cx;
                    const float dy = static_cast<float>(y) - cy;
                    if (dx * dx + dy * dy < 60.0f * 60.0f) c = 0xFFB33A2Fu;  // the red disc
                    row[x] = c;
                }
            }
            s_context->Unmap(s_probeTex, 0);
        }
        InvalidateImage(s_probeImage);
    }

    static bool StartPageThread();        // fwd (the page thread, below)
    static bool TakePageInitFailure();    // fwd

    static void EnsureRenderInit(IDXGISwapChain* sc)
    {
        if (s_renderInit || s_renderDead.load()) return;
        s_renderInit = true;
        s_ulThreadId = GetCurrentThreadId();
        const bool deviceReady = CreateDeviceResources(sc);
        if (deviceReady && s_pageThreadMode.load() == PageThread::Mode::Wanted) {
            // Started: the page thread makes the renderer itself and raises s_rendererUp. Refused: the mode is Off and
            // the next frame starts Ultralight here, as without the setting (FrameWork: the fiber, or the game stack).
            if (!StartPageThread()) s_renderInit = false;
            return;
        }
        if (!deviceReady || !CreateUltralight(s_context)) {
            NoteRenderFailure("render-thread init failed");   // a no-op when a step already said why
            s_renderDead.store(true);
            SKSE::log::error("Magelight: render-thread init failed — overlay disabled");
            return;
        }
        SKSE::log::info("Magelight: render-thread init complete (owning thread {})", s_ulThreadId);
        s_rendererUp.store(true);
        // VR presenter (docs/VR_PRESENTER.md): same thread, after the device
        // resources exist. Not a VR runtime = dormant, one log line.
        VR::Init(s_device, s_context);
    }

    // SEH telemetry (0.3.1 diagnostics): a fault anywhere in our frame or
    // input path becomes a LOG LINE + graceful overlay shutdown instead of a
    // silent CTD (crash loggers routinely miss faults raised inside foreign
    // frames like a Present hook or a window proc).
    static LONG LogSehAndDisable(const char* where, unsigned long code, void* addr)
    {
        SKSE::log::error(
            "Magelight: SEH exception 0x{:08X} at {} in {} — overlay disabled, UI mode will drop (UI mode {}: {})",
            code, addr, where, s_focused.load() ? "on" : "off, last", static_cast<const char*>(s_uiModeLabel));
        NoteRenderFailure("an exception in the frame or input path");   // a static literal: no allocation in a handler
        s_renderDead.store(true);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // ── Dev loop: Web Inspector ───────────────────────────────────────────
    // The inspector is an ordinary Ultralight view WE host: Ultralight asks
    // for one (OnCreateInspectorView) and loads file:///inspector/Main.html
    // from the FileSystem root, so the SDK's inspector/ folder ships under the
    // runtime dir. It takes UI mode while open (input routes to one view), so
    // the inspector toggle flips focus between the page and its inspector; the page keeps
    // running underneath, and the console can drive it.
    static ultralight::RefPtr<ultralight::View> HostInspectorView(ultralight::View* caller)
    {
        ViewId page = 0;
        std::filesystem::path pageRoot;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            for (auto& vp : s_views) if (vp->ul && vp->ul.get() == caller) { page = vp->id; pageRoot = vp->root; break; }
        }
        int bw = 0, bh = 0;
        GetDisplaySize(bw, bh);
        if (bw <= 0 || bh <= 0 || !s_ulRenderer) return nullptr;
        ultralight::ViewConfig vc;
        vc.is_accelerated = s_gpuActive;
        vc.is_transparent = false;
        vc.clipboard_read_policy = ultralight::ClipboardReadPolicy::Deny;
        vc.enable_compositor = kEnableCompositor;
        vc.initial_focus = false;   // View focus comes with UI mode, which the inspector takes as it opens (invariant 25)
        const int h = bh / 2;
        auto ul = s_ulRenderer->CreateView(static_cast<std::uint32_t>(bw), static_cast<std::uint32_t>(h), vc, nullptr);
        if (!ul) return nullptr;
        // The network listener at File level; the inspector frontend is the one
        // page served with no CSP (MlFileSystem::OpenFile), a dev surface.
        ul->set_network_listener(&s_networkListener.value);
        auto v = std::make_unique<MlView>();
        v->id = s_nextViewId.fetch_add(1);
        v->htmlPath = "<inspector>";
        v->root = pageRoot;
        v->isInspector = true;
        v->x = 0; v->y = bh - h; v->w = bw; v->h = h;
        v->visible = true;
        v->layer = 3;   // System
        v->order = s_nextViewOrder.fetch_add(1);
        v->ul = ul;
        const ViewId id = v->id;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            s_views.push_back(std::move(v));
            SortViewsLocked();
        }
        s_inspectorOf[page] = id;
        s_inspectorVisibleFor.store(page);
        SKSE::log::info("Magelight[dev]: inspector view {} hosted for view {} ({}x{}, lower half)", id, page, bw, h);
        return ul;
    }

    // Render thread, after MaterializeViews (the page must have its View).
    static void ProcessInspectorRequest()
    {
        if (const ViewId hidePage = static_cast<ViewId>(s_inspectorHideRequest.exchange(0))) {   // 0.28.0 ShowInspector(false)
            if (auto it = s_inspectorOf.find(hidePage); it != s_inspectorOf.end()) {
                ShowView(it->second, false);
                SwitchUIModeView(hidePage);
                s_inspectorVisibleFor.store(0);
            }
        }
        const ViewId target = static_cast<ViewId>(s_inspectorRequest.exchange(0));
        if (!target) return;
        for (const auto& [page, insp] : s_inspectorOf) {
            if (insp != target) continue;
            // A toggle on the inspector itself: back to the page, inspector hidden.
            ShowView(insp, false);
            SwitchUIModeView(page);
            s_inspectorVisibleFor.store(0);
            SKSE::log::info("Magelight[dev]: inspector {} hidden — focus back to view {}", insp, page);
            return;
        }
        if (auto it = s_inspectorOf.find(target); it != s_inspectorOf.end() && IsViewValid(it->second)) {
            ShowView(it->second, true);
            SwitchUIModeView(it->second);
            s_inspectorVisibleFor.store(target);
            return;
        }
        if (!InspectorAvailable()) {
            SKSE::log::warn("Magelight[dev]: inspector assets missing — expected {}", Magelight::SysInfo::ForLog(s_runtimeDir / "inspector" / "Main.html"));
            return;
        }
        ultralight::RefPtr<ultralight::View> ul;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            if (MlView* v = FindViewLocked(target)) ul = v->ul;
        }
        if (!ul) return;
        ul->CreateLocalInspectorView();   // synchronous: OnCreateInspectorView -> HostInspectorView
        if (auto it = s_inspectorOf.find(target); it != s_inspectorOf.end()) SwitchUIModeView(it->second);
    }

    // ── Memory lines (1.5.0) ──────────────────────────────────────────────
    // Ultralight's footprint in Magelight.log: periodically (memoryLogMinutes), ~5 s after UI mode ends (at most one
    // a minute) and on request (RequestMemoryReport, at most one every 10 s). GetMemoryUsage costs about a
    // millisecond and reclaims nothing; it runs after the frame's stage timing, so the line carries its own time.
    static std::atomic<bool> s_memReportRequested{ false };     // any thread
    // The last UI-mode exit in steady-clock ms, 0 = none: the game thread sets it (GameBookkeeping), the memory
    // report reads it on Ultralight's thread (the page thread with pageThread).
    static std::atomic<long long> s_memAfterUiMs{ 0 };

    static long long SteadyNowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static double MsBetween(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
    {
        return std::chrono::duration<double, std::milli>(b - a).count();
    }

    // Ultralight's thread, in its frame (invariants 1 and 27) and outside s_viewsMutex: GetMemoryUsage runs no page code, but
    // it is an Ultralight call like any other.
    static void TickMemoryReport()
    {
        using Clock = std::chrono::steady_clock;
        static Clock::time_point nextPeriodic{}, lastRequested{}, lastAfterUi{}, nextMsaaCheck{};
        static bool rawLogged = false, msaaNoted = false;
        const auto now = Clock::now();
        if (nextPeriodic == Clock::time_point{}) nextPeriodic = now + std::chrono::seconds(60);   // after the first loads

        // The first antialiased target is worth a line of its own: the startup line can only say MSAA is offered.
        if (!msaaNoted && s_gpuActive && s_gpu && s_gpuStats && s_msaaInEffect > 1 && now >= nextMsaaCheck) {
            nextMsaaCheck = now + std::chrono::seconds(1);
            MgGpuStats st{};
            st.size = sizeof(st);
            if (s_gpuStats(s_gpu, &st) && st.antialiasedCreated > 0) {
                msaaNoted = true;
                SKSE::log::info("Magelight: Ultralight asked for an antialiased page target - MSAA {}x in use", s_msaaInEffect);
            }
        }

        const char* why = nullptr;
        bool full = false;
        if (s_memReportRequested.load() && s_memReportRequested.exchange(false)
            && (lastRequested == Clock::time_point{} || now - lastRequested >= std::chrono::seconds(10))) {
            why = "on request";
            full = true;
            lastRequested = now;
        }
        long long afterUi = s_memAfterUiMs.load();
        if (afterUi && SteadyNowMs() - afterUi >= 5000 && s_memAfterUiMs.compare_exchange_strong(afterUi, 0)) {
            if (lastAfterUi == Clock::time_point{} || now - lastAfterUi >= std::chrono::seconds(60)) {
                lastAfterUi = now;
                if (!why) {
                    why = "after UI mode";
                    full = s_devMode.load();
                }
            }
        }
        if (const int minutes = s_memLogMinutes.load(); minutes > 0 && now >= nextPeriodic) {
            nextPeriodic = now + std::chrono::minutes(minutes);
            if (!why) why = "periodic";
        }
        if (!why || !s_ulRenderer) return;

        int live = 0, dormant = 0, notLoaded = 0;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            for (const auto& vp : s_views) {
                if (vp->ul) ++live;
                else if (vp->dormant) ++dormant;
                else ++notLoaded;
            }
        }
        const auto t0 = Clock::now();
        const std::string report = s_ulRenderer->GetMemoryUsage().utf8().data();
        const double reportMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        const MemoryReport::Figures f = MemoryReport::Parse(report);

        // Whether any target took MSAA is the driver's to say: Ultralight's render-target bytes include the
        // multisample surfaces but not who asked for them.
        std::string msaa;
        if (s_gpuActive) {
            MgGpuStats st{};
            st.size = sizeof(st);
            if (s_msaaInEffect <= 1) msaa = "; MSAA off";
            else if (!s_gpuStats || !s_gpuStats(s_gpu, &st)) msaa = "; MSAA ?";
            else if (st.antialiasedCreated == 0) msaa = fmt::format("; MSAA {}x offered, no page target asked for it", s_msaaInEffect);
            else
                msaa = fmt::format("; MSAA {}x on {} of {} page targets ({} of the render targets), {} antialiased targets so far",
                                   s_msaaInEffect, st.msaaSurfaces, st.renderTargets, MemoryReport::Size(static_cast<double>(st.msaaBytes)),
                                   st.antialiasedCreated);
        }
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        const bool haveSys = GlobalMemoryStatusEx(&ms) != FALSE;
        constexpr double kGB = 1073741824.0;
        SKSE::log::info("Magelight: memory ({}) - {}; views {} live, {} hibernated, {} not loaded{}{}; read in {:.1f} ms",
                        why, MemoryReport::Line(f), live, dormant, notLoaded, msaa,
                        haveSys ? fmt::format("; system RAM {:.1f} of {:.1f} GB free, commit {:.1f} GB free",
                                              ms.ullAvailPhys / kGB, ms.ullTotalPhys / kGB, ms.ullAvailPageFile / kGB)
                                : std::string(),
                        reportMs);
        // A later SDK that reshaped the report gets the raw text once, so the parser can follow it.
        const bool reshaped = !rawLogged && MemoryReport::Missing(f) >= 5;
        if (full || reshaped) {
            if (reshaped) rawLogged = true;
            SKSE::log::info("Magelight: Ultralight memory report ({})\n{}", why, SysInfo::RedactProfile(report));
        }
    }

    // ── FrameWork in parts ───────────────────────────────────────────────────
    // In the frame (the default) FrameWork runs them in this order on the game's main thread, on the page fiber.

    // Page work before the paint: fullscreen views follow the back buffer, then hibernation, rebuilds,
    // materialization, resizes, hot reload, the inspector, lifecycle, listener shims, the first visibility sync and
    // the input, edit-state, tooltip and bridge drains. Ultralight's thread.
    static void PageWorkBegin(float bw, float bh)
    {
        if (!s_pendingSettles.empty()) {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            for (ultralight::View* caller : s_pendingSettles) ApplyPageSettledLocked(caller);
            s_pendingSettles.clear();
        }
        if (s_pageCursorResetPending.exchange(false)) {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            for (auto& vp : s_views) vp->pageCursor = 0;
        }
        // Fullscreen views track the backbuffer: sized here BEFORE
        // materialization (a 0x0 view never materializes), re-Resized via
        // the boundsDirty path if the backbuffer ever changes.
        if (bw > 0 && bh > 0) {
            // In VR the backbuffer is the desktop mirror window, whose aspect
            // is unrelated to the panel in the headset — take the configured
            // panel resolution there instead (see VR::Settings::panelWidth).
            int ibw = static_cast<int>(bw), ibh = static_cast<int>(bh);
            int panelW = 0, panelH = 0;
            if (VR::PanelSizeForFullscreen(panelW, panelH)) { ibw = panelW; ibh = panelH; }
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            for (auto& vp : s_views) {
                MlView& v = *vp;
                if (!v.fullscreen || (v.w == ibw && v.h == ibh)) continue;
                v.w = ibw;
                v.h = ibh;
                if (v.ul) v.boundsDirty = true;
            }
        }

        HibernateIdleViews();
        ApplyPendingRebuilds();
        MaterializeViews();
        ApplyPendingResizes();
        Dev::Tick();                 // hot reload: queues ReloadView for changed folders
        ProcessInspectorRequest();   // inspector toggle (unbound for now)
        ApplyPendingLifecycle();
        // Listeners registered AFTER a page loaded (the OnDomReady pattern)
        // get their window shims here, a frame later — the listener shims
        // only; the page core stays as the page loaded it. Raw MlView*
        // snapshots are safe here because the registry is erased ONLY by
        // ApplyPendingLifecycle, on this thread,
        // which already ran this frame — nothing erases between the collect
        // and the install. InstallBridgeShims takes the lock itself, so
        // collect first, install outside.
        {
            std::vector<MlView*> dirty;
            {
                std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
                for (auto& vp : s_views)
                    if (vp->ul && vp->shimsDirty) dirty.push_back(vp.get());
            }
            for (MlView* v : dirty) InstallBridgeShims(*v, false);
        }
        // Shows and hides made since the last frame: the UI-mode page is visible when its input arrives, and its
        // visibilitychange handler runs in this frame's Update.
        SyncViewVisibility();
        DrainInputQueue(bw, bh);
        DrainEditableStates();   // before the bridge drain and the second sync: a keyboard raised here shows this frame
        NoteEditorComposition();   // the Escape guard's view of the field's composition (window thread reads it)
        DrainTooltips(bh);
        DrainBridgeQueues();
    }

    // The game thread's bookkeeping each frame: the cursor confine, the focus menu's close check, the cursor re-hides
    // and samples, the world-freeze reconcile, the controls hold's release and the page-cursor reset. Engine state is
    // read on the game thread or reached through GameTask::Post.
    static void GameBookkeeping()
    {
        // The CursorMenu's kShow processes a frame or two after SetUIMode, and its movie only exists once it has:
        // hide the vanilla cursor again just after and once the menu has settled.
        // About 30 frames after an entry or exit, sample the Windows cursor (SampleCursorState).
        static int uiFrames = 0;
        static int offFrames = -1;   // frames since the last exit; -1 = no exit sample pending
        ConfineCursor(s_focused.load());
        if (s_focused.load()) {
            if (s_focusMenuHideSeen.load()) CheckFocusMenuClosed();
            s_memAfterUiMs.store(0);   // back in UI mode: the after-UI memory line waits for this exit
            ++uiFrames;
            offFrames = -1;
            // The Cursor Menu reopened (MenuSink): its new movie is visible until hidden, at once and for a few
            // frames in case the movie loads after the open.
            if (s_cursorRehideFrames.load() > 0 && s_cursorRehideFrames.fetch_sub(1) > 0) {
                GameTask::Post([]() { if (s_focused.load()) HideVanillaCursor(true); });
            }
            if (uiFrames == 3 || uiFrames == 30) {
                const bool sample = uiFrames == 30;
                GameTask::Post([sample]() {
                    if (!s_focused.load()) return;   // an exit landed first and restored the cursor movie
                    // Read before the hide, so the line can catch the sprite shown again since frame 3.
                    if (sample) LogMenusOverPages(true);
                    HideVanillaCursor(true);
                    if (sample) SampleCursorState(true);
                });
            }
            // Every ~second: the menus we depend on are still up and the vanilla cursor still hidden (see
            // CursorMenuWatchdog). Game thread, cheap.
            if (uiFrames > 30 && (uiFrames % 60) == 0) {
                GameTask::Post([]() { CursorMenuWatchdog(); });
            }
            // A few times a second: drop the world freeze if the game runs under it (another mod cleared
            // the pause) and put it back once the pause returns.
            if ((uiFrames % 15) == 0 && FreezeWorldAvailable()) {
                GameTask::Post([]() { if (s_focused.load()) ApplyFreezeWorld("reconcile"); });
            }
        } else {
            s_cursorRehideFrames.store(0);
            if (const std::uint32_t gen = s_heldGen.load()) {
                static std::uint32_t seenGen = 0, postedGen = 0;
                static std::uint64_t upSinceMs = 0;   // 0 = a button of the hold is down
                if (gen != seenGen) { seenGen = gen; upSinceMs = 0; }
                if (gen != postedGen) {
                    const std::uint64_t now = GetTickCount64();
                    const VR::HeldButtons held = VR::HeldButtonsNow();
                    if ((held.runtime & s_heldRuntime.load()) | (held.engine & s_heldEngine.load()) |
                        (s_gamepadHeld.load() & s_heldPad.load()))
                        upSinceMs = 0;
                    else if (!upSinceMs) upSinceMs = now;
                    if (upSinceMs && now - upSinceMs >= kHeldReleaseSettleMs) {
                        postedGen = gen;
                        GameTask::Post([gen]() { EndControlsHold("controller buttons released", gen); });
                    }
                }
            }
            if (uiFrames > 0) {
                s_memAfterUiMs.store(SteadyNowMs());
                offFrames = 0;
                // A page that hid the pointer (cursor: none) must not keep it hidden into the next UI mode: forget
                // every page's cursor; each reports again on the first mouse move (the core re-sends it per move).
                // A registry the page thread holds is not waited for: its next PageWorkBegin does the reset.
                if (std::unique_lock<PageThread::OwnedMutex> lk(s_viewsMutex, std::try_to_lock); lk.owns_lock())
                    for (auto& vp : s_views) vp->pageCursor = 0;
                else
                    s_pageCursorResetPending.store(true);
            }
            uiFrames = 0;
            if (offFrames >= 0 && ++offFrames == 30) {
                offFrames = -1;
                GameTask::Post([]() {
                    if (!s_focused.load()) SampleCursorState(false);
                });
            }
        }
    }

    // How the composite draws a quad: over the back buffer now, queued for the UI pass, or both (FrameWork's `emit`).
    using EmitFn = std::function<void(ID3D11ShaderResourceView*, float, float, float, float, float, float, float, float,
                                      const float*)>;

    // One view's reading for every presenter, under s_viewsMutex on Ultralight's thread: false when this frame does
    // not draw it. Its SRV is resolved here (the GPU path) or its surface uploaded through `ctx` (the CPU path). With
    // `addRef` (a page-thread packet) the SRV gets a reference of its own and `ul` stays empty (MagelightPresenter.h).
    static bool CollectView(MlView& v, float bw, float bh, ID3D11DeviceContext* ctx, bool addRef, PresentedView& pv)
    {
        if (!v.visible || !v.ul || !v.ul->is_visible()) return false;   // what Render painted (FrameWork)
        float ex = 0, ey = 0;
        EffectivePos(v, bw, bh, ex, ey);
        // Presenter seam: the same reading the VR submitter gets.
        pv.id = v.id; pv.x = ex; pv.y = ey; pv.w = v.w; pv.h = v.h;
        if (!addRef) pv.ul = v.ul;
        pv.layer = v.layer; pv.isInspector = v.isInspector; pv.gpuPath = s_gpuActive;
        pv.clickThrough = v.clickThrough;   // B3: the laser skips click-through views like hitTest does
        pv.repainted = v.repaintedThisFrame;
        // Cutout: view pixels -> this quad's uv range (the GPU
        // target may be padded, so map through its uv rect).
        float cut[4] = { 0, 0, 0, 0 };
        const float* cutPtr = nullptr;
        if (v.cutW > 0 && v.cutH > 0 && v.w > 0 && v.h > 0) cutPtr = cut;
        auto mapCut = [&](float u0, float v0, float u1, float v1) {
            const float fx0 = v.cutX / v.w, fy0 = v.cutY / v.h;
            const float fx1 = (v.cutX + v.cutW) / v.w, fy1 = (v.cutY + v.cutH) / v.h;
            cut[0] = u0 + fx0 * (u1 - u0); cut[1] = v0 + fy0 * (v1 - v0);
            cut[2] = u0 + fx1 * (u1 - u0); cut[3] = v0 + fy1 * (v1 - v0);
        };
        // Mirror fit: a VR panel is sized for the headset, not
        // for the desktop window, so scale it to FIT the mirror
        // rather than drawing it 1:1 and cropping the right edge.
        float mx = static_cast<float>(ex), my = static_cast<float>(ey);
        float mw = static_cast<float>(v.w), mh = static_cast<float>(v.h);
        if (VR::IsLive() && v.fullscreen && bw > 0 && bh > 0 && v.w > 0 && v.h > 0 &&
            (v.w != static_cast<int>(bw) || v.h != static_cast<int>(bh))) {
            const float s = std::min(static_cast<float>(bw) / v.w,
                                     static_cast<float>(bh) / v.h);
            mw = v.w * s; mh = v.h * s;
            mx = (static_cast<float>(bw) - mw) * 0.5f;
            my = (static_cast<float>(bh) - mh) * 0.5f;
        }
        pv.qx = mx; pv.qy = my; pv.qw = mw; pv.qh = mh;
        if (s_gpuActive) {
            const auto rt = v.ul->render_target();
            // This call resolves the MSAA target, so it is what makes this frame visible:
            // call it every frame after the draw, never cache the result.
            auto* srv = static_cast<ID3D11ShaderResourceView*>(
                s_gpuSrv(s_gpu, rt.texture_id));
            if (cutPtr) mapCut(rt.uv_coords.left, rt.uv_coords.top, rt.uv_coords.right, rt.uv_coords.bottom);
            pv.srv = srv;
            pv.u0 = rt.uv_coords.left; pv.v0 = rt.uv_coords.top;
            pv.u1 = rt.uv_coords.right; pv.v1 = rt.uv_coords.bottom;
        } else {
            UploadSurfaceIfDirty(v, ctx);
            if (cutPtr) mapCut(0.0f, 0.0f, 1.0f, 1.0f);
            pv.srv = v.srv;
        }
        if (cutPtr) { pv.hasCutout = true; std::memcpy(pv.cutUV, cut, sizeof(cut)); }
        if (addRef && pv.srv) pv.srv->AddRef();
        return true;
    }

    // A collected view's quad over the back buffer (the desktop mirror on VR).
    static void EmitView(const PresentedView& pv, const EmitFn& emit)
    {
        if (VR::MirrorEnabled())
            emit(pv.srv, pv.u0, pv.v0, pv.u1, pv.v1, pv.qx, pv.qy, pv.qw, pv.qh, pv.hasCutout ? pv.cutUV : nullptr);
    }

    // The host's cursor quad, over every view. Game thread.
    static void EmitCursor(bool anyVisible, float bw, float bh, const EmitFn& emit)
    {
        // Our own cursor, topmost. The vanilla cursor sprite is hidden while
        // UI mode is up (Hook_CursorAdvanceMovie), and its draw is skipped
        // (skipGameCursor): at Present it would land under the overlay,
        // but in the UI pass the CursorMenu draws after us.
        // This quad tracks the same MenuCursor position, sized from
        // cursorHeight at 1080p and scaled with the back buffer's height.
        if (anyVisible && s_focused.load() && s_cursorSrv && bh > 0 && !VR::IsLive() &&   // never on a prepaint-only frame
            !s_gamepadHidesCursor.load()) {
            const float px = static_cast<float>(s_cursorPosX.load());
            const float py = static_cast<float>(s_cursorPosY.load());
            const int artHeight = static_cast<int>(std::lround(s_cursorHeight * (bh / 1080.0f)));
            // The glow fades in over a clickable element (the drawn art) and a press shrinks the cursor about its
            // hotspot (the drawn arrow, and a mod image that asked for it).
            static float glow = 0.0f, press = 0.0f;
            static auto last = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
            last = now;
            const auto approach = [dt](float v, float target, float seconds) {
                const float step = dt / seconds;
                return target > v ? std::min(target, v + step) : std::max(target, v - step);
            };
            const CursorPick pick = PickCursorAt(static_cast<int>(px), static_cast<int>(py), bw, bh);
            const PageCursor kind = pick.kind;
            glow  = approach(glow, kind == PageCursor::Clickable ? 1.0f : 0.0f, 0.12f);
            press = approach(press, s_mouseDown.load() ? 1.0f : 0.0f, 0.06f);
            ID3D11ShaderResourceView* modSrv = (!pick.hide && pick.image) ? CursorImageSrv(pick.image) : nullptr;
            if (pick.hide) {
                // The page under the pointer draws its own (CSS cursor: none, or its view's "none").
            } else if (modSrv) {
                // A mod's image: its height at 1080p (else its own) scaled with the backbuffer, aspect kept, the
                // hotspot pixel on the cursor position.
                const CursorImage& ci = *pick.image;
                const float h = (ci.height > 0.0f ? ci.height : static_cast<float>(ci.h)) * (bh / 1080.0f);
                const float w = h * (static_cast<float>(ci.w) / static_cast<float>(ci.h));
                const float scale = ci.press ? 1.0f - 0.14f * press : 1.0f;
                float x = px - ci.hotX * (w / ci.w) * scale, y = py - ci.hotY * (h / ci.h) * scale;
                if (scale == 1.0f) { x = std::round(x); y = std::round(y); }
                emit(modSrv, 0.0f, 0.0f, 1.0f, 1.0f, x, y, w * scale, h * scale, nullptr);
            } else if (const CursorArtSet* art = (s_cursorCustom && s_cursorImgH > 0) ? nullptr
                                                      : EnsureCursorArt(artHeight, pick.tint)) {
                // The drawn cursor, in the view's tint. At rest it sits on whole pixels (1:1 with its texture
                // when the target is the back buffer).
                const bool text = kind == PageCursor::Text;
                const CursorArtTex& t = text ? art->ibeam
                    : art->arrow[std::clamp(static_cast<int>(std::lround(glow * (kArtGlowSteps - 1))), 0, kArtGlowSteps - 1)];
                const float scale = text ? 1.0f : 1.0f - 0.14f * press;
                float x = px - t.hotX * scale, y = py - t.hotY * scale;
                if (scale == 1.0f) { x = std::round(x); y = std::round(y); }
                emit(t.srv, 0.0f, 0.0f, 1.0f, 1.0f, x, y, t.w * scale, t.h * scale, nullptr);
            } else if (s_cursorCustom && s_cursorImgH > 0) {
                // Custom art: configured height at 1080p scaled with the
                // backbuffer, aspect preserved, drawn so the hotspot
                // pixel sits exactly on the cursor position.
                const float h = s_cursorHeight * (bh / 1080.0f);
                const float w = h * (static_cast<float>(s_cursorImgW) / s_cursorImgH);
                emit(s_cursorSrv, 0.0f, 0.0f, 1.0f, 1.0f,
                     px - s_cursorHotX * w, py - s_cursorHotY * h, w, h, nullptr);
            } else {
                const float h = s_cursorHeight * (bh / 1080.0f);
                emit(s_cursorSrv, 0.0f, 0.0f, 1.0f, 1.0f,
                     px, py, h * (static_cast<float>(kCursorW) / kCursorH), h, nullptr);
            }
        }
    }

    // Where this frame's quads go. Opens or closes the UI pass's carrier for an interactive view, then: while the UI
    // pass is live the composite records its quads for it (defer) instead of drawing them over the back buffer
    // (drawNow). Game thread.
    struct CompositeTarget {
        bool defer = false;
        bool drawNow = true;
    };
    static CompositeTarget ChooseCompositeTarget(bool interactiveVisible)
    {
        // Main-menu pages draw at Present, but for engine mode, where Present is not ours to draw into.
        if (s_mainMenuOpen.load() && !s_engineComposite.load()) {
            s_compositeFrames.fetch_add(1);
            return {};
        }
        SyncOverlayMenu(interactiveVisible);
        s_compositeFrames.fetch_add(1);
        const bool defer = s_engineComposite.load() || (s_overlayMenuRegistered && UiPassActive());
        if (!s_engineComposite.load()) NoteUiPassState(defer, interactiveVisible);
        // The first deferred frame also draws here: the UI pass already ran this frame with nothing queued. Never in
        // engine mode, where Present is not ours to draw into.
        static bool s_deferredLast = false;
        const bool drawNow = !defer || (!s_deferredLast && !s_engineComposite.load());
        s_deferredLast = defer;
        return { defer, drawNow };
    }

    // This frame's emit: a quad over the back buffer when the target says draw, and queued (with a reference of its
    // own) for the UI pass when it says defer. Game thread.
    static EmitFn MakeEmit(IDXGISwapChain* sc, CompositeTarget target, std::vector<QueuedQuad>& queued)
    {
        return [sc, target, &queued](ID3D11ShaderResourceView* qsrv, float u0, float v0, float u1, float v1,
                                     float x, float y, float w, float h, const float* cutUV) {
            if (target.drawNow) DrawOverlay(sc, qsrv, u0, v0, u1, v1, x, y, w, h, cutUV);
            if (!target.defer || !qsrv) return;
            QueuedQuad q;
            q.srv = qsrv; qsrv->AddRef();
            q.u0 = u0; q.v0 = v0; q.u1 = u1; q.v1 = v1; q.x = x; q.y = y; q.w = w; q.h = h;
            if (cutUV) { q.hasCut = true; std::memcpy(q.cut, cutUV, sizeof(q.cut)); }
            queued.push_back(q);
        };
    }

    // The UI pass's quads for this frame, an empty set included, so a hidden view leaves no trace. Game thread.
    static void PublishUiPass(bool defer, std::vector<QueuedQuad>& queued, float bw, float bh)
    {
        std::lock_guard<std::mutex> lk(s_uiPassMutex);
        ReleaseQuads(s_uiPassQuads);
        if (defer) {
            s_uiPassQuads = std::move(queued);
            s_uiPassBackW = bw;
            s_uiPassBackH = bh;
            s_uiPassPublished = std::chrono::steady_clock::now();
        }
    }

    // Before Render, on Ultralight's thread: a view this frame draws is wanted (visible) and visible to Ultralight
    // since the second sync: Render paints only those, so one shown since waits for the next frame instead of
    // compositing a texture Render did not paint, and one hidden since is not drawn. needs_paint is sampled first, as
    // rendering clears it.
    static void SampleDrawnViews()
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        for (auto& vp : s_views) {
            MlView& v = *vp;
            const bool drawn = v.visible && v.ul && v.ul->is_visible();
            v.repaintedThisFrame = drawn && v.ul->needs_paint();
            if (drawn) {
                v.prepainted = true;
                v.settleRepaint = false;   // this frame's Render paints it
            }
        }
    }

    // Update and what must sit around it, from `t0` taken just before it (both frame paths, Ultralight's thread).
    // Returns the hidden view this frame's Render paints, shown, or null; sets the next frame's load-queue budget
    // (s_lastUlUpdateMs, MaterializeViews).
    static ultralight::View* UpdatePages(std::chrono::steady_clock::time_point t0, ViewId& preId, bool& preAgain)
    {
        s_ulRenderer->Update();
        // Shows and hides made by the input, the bridge drains and this Update (the VR keyboard on a text focus, a JS
        // listener the page calls from them, an OnDomReady callback), so this frame paints and composites them. A
        // show made by page code inside Render (requestAnimationFrame, resize, scroll, observer, transitionend
        // callbacks) waits for the next frame's first sync. Before RefreshDisplay, never after it: a View shown after
        // RefreshDisplay runs no requestAnimationFrame callback in this frame's Render, so a page that draws through
        // rAF would show its old texture for a frame (tools/visibility-test).
        SyncViewVisibility();
        // A hidden view this frame's Render paints (SetViewPrepaint: its prepaint or its settle repaint), shown here
        // for the same reason and hidden again right after Render.
        ultralight::View* pre = TakePrepaintView(preId, preAgain, MsBetween(t0, std::chrono::steady_clock::now()));
        if (pre) pre->set_visible(true);
        // 1.4 pump contract: requestAnimationFrame, CSS animations/transitions
        // and smooth scrolling are driven per-display by RefreshDisplay —
        // Update()+Render() alone still paints, but every animation silently
        // freezes without this call. Our views all ride default display 0.
        s_ulRenderer->RefreshDisplay(0);
        s_lastUlUpdateMs = MsBetween(t0, std::chrono::steady_clock::now());
        return pre;
    }

    static void NotePrepaint(ViewId preId, bool preAgain, double renderMs)
    {
        static std::atomic<int> s_preLog{ 0 };
        if (s_preLog.fetch_add(1) < 32)
            SKSE::log::info("Magelight: view {} prepainted while hidden{} (this frame's render {:.1f} ms)", preId,
                            preAgain ? " again, its page settled" : "", renderMs);
    }

    static void ThreadModeFrame(IDXGISwapChain* sc, float bw, float bh);   // fwd (the page thread, below)

    static void FrameWork(IDXGISwapChain* sc)
    {
        // Diagnostic: name every present-thread change (bounded — a session
        // sees at most a couple). This is what proved the two-thread pattern.
        const DWORD tid = GetCurrentThreadId();
        static DWORD lastTid = 0;
        static int tidLogs = 0;
        if (tid != lastTid && tidLogs < 8) {
            SKSE::log::info("Magelight: present thread now {} (was {})", tid, lastTid);
            lastTid = tid;
            ++tidLogs;
        }

        // Init on the MAIN thread only. Menus and loading screens present from
        // a separate render thread; gameplay presents from the game's main
        // loop — in both field runs the in-game present TID equals the TID
        // that loaded the plugin (11664 in 0.3.6, 16452 in 0.3.7). Heuristics
        // failed twice here: kPostLoadGame fires while the loading screen is
        // still presenting (0.3.6), and that loading screen presents many
        // SECONDS of stable frames, so a stability streak passes on the wrong
        // thread too (0.3.7). Thread identity is the deterministic signal.
        // Taken: this frame already passed the page fiber by, so the next one starts Ultralight, as after a refusal.
        const bool onMain = tid == s_mainThreadId;
        s_mainPresenting.store(onMain);
        if (onMain && TakePageInitFailure()) return;
        // World-ready FALLBACK (0.17.9): SKSEVR never delivered kNewGame for
        // an Alternate Perspective new game (field 2026-09-03: SKSE dispatched
        // 4 and 5 only; no 3, no 7), so the renderer never started and every
        // hotkey suspended the controls over nothing. The player standing in
        // a loaded cell with no main/loading menu up IS the world being
        // ready; the thread-identity gate below still decides WHEN to init.
        // Main-thread frames only (never the loading-screen thread), polled
        // about twice a second. Outside the render-init block: with main-menu pages the renderer is already up.
        if (!s_worldReady.load() && onMain) {
            static int s_wrPolls = 0;
            if ((++s_wrPolls % 30) == 0) {
                auto* pc = RE::PlayerCharacter::GetSingleton();
                auto* ui = RE::UI::GetSingleton();
                if (pc && ui && pc->GetParentCell() && pc->Is3DLoaded() &&
                    !ui->IsMenuOpen(RE::MainMenu::MENU_NAME) && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
                    SKSE::log::info("Magelight: world ready (fallback: player in a loaded cell — no kPostLoadGame/kNewGame arrived)");
                    NotifyWorldReady();
                }
            }
        }
        if (onMain && s_worldReady.load()) s_viewsReleased.store(true);
        if (!s_renderInit) {
            if (!onMain) return;
            if (!s_worldReady.load() &&
                !PageThread::StartsAtMainMenu(s_pageThreadMode.load() == PageThread::Mode::Wanted,
                                              s_anyMainMenuPage.load(), s_mainMenuOpen.load())) {
                // The page thread was refused after a main-menu page asked: no page runs until the world does, so the
                // UI mode entered for it would hold the controls over nothing.
                if (s_focused.load() && s_mainMenuOpen.load()) {
                    static std::atomic<bool> s_exitPosted{ false };
                    if (!s_exitPosted.exchange(true)) {
                        SKSE::log::warn("Magelight: UI mode ended - no page can run at the main menu");
                        GameTask::Post([]() { ForceExitUIMode(); s_exitPosted.store(false); });
                    }
                }
                return;
            }
            EnsureRenderInit(sc);
            // The page thread was refused (StartPageThread said why): the next frame the gate lets through starts
            // Ultralight here, on the page fiber, as without pageThread.
            if (!s_renderInit) return;
        }
        if (s_renderDead.load()) return;
        if (!onMain) {
            // A frame from a foreign thread (loading screens etc.): touching
            // Ultralight here is the fastfail. Skip the whole frame. (The main
            // thread is the one that composites, with or without the page thread.)
            static bool warned = false;
            if (!warned) {
                warned = true;
                SKSE::log::warn("Magelight: frame on foreign thread {} (owner {}) — skipping Ultralight work",
                    tid, s_mainThreadId);
            }
            return;
        }
        DXGI_SWAP_CHAIN_DESC scd{};
        float bw = 0, bh = 0;
        if (SUCCEEDED(sc->GetDesc(&scd))) {
            bw = static_cast<float>(scd.BufferDesc.Width);
            bh = static_cast<float>(scd.BufferDesc.Height);
            const int prevW = s_backbufferW.exchange(static_cast<int>(bw));  // for the cursor clamp (input sink)
            const int prevH = s_backbufferH.exchange(static_cast<int>(bh));
            s_backbufferWH.store((static_cast<std::uint64_t>(scd.BufferDesc.Width) << 32) | scd.BufferDesc.Height);
            // With the page thread it sends this itself (PageThreadFrame), so a RenderThread mod still gets it on the
            // render thread.
            if (prevW > 0 && prevH > 0 && (prevW != static_cast<int>(bw) || prevH != static_cast<int>(bh)) &&
                !PageThreadOn())
                Emit(HostEvent::DisplayResized, 0, static_cast<int>(bw), static_cast<int>(bh));
        }
        // pageThread: the page thread does the page work; this thread runs what it recorded and draws it.
        if (PageThreadOn()) {
            ThreadModeFrame(sc, bw, bh);
            return;
        }

        PageWorkBegin(bw, bh);
        GameBookkeeping();
        // Texture images: repaint the probe (if on), then register/re-point/
        // invalidate — BEFORE Update() so this frame's Render() sees them.
        TickImageProbe();
        MaterializeImages();
        // ── Present-path stage timing ────────────────────────────────
        // Four rounds of reasoning about VR frame cost have now been wrong,
        // twice confidently. The submit gate is provably working (0% upload
        // on an idle panel) and the game still stalls when the UI is up, so
        // the cost is somewhere this has never measured. Time each stage and
        // print the worst frame once a second: cheap, and it ends the
        // guessing rather than adding to it.
        using ClockT = std::chrono::steady_clock;
        struct StageAcc {
            double ulUpdate = 0, ulRender = 0, composite = 0, vrSubmit = 0;
            double worst = 0;
            int    frames = 0;
            ClockT::time_point lastReport{};
        };
        static StageAcc s_acc;
        const auto stageT0 = ClockT::now();
        auto stageMs = [](ClockT::time_point a, ClockT::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };

        ViewId preId = 0;
        bool preAgain = false;
        ultralight::View* pre = UpdatePages(stageT0, preId, preAgain);
        const auto stageAfterUpdate = ClockT::now();
        double frameUlRender = 0.0;   // THIS frame's render, for the composite subtraction
        const bool anyVisible = AnyViewVisible();   // one reading for the render below and the cursor
        PresentedFrame frame;   // this frame's views, for every presenter (flat + VR)
        // While the UI pass is live the composite records its quads for it instead of drawing them over the
        // back buffer; published below every frame, an empty set included, so a hidden view leaves no trace.
        const bool interactiveVisible = AnyInteractiveViewVisible();
        const CompositeTarget target = ChooseCompositeTarget(interactiveVisible);
        const bool defer = target.defer;
        std::vector<QueuedQuad> queued;
        const EmitFn emit = MakeEmit(sc, target, queued);
        if (anyVisible || pre) {
            SampleDrawnViews();
            // One state capture brackets everything we do to the pipeline this
            // frame — Render (the driver's uploads run inside it), its command
            // list (which freely rebinds RTs/shaders/scissor) and every
            // composite draw alike.
            StateBackup backup;
            backup.Capture(s_context);
            // Render() paints every View Ultralight sees visible and skips hidden ones (a hidden page keeps what it
            // drew and repaints only what changed on its next show). Visible pages' requestAnimationFrame callbacks
            // run inside it, so it stays outside s_viewsMutex (invariant 16).
            s_ulRenderer->Render();
            if (pre) pre->set_visible(false);
            frameUlRender = stageMs(stageAfterUpdate, ClockT::now());
            s_acc.ulRender += frameUlRender;
            if (pre) NotePrepaint(preId, preAgain, frameUlRender);
            StateBackup::Neutralize(s_context);
            if (s_gpuActive && s_gpuHas(s_gpu)) s_gpuDraw(s_gpu);
            // Composite visible views in z-order ((layer, order) — the
            // registry is kept sorted by SortViewsLocked). Registry
            // mutation only happens on our own threads — the brief lock walks
            // are safe against the API surface.
            {
                std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
                for (auto& vp : s_views) {
                    MlView& v = *vp;
                    PresentedView pv;
                    if (!CollectView(v, bw, bh, s_context, false, pv)) continue;   // what Render painted (above)
                    EmitView(pv, emit);
                    frame.push_back(std::move(pv));
                }
            }
            EmitCursor(anyVisible, bw, bh, emit);
            backup.Restore(s_context);
        } else {
            // Ultralight 2.0 creates and destroys GPU resources only inside Render (GPUDriver.h), so with nothing
            // shown a View released meanwhile (hibernation, DestroyView, a rebuild) would keep its targets, tiles and
            // textures until a view is next shown. Render at least once a second: with every View hidden it paints
            // nothing (invariant 23) and costs microseconds.
            static ClockT::time_point s_lastIdleRender{};
            const auto now = ClockT::now();
            if (now - s_lastIdleRender >= std::chrono::seconds(1)) {
                s_lastIdleRender = now;
                StateBackup backup;
                backup.Capture(s_context);
                const auto renderT0 = ClockT::now();
                s_ulRenderer->Render();
                frameUlRender = stageMs(renderT0, ClockT::now());
                s_acc.ulRender += frameUlRender;
                StateBackup::Neutralize(s_context);
                if (s_gpuActive && s_gpuHas(s_gpu)) s_gpuDraw(s_gpu);   // a View hidden after the second sync
                backup.Restore(s_context);
            }
        }
        PublishUiPass(defer, queued, bw, bh);
        // VR presenter: runs every frame (an empty frame HIDES stale overlays);
        // no-op unless the runtime is live. Same thread as everything above.
        const auto stageBeforeVR = ClockT::now();
        VR::SubmitFrame(frame, s_focused.load(), static_cast<ViewId>(s_uiModeView.load()));
        const auto stageEnd = ClockT::now();

        s_acc.ulUpdate  += stageMs(stageT0, stageAfterUpdate);
        // Subtract THIS frame's render, not the running accumulator — the
        // latter grew every frame and drove the reported composite deeply
        // negative, which made the one number meant to localise cost useless.
        s_acc.composite += (std::max)(0.0, stageMs(stageAfterUpdate, stageBeforeVR) - frameUlRender);
        s_acc.vrSubmit  += stageMs(stageBeforeVR, stageEnd);
        const double whole = stageMs(stageT0, stageEnd);
        if (whole > s_acc.worst) s_acc.worst = whole;
        ++s_acc.frames;
        if (s_acc.lastReport.time_since_epoch().count() == 0) s_acc.lastReport = stageEnd;
        if (stageEnd - s_acc.lastReport >= std::chrono::seconds(2) && s_acc.frames > 0) {
            const double n = static_cast<double>(s_acc.frames);
            SKSE::log::info(
                "Magelight: present cost over {} frames — ul.update {:.2f}ms, ul.render {:.2f}ms, "
                "composite(+mirror) {:.2f}ms, vr.submit {:.2f}ms, total avg {:.2f}ms, WORST {:.2f}ms "
                "(uiMode={}, views={})",
                s_acc.frames, s_acc.ulUpdate / n, s_acc.ulRender / n, s_acc.composite / n,
                s_acc.vrSubmit / n,
                (s_acc.ulUpdate + s_acc.ulRender + s_acc.composite + s_acc.vrSubmit) / n,
                s_acc.worst, s_focused.load(), frame.size());
            s_acc = StageAcc{};
            s_acc.lastReport = stageEnd;
        }
        TickMemoryReport();   // after the stage timing: its GetMemoryUsage is not page work
    }

    // The carrier's PostDisplay: draw the published quads into the render target the game's UI pass has bound,
    // scaled from the back buffer they were laid out for. Main thread: inside the engine's UI render, or, for the
    // end-of-frame fallback (s_endOfFrameFallback), at the engine's end of frame into the target it left bound. The
    // fallback takes the frame only when a target is bound and never counts toward the no-target latch: without
    // one, Present keeps drawing as before.
    static void UiPassCompositeImpl(bool endOfFrameFallback)
    {
        if (endOfFrameFallback) {
            if (!s_context || !s_device || s_renderDead.load()) return;
            ID3D11RenderTargetView* bound = nullptr;
            s_context->OMGetRenderTargets(1, &bound, nullptr);
            if (!bound) return;
            bound->Release();
        }
        s_lastUiPassFrame.store(s_compositeFrames.load());
        s_uiPassSeen.store(true);
        if (!s_context || !s_device || s_renderDead.load()) return;
        std::vector<QueuedQuad> quads;
        float backW = 0, backH = 0;
        {
            std::lock_guard<std::mutex> lk(s_uiPassMutex);
            // Present stopped composing (a load, a stalled hook): draw nothing rather than a frozen frame.
            if (s_uiPassQuads.empty() ||
                std::chrono::steady_clock::now() - s_uiPassPublished > std::chrono::milliseconds(250)) return;
            quads = s_uiPassQuads;
            for (auto& q : quads) q.srv->AddRef();
            backW = s_uiPassBackW;
            backH = s_uiPassBackH;
        }
        StateBackup backup;
        backup.Capture(s_context);
        ID3D11RenderTargetView* target = backup.rtvs[0];
        float tw = 0, th = 0;
        if (target) {
            ID3D11Resource* res = nullptr;
            target->GetResource(&res);
            ID3D11Texture2D* tex = nullptr;
            if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
                D3D11_TEXTURE2D_DESC td{};
                tex->GetDesc(&td);
                tw = static_cast<float>(td.Width);
                th = static_cast<float>(td.Height);
                tex->Release();
                // Which target the UI pass draws into is the whole question on an unfamiliar setup: log each change.
                static void* s_lastTarget = nullptr;
                static int   s_targetLogs = 0;
                if (res != s_lastTarget && s_targetLogs < 10) {
                    s_lastTarget = res;
                    ++s_targetLogs;
                    SKSE::log::info("Magelight: UI pass draws into a {}x{} target, format {} (back buffer {}x{})",
                        td.Width, td.Height, static_cast<int>(td.Format), backW, backH);
                }
            }
            if (res) res->Release();
        }
        static int s_noTargetFrames = 0;   // consecutive passes with nothing to draw on
        if (target && tw > 0 && th > 0 && backW > 0 && backH > 0) {
            s_noTargetFrames = 0;
            StateBackup::Neutralize(s_context);
            const float sx = tw / backW, sy = th / backH;
            for (const auto& q : quads)
                DrawQuadTo(target, tw, th, q.srv, q.u0, q.v0, q.u1, q.v1,
                           q.x * sx, q.y * sy, q.w * sx, q.h * sy, q.hasCut ? q.cut : nullptr);
        } else if (!target && !s_engineMode && ++s_noTargetFrames >= 30 && !s_uiPassBroken.exchange(true)) {
            // A page drawn at Present (frame generation may drop or smear it) beats an open menu nobody can see.
            SKSE::log::warn("Magelight: the UI pass has had no render target for 30 frames - views draw at Present");
        }
        backup.Restore(s_context);
        ReleaseQuads(quads);
    }

    static void UiPassComposite(bool endOfFrameFallback)
    {
        __try {
            UiPassCompositeImpl(endOfFrameFallback);
        } __except (LogSehAndDisable("UI pass", GetExceptionCode(),
                        (GetExceptionInformation())->ExceptionRecord->ExceptionAddress)) {
        }
    }

    // SEH wrapper — no locals with destructors allowed in here.
    static void GuardedFrameWorkHere(IDXGISwapChain* sc)
    {
        __try {
            FrameWork(sc);
        } __except (LogSehAndDisable("frame work", GetExceptionCode(),
                        (GetExceptionInformation())->ExceptionRecord->ExceptionAddress)) {
        }
    }

    // Pages on a stack of their own (1.3.0). The game gives its main thread 1 MB (the exe header) and presents deep
    // in its frame, under every other Present hook, so WebKit ran in what was left there: a page that recursed in
    // its scripts and then laid itself out, or laid out a deeply nested page, could run it out (0xC00000FD in the
    // frame work, the overlay off for the session). The frame work runs on this fiber on the same thread, so
    // Ultralight keeps its one thread (invariant 1) and gets ulFiberMB of stack. Every Ultralight call is made in
    // the frame work, so WebKit has known this stack as the thread's own since Renderer::Create: its script limits
    // and its collector's scan of the stack both read it. Between frames the thread is back on the game's stack,
    // where a collector helper thread scanning it with the fiber's bounds reads past the fiber (Ultralight 2.0
    // crashes there: tools/fiber-gc-test), so the thread leaves the fiber holding the page VM's lock (s_vmLock).
    // Main thread only.
    static LPVOID          s_ulFiber = nullptr;
    static LPVOID          s_ulFiberReturn = nullptr;   // the fiber the frame work hands back to
    static IDXGISwapChain* s_ulFiberSwapChain = nullptr;
    static unsigned        s_gameMxcsr = 0;             // the game's SSE mode at Present, which the pages run under
    static bool            s_ulFiberTried = false;

    // Taken at the end of the first frame in which WebKit's VM exists (it is made with the first page, not with the
    // renderer) and never released: the thread then holds it on every way off the fiber, between frames, before the
    // first view, with every view hibernated, after an exception in a frame turned the overlay off, and through
    // process exit. Released at each fiber entry and taken again on the way out instead, every lock WebKit takes in a
    // frame is a full acquire and release rather than a recursive one (offline, tools/fiber-gc-test --bench, an idle
    // page: about 165 us a frame against 20, CPU and GPU alike), and a frame that an exception stopped inside the
    // lock's own code could leave it taken by nobody, so taking it on the way out would hang the game. The cost of
    // holding it: collections finish only inside frames, so the heap peaks higher between them (offline, 10 rounds of
    // garbage: 82-125 MB against 44-90, flat in both).
    // While it is held, a collection the collector starts waits for the thread to stop at a safepoint, which it
    // reaches only inside the frame work. Touched only on the fiber: WebKit keeps its per-thread state in fiber-local
    // storage, so off the fiber the holder is a different thread to it. With pageThread it is the page thread's
    // instead (PageThreadMain), held between its frames for the speed alone; never both, as the mode is decided before
    // the renderer exists. constinit and trivially destroyed: nothing runs at exit (invariant 3).
    static constinit PageStack::VmLock s_vmLock;

    // 0 no VM yet, 1 held, 2 taken now (the first VM, or WebKit made a new one), -1 the attempt faulted.
    // SEH wrapper — no locals with destructors allowed in here.
    static int GuardedHoldVmLock()
    {
        __try {
            void* const before = s_vmLock.Vm();
            if (!s_vmLock.Hold(PageStackExports())) return 0;   // resolved here on the page thread
            return s_vmLock.Vm() != before ? 2 : 1;
        } __except (LogSehAndDisable("the page VM lock", GetExceptionCode(),
                        (GetExceptionInformation())->ExceptionRecord->ExceptionAddress)) {
            return -1;
        }
    }

    static void CALLBACK UlFiberMain(void*)
    {
        // Room on this stack for the overflow handler's own work (a guarantee is per fiber).
        ULONG guarantee = 64 * 1024;
        SetThreadStackGuarantee(&guarantee);
        for (;;) {
            _mm_setcsr(s_gameMxcsr);   // as before 1.3.0, WebKit runs under the game's floating-point mode
            GuardedFrameWorkHere(s_ulFiberSwapChain);
            // After every frame, one an exception ended included, and here near the fiber's top: where the lock is
            // first taken is where WebKit counts script depth from.
            if (GuardedHoldVmLock() == 2)
                SKSE::log::info("Magelight: WebKit's VM is up ({}); the main thread holds its lock whenever it is "
                                "off the page stack", s_vmLock.Vm());
            SwitchToFiber(s_ulFiberReturn);
        }
    }

    // The process is exiting: Ultralight's objects are left alive, not destroyed. Their destructors would run on the
    // exiting stack, which WebKit does not know as this thread's (it knows the page fiber's), so a garbage collection
    // or stack scan in them would read the wrong range; the process ends either way. Registered once the fiber exists,
    // after these statics were built, so it runs before their destructors.
    static void LeaveUltralightAtExit()
    {
        (void)s_ulRenderer.LeakRef();
        if (auto* views = new (std::nothrow) std::vector<std::unique_ptr<MlView>>()) views->swap(s_views);
        if (auto* sessions = new (std::nothrow) std::map<std::string, ultralight::RefPtr<ultralight::Session>>())
            sessions->swap(s_sessions);
        if (auto* images = new (std::nothrow) std::vector<std::unique_ptr<MlImage>>()) images->swap(s_images);
    }

    // ── The page thread (1.7.0, "pageThread") ────────────────────────────────
    // With it on, a thread of Magelight's own (PageThreadMain) owns Ultralight from Renderer::Create on: script,
    // layout, Update, Render, and the GPU driver recording into a deferred context of the game's device. Each page
    // frame ends in a PagePacket: its command list and what it drew. At Present the game thread (ThreadModeFrame)
    // takes every finished packet and at once lets the page thread start its next frame, then runs their lists in
    // order on the immediate context and draws the newest packet's views. It never waits on the page thread, so a
    // page stuck in a script freezes every page (they share this thread) but never the game (the watchdog names it).
    // A recorded list keeps every resource it uses alive until it is released, and a packet's SRVs hold references of
    // their own, so a View Ultralight frees meanwhile cannot pull a texture from under the game thread
    // (tools/deferred-test). No fiber here: the thread's stack is WebKit's own, in a frame and between frames
    // (tools/fiber-gc-test --thread passes without the VM lock); it holds the lock between frames for speed only (an
    // idle frame about 13 us against 235). Flat only (VR never asks for it).
    struct PagePacket {
        // The frame's recorded GPU work, in order: the parts the driver split off at each flush (MgGpu_TakeCommandList),
        // then the frame's own. Empty when the frame rendered nothing.
        std::vector<ID3D11CommandList*> lists;
        PresentedFrame                  views;   // what it drew: each srv holds a reference, `ul` stays empty
        PagePacket() = default;
        PagePacket(const PagePacket&) = delete;
        PagePacket& operator=(const PagePacket&) = delete;
        ~PagePacket()   // game thread; D3D references only
        {
            for (ID3D11CommandList* list : lists)
                if (list) list->Release();
            for (auto& pv : views)
                if (pv.srv) pv.srv->Release();
        }
    };
    static ID3D11DeviceContext*             s_pageContext = nullptr;   // the page thread's deferred context
    // Set by a page thread that got no renderer, after it let go of the driver; TakePageInitFailure takes it from there.
    static std::atomic<bool>                s_pageInitFailed{ false };
    static PageThread::Handoff<PagePacket>* s_pageHandoff = nullptr;   // made once and never freed (invariant 3)
    static std::atomic<long long>           s_pageFrameSinceMs{ 0 };   // when the running page frame began; 0 = waiting
    static std::atomic<const char*>         s_pageStage{ "" };         // where the running page frame is
    static std::atomic<std::uint64_t>       s_pageFrames{ 0 };         // page frames made (the game thread's cost line)

    // One page frame, on the page thread: FrameWork's page parts in their order, every drawn view collected with an
    // SRV reference of its own, then the frame's lists. A frame that rendered nothing hands over no list, so the game
    // thread does no Direct3D work while nothing is shown. Its own cost line every 2 s, timed as FrameWork's:
    // ul.update from just before Update, the page work before it apart.
    static void PageThreadFrame()
    {
        using ClockT = std::chrono::steady_clock;
        const auto t0 = ClockT::now();
        s_pageFrameSinceMs.store(SteadyNowMs());
        const std::uint64_t wh = s_backbufferWH.load();
        const float bw = static_cast<float>(static_cast<std::uint32_t>(wh >> 32));
        const float bh = static_cast<float>(static_cast<std::uint32_t>(wh));
        // The game thread leaves DisplayResized to this thread, so a RenderThread mod gets it on the render thread.
        static int s_lastW = 0, s_lastH = 0;
        if (static_cast<int>(bw) != s_lastW || static_cast<int>(bh) != s_lastH) {
            if (s_lastW > 0 && s_lastH > 0 && bw > 0 && bh > 0)
                Emit(HostEvent::DisplayResized, 0, static_cast<int>(bw), static_cast<int>(bh));
            s_lastW = static_cast<int>(bw);
            s_lastH = static_cast<int>(bh);
        }
        s_pageStage.store("its page work (input, bridge, lifecycle)");
        PageWorkBegin(bw, bh);
        MaterializeImages();
        const auto tWork = ClockT::now();
        s_pageStage.store("Renderer::Update");
        ViewId preId = 0;
        bool preAgain = false;
        ultralight::View* pre = UpdatePages(tWork, preId, preAgain);
        const auto tUpdated = ClockT::now();
        auto packet = std::make_unique<PagePacket>();
        bool rendered = false;
        s_pageStage.store("Renderer::Render");
        if (AnyViewVisible() || pre) {
            SampleDrawnViews();
            s_ulRenderer->Render();
            rendered = true;
            if (pre) pre->set_visible(false);
            if (pre) NotePrepaint(preId, preAgain, MsBetween(tUpdated, ClockT::now()));
            if (s_gpuActive && s_gpuHas(s_gpu)) s_gpuDraw(s_gpu);
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            for (auto& vp : s_views) {
                PresentedView pv;
                if (CollectView(*vp, bw, bh, s_pageContext, true, pv)) packet->views.push_back(std::move(pv));
            }
        } else {
            // Nothing shown: Render at least once a second all the same (invariant 22), as FrameWork does.
            static ClockT::time_point s_lastIdleRender{};
            if (tUpdated - s_lastIdleRender >= std::chrono::seconds(1)) {
                s_lastIdleRender = tUpdated;
                s_ulRenderer->Render();
                rendered = true;
                if (s_gpuActive && s_gpuHas(s_gpu)) s_gpuDraw(s_gpu);
            }
        }
        // Only Render, the driver's draws and the views' uploads record into the deferred context: a frame without
        // them recorded nothing.
        if (rendered) {
            s_pageStage.store("FinishCommandList");
            if (s_gpuActive)
                while (ID3D11CommandList* part = s_gpuTakeList(s_gpu)) packet->lists.push_back(part);
            ID3D11CommandList* list = nullptr;
            if (const HRESULT hr = s_pageContext->FinishCommandList(FALSE, &list); SUCCEEDED(hr)) {
                packet->lists.push_back(list);
            } else {
                static std::atomic<int> s_finishLogs{ 0 };
                if (s_finishLogs.fetch_add(1) < 4)
                    SKSE::log::error("Magelight: FinishCommandList failed (0x{:08X}) - this page frame's GPU work is lost",
                                     static_cast<unsigned>(hr));
            }
        }
        const auto tEnd = ClockT::now();
        s_pageStage.store("its memory report");
        TickMemoryReport();
        s_pageHandoff->Push(std::move(packet));
        s_pageFrames.fetch_add(1);
        s_pageFrameSinceMs.store(0);
        s_pageStage.store("");

        struct Acc {
            double work = 0, update = 0, render = 0, worst = 0;
            int    frames = 0;
            ClockT::time_point last{};
        };
        static Acc acc;
        acc.work += MsBetween(t0, tWork);
        acc.update += MsBetween(tWork, tUpdated);
        acc.render += MsBetween(tUpdated, tEnd);
        acc.worst = (std::max)(acc.worst, MsBetween(t0, tEnd));
        ++acc.frames;
        if (acc.last == ClockT::time_point{}) acc.last = tEnd;
        if (tEnd - acc.last >= std::chrono::seconds(2)) {
            const double n = static_cast<double>(acc.frames);
            SKSE::log::info("Magelight: page thread over {} frames - page work {:.2f}ms, ul.update {:.2f}ms, ul.render "
                            "{:.2f}ms, total avg {:.2f}ms, WORST {:.2f}ms, {:.0f} frames/s",
                            acc.frames, acc.work / n, acc.update / n, acc.render / n,
                            (acc.work + acc.update + acc.render) / n, acc.worst, n * 1000.0 / MsBetween(acc.last, tEnd));
            acc = Acc{};
            acc.last = tEnd;
        }
    }

    // The stall watchdog's look at the page thread: a page frame running 5 s or more is named once, with the thread's
    // stack (a script that never returns, a page stuck in layout). The game is not stalled by it; nothing is done.
    static void CheckPageThreadStall()
    {
        static long long s_reported = 0;   // the start of the page frame last reported
        if (!PageThreadOn() || s_renderDead.load()) return;
        const long long since = s_pageFrameSinceMs.load();
        if (!since || since == s_reported) return;
        const long long running = SteadyNowMs() - since;
        if (running < 5000) return;
        s_reported = since;
        SKSE::log::warn("Magelight: the page thread has been in {} for {} s - its pages are frozen; the game goes on",
                        s_pageStage.load(), running / 1000);
        LogThreadStack(s_ulThreadId, "page", running, kMaxStackFrames, true);
    }

    // After a fault on the page thread: the host is built /EHsc, so no lock_guard between the fault and the guard
    // unlocked on the way out. The locks the page work holds across Ultralight and driver calls are given back if this
    // thread still holds them, or the parked page thread would hang the game's next lock (tools/pagethread-test). The
    // frame it was in is over, for the watchdog too.
    static void ReleasePageLocksAfterFault()
    {
        s_pageFrameSinceMs.store(0);
        s_pageStage.store("");
        const bool views = s_viewsMutex.ReleaseIfHeldByThisThread();
        const bool images = s_imagesMutex.ReleaseIfHeldByThisThread();
        if (views || images)
            SKSE::log::warn("Magelight: the page thread faulted holding {}{}{}; given back", views ? "the view registry" : "",
                            views && images ? " and " : "", images ? "the image registry" : "");
    }

    // SEH wrappers — no locals with destructors allowed in here.
    static bool GuardedPageInit()
    {
        __try {
            return CreateUltralight(s_pageContext);
        } __except (LogSehAndDisable("the page thread's start", GetExceptionCode(),
                        (GetExceptionInformation())->ExceptionRecord->ExceptionAddress)) {
            ReleasePageLocksAfterFault();
            return false;
        }
    }

    static void GuardedPageFrame()
    {
        __try {
            PageThreadFrame();
        } __except (LogSehAndDisable("page frame work", GetExceptionCode(),
                        (GetExceptionInformation())->ExceptionRecord->ExceptionAddress)) {
            ReleasePageLocksAfterFault();
        }
    }

    static DWORD WINAPI PageThreadMain(void*)
    {
        // Room on this stack for the overflow handler's own work (a guarantee is per thread), as on the page fiber.
        ULONG guarantee = 64 * 1024;
        SetThreadStackGuarantee(&guarantee);
        // Posts made here go through the pump, in order, as posts made inside Present do (MagelightGameTask.h).
        GameTask::Scope scope;
        if (GuardedPageInit()) {
            std::atexit(&LeaveUltralightAtExit);
            SKSE::log::info("Magelight: render-thread init complete (the page thread {} owns Ultralight)",
                            GetCurrentThreadId());
            s_rendererUp.store(true);
            // One frame per Go from the game thread's Present (StartPageThread made sure the event exists).
            while (s_pageHandoff->WaitGo(nullptr, INFINITE)) {
                if (s_renderDead.load()) break;
                GuardedPageFrame();
                // After every frame (one an exception ended included), near the top of this thread's stack, where
                // WebKit counts script depth from: the page VM's lock, held from the first frame with a VM on. For speed
                // (WebKit's own locks become recursive): nothing depends on it here, unlike on the page fiber.
                if (!s_renderDead.load() && GuardedHoldVmLock() == 2)
                    SKSE::log::info("Magelight: WebKit's VM is up ({}); the page thread holds its lock between frames",
                                    s_vmLock.Vm());
            }
        } else if (s_renderDead.load()) {
            // A fault: Ultralight may be half built, so nothing tries it again.
            SKSE::log::error("Magelight: render-thread init failed on the page thread — overlay disabled");
        } else {
            // No renderer and no fault: the game thread starts Ultralight in its own frame with a driver of its own.
            ultralight::Platform::instance().set_gpu_driver(nullptr);
            if (s_gpu && s_gpuDestroy) s_gpuDestroy(s_gpu);
            s_gpu = nullptr;
            s_gpuActive = false;
            s_pageInitFailed.store(true);
        }
        // Done with Ultralight: the thread never touches it again, and never returns (WebKit's per-thread state would
        // be torn down on the way out).
        for (;;) Sleep(INFINITE);
    }

    // Render init, game thread, with the device resources made: whether this device can carry the page thread and, if
    // so, its deferred context, its handoff and the thread. False: the mode is Off (the log line says why) and render
    // init runs again next frame, as without the setting.
    static bool StartPageThread()
    {
        ID3D11DeviceContext* deferred = nullptr;
        const bool deferredOk = SUCCEEDED(s_device->CreateDeferredContext(0, &deferred)) && deferred;
        const char* refused = PageThread::Decide(s_device->GetCreationFlags(), deferredOk);
        const int stackMb = s_ulFiberMb > 0 ? s_ulFiberMb : kMinFiberMb;
        HANDLE thread = nullptr;
        if (!refused) {
            s_pageContext = deferred;
            s_pageHandoff = new (std::nothrow) PageThread::Handoff<PagePacket>();
            s_pageThreadMode.store(PageThread::Mode::On);   // before the thread: CreateUltralight reads it
            if (s_pageHandoff && s_pageHandoff->Valid())
                thread = PageThread::StartThread(&PageThreadMain, nullptr, stackMb, L"Magelight page thread");
            if (!thread) {
                refused = "the page thread could not be created";
                s_pageContext = nullptr;
            }
        }
        if (refused) {
            if (deferred) deferred->Release();
            s_pageThreadMode.store(PageThread::Mode::Off);
            SKSE::log::warn("Magelight: the page thread is off - {}; pages run in the game's frame", refused);
            return false;
        }
        s_ulThreadId = GetThreadId(thread);
        CloseHandle(thread);   // the thread runs on; the host never joins it
        D3D11_FEATURE_DATA_THREADING threading{};
        const bool known = SUCCEEDED(s_device->CheckFeatureSupport(D3D11_FEATURE_THREADING, &threading, sizeof(threading)));
        SKSE::log::info("Magelight: pages run on a thread of their own ({}, {} MB stack); the graphics driver's command "
                        "lists are {}", s_ulThreadId, stackMb,
                        !known ? "unknown" : threading.DriverCommandLists ? "native" : "emulated by Direct3D");
        return true;
    }

    // Game thread, every main-thread Present. A page thread that could not make a renderer hands render init back: the
    // mode is Off and render init runs again, as when the thread is refused. True when it took one. The note the
    // failure left is cleared, or a later death of the overlay would report this one.
    static bool TakePageInitFailure()
    {
        if (!s_pageInitFailed.exchange(false)) return false;
        if (s_pageContext) s_pageContext->Release();
        s_pageContext = nullptr;
        s_pageThreadMode.store(PageThread::Mode::Off);
        s_ulThreadId = GetCurrentThreadId();
        s_renderInit = false;
        s_renderDeadReasonState.store(0);
        SKSE::log::warn("Magelight: the page thread could not start Ultralight; pages run in the game's frame");
        return true;
    }

    // The frame at Present with the page thread: the game thread's bookkeeping, every page frame finished since the
    // last Present taken and the next one let go at once (it records while this frame runs the taken frames' lists in
    // order and draws the newest one's views). Never waits on the page thread: until it has finished a frame there is
    // simply nothing new to draw.
    static void ThreadModeFrame(IDXGISwapChain* sc, float bw, float bh)
    {
        using ClockT = std::chrono::steady_clock;
        const auto t0 = ClockT::now();
        GameBookkeeping();
        TickImageProbe();
        std::vector<std::unique_ptr<PagePacket>> packets = s_pageHandoff->TakeAll();
        // The views wanted now, read here rather than from the page frame: one hidden since (Escape, a mod's hide) is
        // not drawn even when the page thread is slow or stuck in a script (it never runs script under the registry's
        // lock, invariant 16). Read before Go, while a page thread that finished its frame is waiting and the lock is
        // free; while the page thread still holds it (a page's load or resize, the views it collects), this frame
        // keeps the last frame's reading rather than wait.
        static std::vector<ViewId> s_wanted;
        static bool s_anyVisible = false, s_interactiveVisible = false;
        if (std::unique_lock<PageThread::OwnedMutex> lk(s_viewsMutex, std::try_to_lock); lk.owns_lock()) {
            s_wanted.clear();
            s_anyVisible = s_interactiveVisible = false;
            for (const auto& v : s_views) {
                if (!v->visible || !ViewShows(*v)) continue;
                s_anyVisible = true;
                s_interactiveVisible = s_interactiveVisible || !v->clickThrough;
                s_wanted.push_back(v->id);
            }
        }
        s_pageHandoff->Go();   // the next page frame overlaps this frame's execute and draws
        // The newest page frame's views, drawn until a newer frame comes. Never freed at exit: its references are
        // Direct3D's, which may be gone by then.
        static PagePacket* s_held = nullptr;
        const PagePacket* newest = packets.empty() ? s_held : packets.back().get();
        const bool anyVisible = s_anyVisible, interactiveVisible = s_interactiveVisible;
        const CompositeTarget target = ChooseCompositeTarget(interactiveVisible);
        std::vector<QueuedQuad> queued;
        const EmitFn emit = MakeEmit(sc, target, queued);
        bool lists = false;
        for (const auto& p : packets) lists = lists || !p->lists.empty();
        const auto tPrelude = ClockT::now();
        auto tExecuted = tPrelude;
        std::size_t drawn = 0;   // the cost line's views, as the in-frame line's: drawn this frame, not held
        if (lists || anyVisible) {
            StateBackup backup;
            backup.Capture(s_context);
            // Each list with the game's whole state saved and put back around it (PageThread::RunRecordedList), and the
            // immediate context flushed after each part a flush split off, where the in-frame driver flushes, and
            // between two page frames, which in the frame always have a Present between them.
            for (std::size_t k = 0; k < packets.size(); ++k) {
                auto& p = packets[k];
                for (std::size_t i = 0; i < p->lists.size(); ++i) {
                    PageThread::RunRecordedList(s_context, p->lists[i]);
                    if (i + 1 < p->lists.size() || k + 1 < packets.size()) s_context->Flush();
                    p->lists[i]->Release();
                }
                p->lists.clear();
            }
            tExecuted = ClockT::now();
            StateBackup::Neutralize(s_context);
            if (newest)
                for (const auto& pv : newest->views)
                    if (std::find(s_wanted.begin(), s_wanted.end(), pv.id) != s_wanted.end()) {
                        EmitView(pv, emit);
                        ++drawn;
                    }
            EmitCursor(anyVisible, bw, bh, emit);
            backup.Restore(s_context);
        }
        PublishUiPass(target.defer, queued, bw, bh);
        if (!packets.empty()) {
            delete s_held;
            s_held = packets.back().release();
        }
        const auto tEnd = ClockT::now();

        struct Acc {
            double prelude = 0, execute = 0, composite = 0, worst = 0;
            int    frames = 0, withoutNew = 0;
            std::uint64_t pageFramesAtLast = 0;
            ClockT::time_point last{};
        };
        static Acc acc;
        acc.prelude += MsBetween(t0, tPrelude);
        acc.execute += MsBetween(tPrelude, tExecuted);
        acc.composite += MsBetween(tExecuted, tEnd);
        acc.worst = (std::max)(acc.worst, MsBetween(t0, tEnd));
        ++acc.frames;
        if (packets.empty()) ++acc.withoutNew;
        if (acc.last == ClockT::time_point{}) {
            acc.last = tEnd;
            acc.pageFramesAtLast = s_pageFrames.load();
        }
        if (tEnd - acc.last >= std::chrono::seconds(2)) {
            const double n = static_cast<double>(acc.frames);
            const std::uint64_t pageFrames = s_pageFrames.load();
            SKSE::log::info("Magelight: present cost over {} frames - prelude {:.2f}ms, execute {:.2f}ms, composite "
                            "{:.2f}ms, total avg {:.2f}ms, WORST {:.2f}ms (uiMode={}, views={}, page thread: {} frames, "
                            "{} presents without a new one)",
                            acc.frames, acc.prelude / n, acc.execute / n, acc.composite / n,
                            (acc.prelude + acc.execute + acc.composite) / n, acc.worst, s_focused.load(), drawn,
                            pageFrames - acc.pageFramesAtLast, acc.withoutNew);
            acc = Acc{};
            acc.last = tEnd;
            acc.pageFramesAtLast = pageFrames;
        }
    }

    // False: the frame work runs on the caller's stack, as before 1.3.0. Decided once, before the renderer exists.
    static bool EnsureUlFiber()
    {
        if (s_ulFiber) return true;
        if (s_ulFiberTried) return false;
        s_ulFiberTried = true;
        if (s_ulFiberMb <= 0) {
            SKSE::log::info("Magelight: pages run on the game's frame stack (ulFiberMB 0)");
            return false;
        }
        if (s_renderInit) {   // WebKit already knows the game's stack as this thread's: it must stay there
            SKSE::log::warn("Magelight: the renderer started before the page stack - pages run on the game's stack");
            return false;
        }
        // Without the VM lock the fiber is a crash in waiting, so the pages take the game's stack (as ulFiberMB 0).
        if (const PageStack::Exports& e = PageStackExports(); !e.LockReady()) {
            SKSE::log::error("Magelight: this Ultralight runtime does not export {}, which the page stack needs - "
                             "pages run on the game's stack instead (as with ulFiberMB 0). The SDK changed under "
                             "Magelight: tell its author", e.missingLock);
            return false;
        }
        // The game's own floating-point state is saved across the switch with the flag (and by hand around it,
        // for a thread another plugin made a fiber).
        if (!IsThreadAFiber() && !ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH)) {
            SKSE::log::warn("Magelight: the main thread could not become a fiber (error {}) - pages run on the "
                            "game's stack", GetLastError());
            return false;
        }
        s_ulFiber = CreateFiberEx(256 * 1024, static_cast<SIZE_T>(s_ulFiberMb) << 20, FIBER_FLAG_FLOAT_SWITCH,
                                  &UlFiberMain, nullptr);
        if (!s_ulFiber) {
            SKSE::log::warn("Magelight: the page stack ({} MB) could not be created (error {}) - pages run on the "
                            "game's stack", s_ulFiberMb, GetLastError());
            return false;
        }
        std::atexit(&LeaveUltralightAtExit);
        s_pagesOnFiber = true;
        SKSE::log::info("Magelight: pages run on a stack of their own ({} MB, a fiber on the game's main thread)",
                        s_ulFiberMb);
        return true;
    }

    // How much of its own stack the game's frame leaves at Present: at the first frame and at each new low (16 KB
    // steps). Without the page stack it is all the pages get. A stack guarantee (CrashLogger's, for its crash handler)
    // moves the overflow up by its size; on the game's stack the pages' JavaScript zones clear the guarantee the
    // thread had when the renderer started (ApplyGameStackReserve), so a larger one set later is worth a line.
    static void NoteGameStack(bool onFiber)
    {
        if (!onFiber && s_reserveGuarantee != SIZE_MAX) {
            static bool s_grewWarned = false;
            ULONG now = 0;
            if (!s_grewWarned && SetThreadStackGuarantee(&now) && now > s_reserveGuarantee) {
                s_grewWarned = true;
                SKSE::log::warn("Magelight: the game's main thread now holds back {} KB of its stack (a stack "
                                "guarantee), more than the {} KB the pages' JavaScript zones were sized for - an error a "
                                "page leaves uncaught may end the overlay", now / 1024, s_reserveGuarantee / 1024);
            }
        }
        static std::size_t s_lowest = SIZE_MAX;
        const std::size_t  left = StackReserveLeftKb();
        if (left + 16 > s_lowest) return;
        s_lowest = left;
        ULONG guarantee = 0;   // 0 reads the thread's guarantee without changing it
        const bool known = SetThreadStackGuarantee(&guarantee) != 0;
        const std::string held = !known ? std::string("stack guarantee unknown")
                                 : guarantee ? std::to_string(guarantee / 1024) + " KB of it held back by a stack guarantee"
                                             : std::string("no stack guarantee");
        SKSE::log::info("Magelight: the game's frame leaves {} KB of its stack at Present (the lowest this session; {}){}",
                        left, held, onFiber ? "; pages run on their own" : "; pages run in it");
    }

    static void GuardedFrameWork(IDXGISwapChain* sc)
    {
        // Before the world is ready the frame work returns before any Ultralight call, so the fiber can start here.
        // With the page thread asked for or running, this thread makes no Ultralight call at all: no fiber (a refusal
        // turns the mode Off before the renderer exists, so the fiber can still start then).
        if (GetCurrentThreadId() == s_mainThreadId && s_pageThreadMode.load() == PageThread::Mode::Off) {
            // Called from inside the frame work itself (no path does today): the frame is already being worked.
            if (s_ulFiber && IsThreadAFiber() && GetCurrentFiber() == s_ulFiber) return;
            const bool fiber = EnsureUlFiber();
            NoteGameStack(fiber);
            if (fiber) {
                // Another plugin may have turned the thread back into a plain one since. Once WebKit knows the
                // fiber's stack the frame work never runs on the game's, so a frame that cannot switch is skipped.
                if (!IsThreadAFiber() && !ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH)) return;
                s_ulFiberSwapChain = sc;
                s_ulFiberReturn = GetCurrentFiber();
                s_gameMxcsr = _mm_getcsr();
                SwitchToFiber(s_ulFiber);
                _mm_setcsr(s_gameMxcsr);   // the game goes on in its own mode, whatever a page set
                return;
            }
        }
        GuardedFrameWorkHere(sc);
    }

    // IDXGISwapChain::ResizeBuffers (vtable slot 13): resolution / fullscreen
    // changes. We hold NO backbuffer references between frames (DrawOverlay
    // takes and releases its RTV per draw), so nothing must be dropped here;
    // fullscreen views re-size themselves off the new backbuffer next frame
    // and DisplayResized fires from that size change. This hook exists so a
    // resize is VISIBLE in the log and so a failed ResizeBuffers (some other
    // hook holding a reference) is named instead of silently breaking the
    // game's present.
    using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    static ResizeBuffersFn s_origResizeBuffers = nullptr;
    static HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h,
                                                       DXGI_FORMAT fmt, UINT flags)
    {
        const HRESULT hr = s_origResizeBuffers(sc, count, w, h, fmt, flags);
        static std::atomic<int> s_logged{ 0 };
        if (s_logged.fetch_add(1) < 16) {
            if (SUCCEEDED(hr))
                SKSE::log::info("Magelight: swapchain resized to {}x{} (buffers {}, format {})", w, h, count,
                    static_cast<int>(fmt));
            else
                SKSE::log::error("Magelight: ResizeBuffers({}x{}) FAILED 0x{:08X} — a swapchain reference is "
                    "outstanding somewhere (not ours: we hold none between frames)", w, h,
                    static_cast<unsigned>(hr));
        }
        return hr;
    }

    // ── Present diagnostics: who presents, and what ──────────────────────────
    // A page that draws (ul.render, composite in the cost line) but never shows means something presents
    // another image after us: frame generation, an upscaler or ENB proxy, a second swapchain. These lines
    // name it from the log alone.

    static std::string ModuleOf(const void* addr)
    {
        HMODULE mod = nullptr;
        if (!addr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                         static_cast<LPCWSTR>(addr), &mod) || !mod)
            return fmt::format("{:p} (no module)", addr);
        char path[MAX_PATH] = {};
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* name = std::strrchr(path, '\\');
        return fmt::format("{}+0x{:X}", name ? name + 1 : path,
            reinterpret_cast<std::uintptr_t>(addr) - reinterpret_cast<std::uintptr_t>(mod));
    }

    static std::string DescribeSwapChain(IDXGISwapChain* sc)
    {
        DXGI_SWAP_CHAIN_DESC d{};
        if (!sc || FAILED(sc->GetDesc(&d))) return "GetDesc failed";
        return fmt::format("{}x{} format {} buffers {} swapEffect {} flags 0x{:X} windowed {} hwnd {:p}",
            d.BufferDesc.Width, d.BufferDesc.Height, static_cast<int>(d.BufferDesc.Format), d.BufferCount,
            static_cast<int>(d.SwapEffect), d.Flags, d.Windowed ? 1 : 0, static_cast<void*>(d.OutputWindow));
    }

    static IDXGISwapChain*                 s_gameSwapChain = nullptr;   // the one InstallHook hooked (renderWindows[0])
    static std::mutex                      s_seenSwapChainsMutex;
    static std::vector<IDXGISwapChain*>    s_seenSwapChains;            // bounded: kMaxSeenSwapChains
    static std::atomic<IDXGISwapChain*>    s_lastPresenter{ nullptr };
    static std::atomic<std::uint64_t>      s_presentsGame{ 0 }, s_presentsOther{ 0 };
    static constexpr std::size_t           kMaxSeenSwapChains = 8;

    // Once per swapchain: its description and the module that called Present on it. A periodic line counts
    // presents of other swapchains, logged only while there are some.
    static void NotePresenter(IDXGISwapChain* sc, const void* caller)
    {
        (sc == s_gameSwapChain ? s_presentsGame : s_presentsOther).fetch_add(1, std::memory_order_relaxed);
        if (s_lastPresenter.exchange(sc) != sc) {
            std::lock_guard<std::mutex> lk(s_seenSwapChainsMutex);
            if (std::find(s_seenSwapChains.begin(), s_seenSwapChains.end(), sc) == s_seenSwapChains.end()
                && s_seenSwapChains.size() < kMaxSeenSwapChains) {
                s_seenSwapChains.push_back(sc);
                SKSE::log::info("Magelight: Present on swapchain {:p} ({}) from {} - {}", static_cast<void*>(sc),
                    sc == s_gameSwapChain ? "the game's" : "NOT the game's", ModuleOf(caller), DescribeSwapChain(sc));
            }
        }
        static std::atomic<long long> s_nextSummaryMs{ 0 };
        const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        long long due = s_nextSummaryMs.load();
        if (now >= due && s_nextSummaryMs.compare_exchange_strong(due, now + 10000) && due != 0) {
            const auto other = s_presentsOther.exchange(0);
            const auto game = s_presentsGame.exchange(0);
            if (other > 0)
                SKSE::log::warn("Magelight: presents in the last 10 s - game swapchain {}, other swapchains {}", game, other);
        }
    }

    // Where the frame is composited. The vtable hooks below are installed at kDataLoaded, so a plugin that
    // hooked Present earlier (an upscaler, a proxy) runs AFTER our composite and can draw over it. Late
    // composite moves the draw into a detour on dxgi's own Present, the last code before the frame is queued.
    // One composite per present per thread: a wrapper's Present that calls Present1, or a vtable hook above
    // the late detour, must not draw twice (t_presentDepth / t_composited).
    static std::atomic<int>  s_lateMisses{ 0 };   // vtable presents whose chain never reached the late detour
    static thread_local int  t_presentDepth = 0;
    static thread_local bool t_composited = false;

    static void StampPresent(IDXGISwapChain* sc, const void* caller)
    {
        NotePresenter(sc, caller);
        // Stall watchdog heartbeat: EVERY present counts, from whichever thread (loading screens and menus
        // present from a foreign thread and FrameWork returns at its top).
        s_lastPresentMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch()).count());
        s_lastPresentTid.store(GetCurrentThreadId());
    }

    // A swapchain on another device (a frame-generation backend's own D3D11 or D3D12 swapchain) must never be
    // drawn into with our device's resources. Skipping without marking the present composited lets the late
    // fallback count it as a miss. Asked only of dxgi's own swapchains, inside the detours: a wrapper's
    // GetDevice (Community Shaders') can hand out the device without a reference, and our Release then
    // freed the game's device within a second.
    static ID3D11Device* s_gameDevice = nullptr;   // the game swapchain's device, read at hook install; never Release (a wrapper may not AddRef)

    static bool OnOurDevice(IDXGISwapChain* sc)
    {
        ID3D11Device* const ours = s_device ? s_device : s_gameDevice;
        ID3D11Device* dev = nullptr;
        if (!ours || FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) return false;
        const bool same = dev == ours;
        dev->Release();
        return same;
    }

    // `inDetour`: called from a dxgi detour, where `sc` is dxgi's own swapchain; from a vtable hook it is the
    // game's object, which may be a wrapper, so it is compared, never asked.
    static void ComposeOnce(IDXGISwapChain* sc, bool fromLateDetour, bool inDetour)
    {
        if (t_composited || fromLateDetour != s_lateComposite.load()) return;
        if (inDetour ? !OnOurDevice(sc) : sc != s_gameSwapChain) return;
        t_composited = true;
        if (s_runtimeReady.load() && !s_renderDead.load()) GuardedFrameWork(sc);
    }

    static void AfterCompose()
    {
        if (s_device && !s_renderDead.load()) {
            const HRESULT removed = s_device->GetDeviceRemovedReason();
            if (FAILED(removed)) {
                char detail[64];
                std::snprintf(detail, sizeof(detail), "reason 0x%08X", static_cast<unsigned>(removed));
                SKSE::log::error("Magelight: the graphics device was removed ({}){}", detail,
                    SmoothMotionLoaded() ? " - NVIDIA Smooth Motion is loaded" : "");
                NoteRenderFailure("the graphics device was removed", detail);
                s_renderDead.store(true);
            }
        }
        // Render death is announced once, outside the SEH guard (FrameWork is never entered again after the
        // kill switch), and a player is never left with suspended controls by a dead renderer.
        if (s_renderDead.load()) {
            static std::atomic<bool> s_deadNotified{ false };
            if (!s_deadNotified.exchange(true)) {
                VR::Shutdown();   // overlays must not outlive a dead overlay host
                ReleaseCursorClip("the overlay stopped");   // FrameWork, which would let it go, never runs again
                Emit(HostEvent::RenderDead, 0, 0, 0, RenderDeadReasonText());
                s_deadNoticePending.store(true);
                if (s_overlayMenuRegistered)
                    GameTask::Post([]() {
                        if (auto* q = RE::UIMessageQueue::GetSingleton())
                            q->AddMessage(MagelightOverlayMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
                    });
            }
            if (s_focused.load()) GameTask::Post([]() { SetUIMode(false); });
            // A death at start lands at the main menu, where the HUD is closed and a notice would be lost: it
            // waits for the HUD (asked every 60 presents).
            static int s_deadNoticeFrames = 0;
            if (s_deadNoticePending.load() && ++s_deadNoticeFrames >= 60) {
                s_deadNoticeFrames = 0;
                auto* ui = RE::UI::GetSingleton();
                if (ui && ui->IsMenuOpen(RE::HUDMenu::MENU_NAME) && s_deadNoticePending.exchange(false))
                    GameTask::Post([]() {
                        RE::SendHUDMessage::ShowHUDMessage("Magelight UI could not draw and is off for this session - see Magelight.log");
                    });
            }
        }
    }

    // The outermost present of a thread ends here; the next one may composite again. In late mode a vtable
    // present whose chain never reached the detour (the plugin under us presented another way) is a miss, and
    // 30 in a row fall back to compositing in the vtable hook, so the UI is never lost for good.
    static void EndPresent(bool vtableHook)
    {
        if (--t_presentDepth > 0) return;
        if (vtableHook && s_lateComposite.load()) {
            if (t_composited) {
                s_lateMisses.store(0);
            } else if (s_lateMisses.fetch_add(1) + 1 >= 30) {
                s_lateComposite.store(false);
                SKSE::log::warn("Magelight: late composite did not draw in 30 frames (dxgi's Present not reached, or not on the game's device) - compositing in the vtable hook again");
            }
        }
        t_composited = false;
    }

    static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
    {
        GameTask::Scope pumpScope;   // posts from here go through the pump (MagelightGameTask.h)
        ++t_presentDepth;
        StampPresent(sc, _ReturnAddress());
        ComposeOnce(sc, false, false);
        AfterCompose();
        const HRESULT hr = s_origPresent(sc, sync, flags);
        EndPresent(true);
        return hr;
    }

    using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    static Present1Fn s_origPresent1 = nullptr;

    // Flip-model presenters (frame generation, some upscalers and overlays) can present through Present1,
    // which never passes through Present.
    static std::atomic<bool> s_latePresent1Hooked{ false };   // dxgi's Present1 carries a late detour

    static HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                                  const DXGI_PRESENT_PARAMETERS* params)
    {
        GameTask::Scope pumpScope;
        ++t_presentDepth;
        StampPresent(sc, _ReturnAddress());
        // Without a Present1 detour, late mode has nowhere later to draw a Present1 frame: draw it here.
        ComposeOnce(sc, s_lateComposite.load() && !s_latePresent1Hooked.load(), false);
        AfterCompose();
        const HRESULT hr = s_origPresent1(sc, sync, flags, params);
        EndPresent(true);
        return hr;
    }

    static PresentFn  s_latePresentNext = nullptr;    // MinHook trampolines into dxgi's own code
    static Present1Fn s_latePresent1Next = nullptr;

    static HRESULT STDMETHODCALLTYPE LatePresent(IDXGISwapChain* sc, UINT sync, UINT flags)
    {
        GameTask::Scope pumpScope;
        ++t_presentDepth;
        ComposeOnce(sc, true, true);
        AfterCompose();
        const HRESULT hr = s_latePresentNext(sc, sync, flags);
        EndPresent(false);
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE LatePresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                                  const DXGI_PRESENT_PARAMETERS* params)
    {
        GameTask::Scope pumpScope;
        ++t_presentDepth;
        ComposeOnce(sc, true, true);
        AfterCompose();
        const HRESULT hr = s_latePresent1Next(sc, sync, flags, params);
        EndPresent(false);
        return hr;
    }

    static const IMAGE_NT_HEADERS* NtHeaders(const void* base)
    {
        const auto* b = static_cast<const std::uint8_t*>(base);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(b + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
    }

    // dxgi's own function for a vtable slot, whatever was patched into the slot since. The slot's bytes are
    // read from the dxgi.dll FILE: an image mapping shares the live module's (relocated, patched) pages. The
    // file holds preferred base + RVA, unrelocated.
    static void* OriginalVtableSlot(HMODULE mod, void* const* vtbl, int slot)
    {
        wchar_t path[MAX_PATH] = {};
        if (!GetModuleFileNameW(mod, path, MAX_PATH)) return nullptr;
        const auto* liveNt = NtHeaders(mod);
        if (!liveNt) return nullptr;
        const std::uint64_t rva = static_cast<std::uint64_t>(
            reinterpret_cast<const std::uint8_t*>(&vtbl[slot]) - reinterpret_cast<const std::uint8_t*>(mod));
        std::ifstream f(path, std::ios::binary);
        if (!f) return nullptr;
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!f.read(reinterpret_cast<char*>(&dos), sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        f.seekg(dos.e_lfanew);
        if (!f.read(reinterpret_cast<char*>(&nt), sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.OptionalHeader.SizeOfImage != liveNt->OptionalHeader.SizeOfImage ||
            nt.FileHeader.TimeDateStamp != liveNt->FileHeader.TimeDateStamp ||
            nt.OptionalHeader.CheckSum != liveNt->OptionalHeader.CheckSum)
            return nullptr;
        f.seekg(dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader);
        for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
            IMAGE_SECTION_HEADER sh{};
            if (!f.read(reinterpret_cast<char*>(&sh), sizeof(sh))) return nullptr;
            if (rva < sh.VirtualAddress || rva + sizeof(std::uint64_t) > std::uint64_t{ sh.VirtualAddress } + sh.SizeOfRawData) continue;
            std::uint64_t onDisk = 0;
            f.seekg(sh.PointerToRawData + (rva - sh.VirtualAddress));
            if (!f.read(reinterpret_cast<char*>(&onDisk), sizeof(onDisk))) return nullptr;
            const std::uint64_t fnRva = onDisk - nt.OptionalHeader.ImageBase;
            return fnRva < nt.OptionalHeader.SizeOfImage ? reinterpret_cast<std::uint8_t*>(mod) + fnRva : nullptr;
        }
        return nullptr;
    }

    static HMODULE ModuleAt(const void* p)
    {
        HMODULE mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(p), &mod);
        return mod;
    }

    // `mod`'s file name is `name`, whichever folder it loaded from (Streamline ships one per backend).
    static bool ModuleBaseNameIs(HMODULE mod, const wchar_t* name)
    {
        wchar_t path[MAX_PATH] = {};
        if (!mod || !GetModuleFileNameW(mod, path, MAX_PATH)) return false;
        const wchar_t* file = std::wcsrchr(path, L'\\');
        return _wcsicmp(file ? file + 1 : path, name) == 0;
    }

    // What a known module in the present chain is, for the log: "" for anything else.
    static const char* KnownPresentLayer(const void* addr)
    {
        const HMODULE mod = ModuleAt(addr);
        if (ModuleBaseNameIs(mod, L"NvPresent64.dll")) return " (NVIDIA driver present layer, used by Smooth Motion)";
        if (ModuleBaseNameIs(mod, L"sl.interposer.dll")) return " (NVIDIA Streamline)";
        if (ModuleBaseNameIs(mod, L"CommunityShaders.dll")) return " (Community Shaders frame-generation proxy)";
        if (ModuleBaseNameIs(mod, L"SkyrimUpscaler.dll")) return " (Skyrim Upscaler)";
        return "";
    }

    // Whether `p` points into committed executable memory. A non-canonical value (vtable slot past the end of a
    // short vtable, read as data) fails VirtualQuery and answers false.
    static bool IsExecutableAddress(const void* p)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
        constexpr DWORD kExec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return (mbi.Protect & kExec) != 0 && (mbi.Protect & PAGE_GUARD) == 0;
    }

    // `mod` is the system directory's `name`: a same-named proxy in the game folder (ReShade or a frame-gen
    // layer installed as dxgi.dll) is not dxgi's own code.
    static bool IsSystemModule(HMODULE mod, const wchar_t* name)
    {
        wchar_t path[MAX_PATH] = {}, sys[MAX_PATH] = {};
        if (!mod || !GetModuleFileNameW(mod, path, MAX_PATH) || !GetSystemDirectoryW(sys, MAX_PATH)) return false;
        return _wcsicmp(path, (std::wstring(sys) + L"\\" + name).c_str()) == 0;
    }

    // dxgi's swapchain vtable when the game's swapchain is a proxy (ENB's d3d11.dll wraps it): a throwaway
    // WARP device and swapchain made through the SYSTEM d3d11.dll, so a d3d11.dll proxy does not see them. A
    // dxgi.dll proxy would (system d3d11 binds to the loaded dxgi.dll), so the caller never calls this behind one.
    // The vtable is shared by every dxgi swapchain; only its address is kept.
    static void* const* DxgiSwapChainVtable()
    {
        wchar_t sys[MAX_PATH] = {};
        if (!GetSystemDirectoryW(sys, MAX_PATH)) return nullptr;
        HMODULE d3d = LoadLibraryW((std::wstring(sys) + L"\\d3d11.dll").c_str());
        if (!d3d) return nullptr;
        void* const* out = nullptr;
        auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d, "D3D11CreateDevice"));
        HWND wnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ID3D11Device* dev = nullptr;
        if (create && wnd && SUCCEEDED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, nullptr))) {
            IDXGIDevice* dxgiDev = nullptr;
            IDXGIAdapter* adapter = nullptr;
            IDXGIFactory* factory = nullptr;
            if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) &&
                SUCCEEDED(dxgiDev->GetAdapter(&adapter)) &&
                SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory)))) {
                DXGI_SWAP_CHAIN_DESC d{};
                d.BufferCount = 1;
                d.BufferDesc.Width = 8;
                d.BufferDesc.Height = 8;
                d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                d.OutputWindow = wnd;
                d.SampleDesc.Count = 1;
                d.Windowed = TRUE;
                IDXGISwapChain* sc = nullptr;
                if (SUCCEEDED(factory->CreateSwapChain(dev, &d, &sc)) && sc) {
                    out = *reinterpret_cast<void***>(sc);
                    sc->Release();
                }
            }
            if (factory) factory->Release();
            if (adapter) adapter->Release();
            if (dxgiDev) dxgiDev->Release();
            dev->Release();
        }
        if (wnd) DestroyWindow(wnd);
        return out;
    }

    // "auto" turns it on behind an earlier Present hook on a plain dxgi swapchain, and for Smooth Motion without
    // engine mode when d3d11.dll is the system one; never on VR. Behind a swapchain wrapper (a d3d11.dll proxy such
    // as ENB's, Streamline, Skyrim Upscaler) dxgi's own Present is found through a throwaway device, which only
    // "late" and Smooth Motion take: it killed the game at start behind ENB. Behind a dxgi.dll proxy (ReShade's)
    // nothing takes it: the device binds to the proxy, so the search could only find the proxy's swapchain again.
    static void InstallLatePresent(void* const* vtbl, void* const* vtbl1)
    {
        const std::string& mode = s_presentHookMode;
        if (mode == "vtable") {
            SKSE::log::info("Magelight: presentHook 'vtable' - compositing in the vtable hook");
            return;
        }
        if (s_engineMode) return;   // Present is not ours to draw into
        if (s_untrustedChain.load()) {
            // Its swapchain is not a dxgi D3D11 one (frame generation on Direct3D 12): dxgi's Present never draws
            // on our device, and the throwaway swapchain the search takes is not worth the risk.
            SKSE::log::info("Magelight: presentHook '{}' - the game swapchain is untrusted, compositing in the vtable hook", mode);
            return;
        }
        const bool earlierHook = ModuleAt(reinterpret_cast<const void*>(s_origPresent)) != ModuleAt(vtbl);
        // Without engine mode, Smooth Motion leaves dxgi's own Present as the only safe place to draw. Not behind
        // ENB's or ReShade's d3d11.dll: the throwaway device the dxgi search takes killed the game at start there.
        const bool smoothMotion = mode == "auto" && !REL::Module::IsVR() && SmoothMotionLoaded()
            && IsSystemModule(GetModuleHandleW(L"d3d11.dll"), L"d3d11.dll");
        if (smoothMotion)
            SKSE::log::info("Magelight: NVIDIA Smooth Motion is loaded and engine mode is off - drawing late, inside "
                            "dxgi's own Present (pages flicker while it is on)");
        if (mode != "late" && !smoothMotion && (!earlierHook || REL::Module::IsVR())) {
            SKSE::log::info("Magelight: presentHook 'auto' - {}, compositing in the vtable hook",
                REL::Module::IsVR() ? "VR" : "no earlier Present hook");
            return;
        }
        HMODULE dxgi = ModuleAt(vtbl);
        if (!IsSystemModule(dxgi, L"dxgi.dll")) {
            if (mode != "late" && !smoothMotion) {
                SKSE::log::info("Magelight: presentHook 'auto' - the game swapchain is a proxy ({}), compositing in "
                                "the vtable hook", ModuleOf(vtbl));
                return;
            }
            // The throwaway device below binds to whatever dxgi.dll is loaded, so behind a dxgi.dll proxy the search
            // can only find the proxy's swapchain again; do not take the device for nothing.
            if (!IsSystemModule(GetModuleHandleW(L"dxgi.dll"), L"dxgi.dll")) {
                SKSE::log::warn("Magelight: late composite unavailable - the loaded dxgi.dll is not the system one (a "
                                "proxy such as ReShade's), so dxgi's own Present cannot be reached");
                return;
            }
            // A proxy's swapchain wraps a real dxgi one: detour dxgi's own functions, found through a throwaway
            // dxgi swapchain.
            SKSE::log::info("Magelight: the game swapchain is a proxy ({}); finding dxgi's own swapchain", ModuleOf(vtbl));
            vtbl = DxgiSwapChainVtable();
            vtbl1 = vtbl;
            dxgi = vtbl ? ModuleAt(vtbl) : nullptr;
            if (!vtbl || !IsSystemModule(dxgi, L"dxgi.dll")) {
                SKSE::log::warn("Magelight: late composite unavailable - no dxgi swapchain vtable found");
                return;
            }
        }
        void* present = OriginalVtableSlot(dxgi, vtbl, 8);
        void* present1 = (vtbl1 && ModuleAt(vtbl1) == dxgi) ? OriginalVtableSlot(dxgi, vtbl1, 22) : nullptr;
        if (!present || ModuleAt(present) != dxgi) {
            SKSE::log::warn("Magelight: late composite unavailable - dxgi's own Present not found");
            return;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
            SKSE::log::warn("Magelight: late composite unavailable - MinHook init: {}", MH_StatusToString(init));
            return;
        }
        MH_STATUS st = MH_CreateHook(present, reinterpret_cast<void*>(&LatePresent), reinterpret_cast<void**>(&s_latePresentNext));
        if (st == MH_OK) st = MH_EnableHook(present);
        if (st != MH_OK) {
            SKSE::log::warn("Magelight: late composite unavailable - hooking dxgi's Present at {} failed: {}",
                ModuleOf(present), MH_StatusToString(st));
            return;
        }
        if (present1 && ModuleAt(present1) == dxgi) {
            MH_STATUS st1 = MH_CreateHook(present1, reinterpret_cast<void*>(&LatePresent1), reinterpret_cast<void**>(&s_latePresent1Next));
            if (st1 == MH_OK) st1 = MH_EnableHook(present1);
            if (st1 == MH_OK)
                s_latePresent1Hooked.store(true);
            else
                SKSE::log::warn("Magelight: late composite: dxgi's Present1 at {} not hooked ({}) - Present1 frames composite in the vtable hook",
                    ModuleOf(present1), MH_StatusToString(st1));
        }
        s_lateComposite.store(true);
        SKSE::log::info("Magelight: late composite ON ('{}') - drawing inside dxgi's Present at {}, after {}",
            mode, ModuleOf(present), ModuleOf(reinterpret_cast<const void*>(s_origPresent)));
    }

    // ── Hook install (game thread, kDataLoaded) ─────────────────────────────

    void InstallHook()
    {
        bool expected = false;
        if (!s_hookInstalled.compare_exchange_strong(expected, true)) return;
        GameTask::Start();   // before the first present, so no thread is created inside one

        auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
        if (!data) {
            SKSE::log::error("Magelight: no renderer data — hook not installed");
            return;
        }
        auto* sc = reinterpret_cast<IDXGISwapChain*>(data->renderWindows[0].swapChain);
        if (!sc) {
            SKSE::log::error("Magelight: no swapchain — hook not installed");
            return;
        }

        void** vtbl = *reinterpret_cast<void***>(sc);
        constexpr int kPresentSlot = 8;
        DWORD oldProt = 0;
        if (!VirtualProtect(&vtbl[kPresentSlot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
            SKSE::log::error("Magelight: VirtualProtect failed — hook not installed");
            return;
        }
        s_origPresent = reinterpret_cast<PresentFn>(vtbl[kPresentSlot]);
        s_gameSwapChain = sc;
        const HRESULT devHr = sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&s_gameDevice));
        if (FAILED(devHr) || !s_gameDevice) {
            s_gameDevice = EngineDevice();   // borrowed like the swapchain's: never Released
            s_untrustedChain.store(true);
            SKSE::log::warn("Magelight: the game swapchain refused its device (0x{:08X}); the engine's is {:p}",
                static_cast<unsigned>(devHr), static_cast<void*>(s_gameDevice));
        }
        vtbl[kPresentSlot] = reinterpret_cast<void*>(&HookPresent);
        VirtualProtect(&vtbl[kPresentSlot], sizeof(void*), oldProt, &oldProt);
        // dxgi.dll here is a plain swapchain; anything else (an ENB d3d11.dll, an upscaler or frame-generation
        // proxy) wraps it, and what it presents is not necessarily the buffer we draw into.
        SKSE::log::info("Magelight: hooked Present was {}{} (vtable in {}{}); game swapchain {}",
            ModuleOf(reinterpret_cast<const void*>(s_origPresent)), KnownPresentLayer(reinterpret_cast<const void*>(s_origPresent)),
            ModuleOf(vtbl), KnownPresentLayer(vtbl), DescribeSwapChain(sc));
        void** vtbl1 = nullptr;
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1))) && sc1) {
            vtbl1 = *reinterpret_cast<void***>(sc1);
            constexpr int kPresent1Slot = 22;
            // A wrapper may answer IDXGISwapChain1 with an IDXGISwapChain-only vtable (Community Shaders' frame
            // generation proxy has 18 slots): slots 18-22 are then whatever follows it, and writing slot 22
            // corrupted the IID its GetDevice compares. Patch only when IDXGISwapChain1's five slots are code.
            // Slots 18-22 span at most the vtable's own page and slot 22's, so slot 22's page decides readability.
            MEMORY_BASIC_INFORMATION slotMem{};
            const bool slotsReadable = VirtualQuery(&vtbl1[kPresent1Slot], &slotMem, sizeof(slotMem))
                && slotMem.State == MEM_COMMIT && (slotMem.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
            bool slotsAreCode = slotsReadable;
            for (int i = 18; slotsAreCode && i <= kPresent1Slot; ++i) slotsAreCode = IsExecutableAddress(vtbl1[i]);
            if (!slotsAreCode) {
                s_untrustedChain.store(true);
                if (slotsReadable)
                    SKSE::log::warn("Magelight: IDXGISwapChain1 vtable in {} is short (slots 18-22: {:p} {:p} {:p} {:p} {:p}) - "
                        "Present1 not hooked, the chain is untrusted", ModuleOf(vtbl1), vtbl1[18], vtbl1[19], vtbl1[20],
                        vtbl1[21], vtbl1[22]);
                else
                    SKSE::log::warn("Magelight: IDXGISwapChain1 vtable in {} ends before slot 22 (unreadable memory) - "
                        "Present1 not hooked, the chain is untrusted", ModuleOf(vtbl1));
                vtbl1 = nullptr;
            } else if (VirtualProtect(&vtbl1[kPresent1Slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
                s_origPresent1 = reinterpret_cast<Present1Fn>(vtbl1[kPresent1Slot]);
                vtbl1[kPresent1Slot] = reinterpret_cast<void*>(&HookPresent1);
                VirtualProtect(&vtbl1[kPresent1Slot], sizeof(void*), oldProt, &oldProt);
            }
            sc1->Release();
        }
        constexpr int kResizeBuffersSlot = 13;
        if (VirtualProtect(&vtbl[kResizeBuffersSlot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
            s_origResizeBuffers = reinterpret_cast<ResizeBuffersFn>(vtbl[kResizeBuffersSlot]);
            vtbl[kResizeBuffersSlot] = reinterpret_cast<void*>(&HookResizeBuffers);
            VirtualProtect(&vtbl[kResizeBuffersSlot], sizeof(void*), oldProt, &oldProt);
        } else {
            SKSE::log::warn("Magelight: ResizeBuffers hook not installed (VirtualProtect failed)");
        }

        SKSE::log::info("Magelight: Present hook installed (swapchain {:p})",
            static_cast<void*>(sc));

        // Milestone 3: subclass the game window for input. Real WM messages
        // are the only clean source of translated text (WM_CHAR); everything
        // is queued to the render thread, nothing touches Ultralight here.
        DXGI_SWAP_CHAIN_DESC scd{};
        if (SUCCEEDED(sc->GetDesc(&scd)) && scd.OutputWindow) {
            s_hwnd = scd.OutputWindow;
            s_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                s_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookWndProc)));
            if (s_origWndProc) {
                SKSE::log::info("Magelight: window subclassed for input (hwnd {:p})",
                    static_cast<void*>(s_hwnd));
                // Read before installing, so the shim never runs with no next proc (see AnsiShimWndProc).
                s_ansiShimPrev.store(reinterpret_cast<WNDPROC>(GetWindowLongPtrA(s_hwnd, GWLP_WNDPROC)));
                const auto replaced = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
                    s_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&AnsiShimWndProc)));
                if (replaced) {
                    s_ansiShimPrev.store(replaced);
                    SKSE::log::info("Magelight: ANSI shim on top of the input subclass");
                } else {
                    SKSE::log::warn("Magelight: ANSI shim not installed (GetLastError={}); a plugin that subclasses "
                        "after Magelight with SetWindowLongPtrA and calls its previous proc directly will crash",
                        GetLastError());
                }
            } else {
                SKSE::log::error("Magelight: window subclass failed (GetLastError={}) — no view input",
                    GetLastError());
            }
        } else {
            SKSE::log::error("Magelight: no output window on swapchain — no view input");
        }

        // Host settings (toggle key, demo views) — s_runtimeDir is set by
        // PreloadRuntime, well before kDataLoaded lands here.
        EnsureHostSettings();
        ApplyExceptionTelemetrySetting();
        InstallPolledKeyFilter();
        InstallGameCursorSkip();
        InstallCursorMenuPin();
        InstallCursorAdvanceHide();
        s_engineMode = !REL::Module::IsVR()
            && (s_compositeMode == "engine" || (s_compositeMode == "auto" && SmoothMotionLoaded()))
            && InstallEngineEndHook();
        if (s_engineMode) {
            s_engineComposite.store(true);
            SKSE::log::info("Magelight: engine mode - the engine's device, drawn from its end-of-frame call{}",
                SmoothMotionLoaded() ? " (NVIDIA Smooth Motion is loaded)" : "");
        }
        InstallLatePresent(vtbl, vtbl1);
        {
            // An untrusted wrapper (frame generation on Direct3D 12, set above) keeps drawing at Present, into the
            // engine's framebuffer: the UI pass has not been tried behind one.
            const HMODULE chain = ModuleAt(vtbl);
            RegisterOverlayMenu(ModuleBaseNameIs(chain, L"sl.interposer.dll")                ? SwapChainKind::kStreamline
                                : IsSystemModule(chain, L"dxgi.dll") || s_untrustedChain.load() ? SwapChainKind::kDxgi
                                                                                              : SwapChainKind::kWrapper);
        }
        RegisterFocusMenu();
        // Manifest mods (Data/Magelight/<ModId>/manifest.json): folders that
        // are mods. Registered through the v4 path like any DLL consumer.
        Manifest::LoadAll();

        // Demo views — DEV surfaces, created only when Magelight.json sets
        // "demoViews": true. With a real consumer (SeverActions) riding the
        // host, the always-on badge and the toggle-key playground are noise
        // for end users; with the flag on they still exercise the whole
        // pipeline (badge = click-through HUD anchor, playground = the
        // Vite/React file:/// shape the SA frontend ships in). Registered
        // before Ultralight initializes — they materialize on the render
        // thread once the world is up.
        if (s_demoViews) {
            s_playgroundId = CreateView("views/app/index.html", 48, 48, 720, 460);
            SetUIModeView(s_playgroundId);
            RegisterJSListener(s_playgroundId, "requestPageData", &OnRequestPageData);
            RegisterJSListener(s_playgroundId, "settingChanged", &OnSettingChanged);
            s_badgeId = CreateView("views/badge.html", -228, 12, 216, 44,
                                   nullptr, /*clickThrough=*/true, /*startVisible=*/true);
        }
        // ImageSource probe page — registered last so the toggle key opens it
        // when both dev flags are on. The texture itself is created lazily on
        // the render thread (TickImageProbe).
        if (s_imageProbe) {
            // Dogfood: the probe rides the v4 path (RegisterMod + CreateViewEx + events).
            s_probeViewId = Api4::CreateHostProbeView("views/probe/index.html", 40, 40, 1180, 860);
            SetUIModeView(s_probeViewId);
        }
    }

    // ── Input sink (toggle key + ALL mouse input) ───────────────────────────
    // Skyrim reads the mouse through exclusive DirectInput — the game window
    // never receives WM mouse messages (field-proven: zero across every run).
    // So while UI mode is up, mouse input comes from the game's own event
    // stream: kMouseMove events read the vanilla CursorMenu's position
    // (MenuCursor — the very cursor the user is looking at, in render-target
    // pixels) and mouse ButtonEvents map to synthetic WM_* entries in the same
    // queue the render thread already drains.

    // Cursor position source. MenuCursor is authoritative while the engine
    // drives it — but the engine stops updating it whenever a foreign engine
    // menu without kDontHideCursorWhenTopmost is topmost (SA's item-carrier
    // IMenu during the live mirror: field report 2026-09-01, cursor frozen on
    // the Outfits page). The device deltas keep arriving at the input sink
    // regardless, so: learn the engine's delta→pixel scale while MenuCursor
    // moves, detect a stall (deltas without movement), and integrate the raw
    // deltas ourselves until MenuCursor moves again. Input-sink thread only.
    static int   s_ownCursorX = 0, s_ownCursorY = 0;
    static int   s_lastMenuX = INT_MIN, s_lastMenuY = INT_MIN;
    static int   s_stallEvents = 0;
    static bool  s_integrating = false;
    static float s_deltaScale = 1.0f;                 // engine px per raw count, learned

    static LPARAM PackCursorPos(int dx = 0, int dy = 0)
    {
        int mx = 0, my = 0;
        if (auto* mc = RE::MenuCursor::GetSingleton()) {
            const auto& rd = mc->GetRuntimeData();
            mx = static_cast<int>(rd.cursorPosX);
            my = static_cast<int>(rd.cursorPosY);
        }
        const bool menuMoved = (mx != s_lastMenuX || my != s_lastMenuY);
        if (menuMoved) {
            // Learn the scale from real movement (both axes, EMA, clamped).
            if (s_lastMenuX != INT_MIN && (dx != 0 || dy != 0)) {
                const float rawLen  = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                const float menuLen = std::sqrt(static_cast<float>(
                    (mx - s_lastMenuX) * (mx - s_lastMenuX) + (my - s_lastMenuY) * (my - s_lastMenuY)));
                if (rawLen > 0.0f && menuLen > 0.0f) {
                    const float r = std::clamp(menuLen / rawLen, 0.1f, 10.0f);
                    s_deltaScale = 0.85f * s_deltaScale + 0.15f * r;
                }
            }
            s_lastMenuX = mx;
            s_lastMenuY = my;
            s_stallEvents = 0;
            if (s_integrating) {
                s_integrating = false;
                SKSE::log::info("Magelight: MenuCursor moving again — engine cursor authoritative");
            }
            s_ownCursorX = mx;
            s_ownCursorY = my;
        } else if (dx != 0 || dy != 0) {
            if (!s_integrating && ++s_stallEvents >= 12) {
                s_integrating = true;
                SKSE::log::info("Magelight: MenuCursor stalled under mouse input — integrating raw deltas (scale {:.2f})",
                    s_deltaScale);
            }
            if (s_integrating) {
                const int bw = s_backbufferW.load(), bh = s_backbufferH.load();
                s_ownCursorX = std::clamp(s_ownCursorX + static_cast<int>(std::lround(dx * s_deltaScale)),
                                          0, std::max(0, bw - 1));
                s_ownCursorY = std::clamp(s_ownCursorY + static_cast<int>(std::lround(dy * s_deltaScale)),
                                          0, std::max(0, bh - 1));
            }
        }
        // Mirror for the render thread's own cursor sprite.
        s_cursorPosX.store(s_ownCursorX);
        s_cursorPosY.store(s_ownCursorY);
        return MAKELPARAM(static_cast<WORD>(s_ownCursorX), static_cast<WORD>(s_ownCursorY));
    }

    // ── Gamepad layer (1.6.0, GamepadCore.h) ─────────────────────────────────
    // Every engine gamepad event of a batch goes through the router, with or without a view holding the pad: it
    // keeps which presses the game saw, so each release goes where its press went. A view holds the pad while it
    // holds UI mode, opted in, with its page's helper ready, the game in the foreground and no menu that takes menu
    // input above the focus menu. Flat only. The router and the plain statics are GamepadSink's: the engine's input
    // dispatch, one batch at a time, on whichever thread its input job runs.
    static Gamepad::Router            s_gamepadRouter;
    static Gamepad::BatchIn           s_gamepadIn;              // this batch's, for EndGamepadBatch
    static Gamepad::Mode              s_gamepadLastMode = Gamepad::Mode::Navigation;   // the last batch's out.mode
    static bool                       s_gamepadHostDirty = false;
    static std::uint64_t              s_gamepadSequence = 0;
    static ViewId                     s_gamepadPacketView = 0;  // the view the last packet went to
    static int                        s_gamepadBatches = 0, s_gamepadEmpty = 0;   // "trace"
    static double                     s_gamepadTraceAt = 0.0;
    static bool                       s_gamepadCursorDown = false;   // cursor mode's left press is down
    static bool                       s_mouseLeftInBatch = false;    // the real left button went down this batch
    static std::atomic<std::uint64_t> s_gamepadGeneration{ 0 };
    static std::atomic<bool>          s_gamepadConnected{ false };

    static bool ForegroundIsGame()
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    // A message box or another mod's menu over the page takes the pad: it is the menu that gets menu input. The menu
    // sink keeps the open menus that take menu input at or above the focus menu's depth (NoteMenuForGamepad); the
    // input sink reads only the count, since UI messages, which change the menu stack, can run on another thread.
    static std::mutex                      s_padMenusMutex;
    static std::unordered_set<std::string> s_padMenus;
    static std::atomic<int>                s_padMenuCount{ 0 };

    static bool MenuInputAboveFocusMenu() { return s_padMenuCount.load() > 0; }

    // Menu sink only: there the menu map may be read in place (MenuNamed).
    static void NoteMenuForGamepad(const RE::BSFixedString& name, bool opening)
    {
        const std::string_view n(name.c_str() ? name.c_str() : "");
        if (n.empty() || n == MagelightFocusMenu::MENU_NAME || n == MagelightOverlayMenu::MENU_NAME ||
            n == RE::CursorMenu::MENU_NAME)
            return;
        bool takesPad = false;
        if (opening) {
            auto* ui = RE::UI::GetSingleton();
            const RE::IMenu* m = ui ? MenuNamed(ui, name) : nullptr;
            takesPad = m && m->UsesMenuContext() && m->depthPriority >= MagelightFocusMenu::kDepth;
        }
        std::lock_guard<std::mutex> lk(s_padMenusMutex);
        if (takesPad) s_padMenus.insert(std::string(n));
        else s_padMenus.erase(std::string(n));
        s_padMenuCount.store(static_cast<int>(s_padMenus.size()));
    }

    // Queues the packet for `view` (GamepadCore.h PacketJson); active false tells a view it no longer holds the pad.
    static void SendGamepadPacket(ViewId view, bool active, const Gamepad::BatchOut& out)
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        if (!v || v->gamepadPage.empty()) return;
        // Calls wait for DOM ready (DrainBridgeQueues), and the page's own InteropCalls share their cap: input made
        // while the page loads is dropped, and the page gets the state once it is ready.
        if (!v->domReady || v->outbound.size() >= kOutboundCap) {
            v->gamepadDirty = true;
            return;
        }
        Gamepad::PacketIn p;
        p.page = v->gamepadPage;
        p.generation = out.generation;
        p.sequence = ++s_gamepadSequence;
        p.active = active;
        p.reset = out.reset || !active;
        p.enabled = v->gamepad.enabled && s_gamepadTuning.enabled;
        p.connected = s_gamepadConnected.load();
        p.style = v->gamepad.style;
        p.mode = active ? out.mode : v->gamepadMode;
        p.gamepadDevice = out.gamepadDevice;
        p.glyphs = s_gamepadTuning.glyphs;
        p.config = &v->gamepad;
        p.dt = out.dt;
        p.events = active ? &out.events : nullptr;
        QueueOutboundLocked(*v, { "__gamepad", Gamepad::PacketJson(p), false });
    }

    // Cursor mode's left press, released (invariant 24): at the end of a batch, or at once when a real mouse button
    // goes down, so the two presses on the one pointer never cross.
    static void ReleaseGamepadCursorPress()
    {
        if (!s_gamepadCursorDown) return;
        QueueInput(WM_LBUTTONUP, 0, PackCursorPos());
        s_gamepadCursorDown = false;
        s_mouseDown.store(false);
    }

    // False: no gamepad layer this batch (VR, or Magelight.json "gamepad" off), and the pad passes untouched.
    static bool BeginGamepadBatch(bool empty)
    {
        EnsureHostSettings();   // the sink runs from kInputLoaded, before Magelight's kDataLoaded reads them
        if (REL::Module::IsVR() || !s_gamepadTuning.enabled) return false;
        Gamepad::BatchIn in;
        in.now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        auto* mgr = RE::BSInputDeviceManager::GetSingleton();
        in.connected = mgr && mgr->IsGamepadConnected() && mgr->IsGamepadEnabled();
        // A pad that went away took its releases with it: a bit left set would keep the next press of that button
        // from being stamped, and the exit it closes would arm no controls hold.
        if (in.connected != s_gamepadConnected.load()) s_gamepadHeld.store(0);
        if (in.connected) {
            if (auto* pad = skyrim_cast<RE::BSWin32GamepadDevice*>(mgr->GetGamepad())) {
                const auto& g = pad->GetRuntimeData().currentState.gamepad;
                in.pad = { g.buttons, g.leftTrigger, g.rightTrigger, g.thumbLX, g.thumbLY, g.thumbRX, g.thumbRY };
            }
        }
        s_gamepadHostDirty = false;
        if (in.connected && s_focused.load() && !s_renderDead.load() && ForegroundIsGame() && !MenuInputAboveFocusMenu()) {
            const auto uiView = static_cast<ViewId>(s_uiModeView.load());
            // With pageThread the page thread can hold the registry across a page load: a batch whose view already
            // held the pad keeps the last batch's reading rather than wait (a gamepadDirty stays for the next one).
            // A view taking the pad waits, so its first presses never reach the game unowned.
            std::unique_lock<PageThread::OwnedMutex> lk(s_viewsMutex, std::defer_lock);
            bool busy = false;
            if (uiView != 0 && s_gamepadIn.owner == uiView) busy = !lk.try_lock();
            else lk.lock();
            if (busy) {
                in.owner = s_gamepadIn.owner;
                in.ownerEpoch = s_gamepadIn.ownerEpoch;
                in.config = s_gamepadIn.config;
                in.mode = s_gamepadLastMode;
                in.escapeCaptured = s_gamepadIn.escapeCaptured;
            } else if (MlView* v = FindViewLocked(uiView);
                       v && !v->destroyPending && v->gamepad.enabled && !v->gamepadPage.empty()) {
                in.owner = uiView;
                in.ownerEpoch = v->gamepadEpoch;
                in.config = v->gamepad;
                in.mode = v->gamepadMode;
                in.escapeCaptured = v->escapeCapture;
                s_gamepadHostDirty = v->gamepadDirty;
                v->gamepadDirty = false;
            }
        }
        s_gamepadRouter.tuning = s_gamepadTuning;
        s_gamepadRouter.Begin(in);
        if (s_gamepadKeyActivity.exchange(false)) s_gamepadRouter.OtherDeviceActivity();
        s_gamepadIn = in;
        if (s_gamepadTuning.trace) {
            ++s_gamepadBatches;
            if (empty) ++s_gamepadEmpty;
            if (in.now - s_gamepadTraceAt >= 1.0) {
                SKSE::log::info("Magelight: gamepad batches {} (empty {}) connected {} owner {} contexts [{}]",
                    s_gamepadBatches, s_gamepadEmpty, in.connected, s_gamepadRouter.Owner(), InputContextStackText());
                s_gamepadBatches = s_gamepadEmpty = 0;
                s_gamepadTraceAt = in.now;
            }
        }
        return true;
    }

    static void EndGamepadBatch()
    {
        const Gamepad::BatchOut out = s_gamepadRouter.End();
        const auto owner = static_cast<ViewId>(s_gamepadRouter.Owner());
        s_gamepadOwner.store(owner);
        s_gamepadGeneration.store(out.generation);
        s_gamepadConnected.store(s_gamepadIn.connected);
        s_gamepadLastMode = out.mode;
        if (s_gamepadPacketView && s_gamepadPacketView != owner) {   // its page cancels what it holds
            SendGamepadPacket(s_gamepadPacketView, false, out);
            s_gamepadPacketView = 0;
        }
        // Cursor mode moves the cursor the mouse moves (MenuCursor), so the page sees one pointer; every press it
        // sends gets its release (invariant 24), also when the view loses the pad. GamepadSink runs in the same input
        // dispatch as the engine's menu handling that moves MenuCursor for the mouse, just before it, so the two
        // writes are sequential.
        if (out.pointer.release || !owner) ReleaseGamepadCursorPress();
        s_gamepadHidesCursor.store(owner && out.mode == Gamepad::Mode::Navigation && out.gamepadDevice);
        if (!owner) return;
        if (out.close) {   // invariant 7's controls hold keeps B's release from the game
            SKSE::log::info("Magelight: gamepad cancel - leaving UI mode (view {})", owner);
            GameTask::Post([]() { SetUIMode(false); });
        }
        if (out.pointer.dx != 0.0f || out.pointer.dy != 0.0f) {
            const int bw = s_backbufferW.load(), bh = s_backbufferH.load();
            if (auto* mc = RE::MenuCursor::GetSingleton(); mc && bw > 0 && bh > 0) {
                const float scale = bh / 1080.0f;
                auto& rd = mc->GetRuntimeData();
                rd.cursorPosX = std::clamp(rd.cursorPosX + out.pointer.dx * scale, 0.0f, static_cast<float>(bw - 1));
                rd.cursorPosY = std::clamp(rd.cursorPosY + out.pointer.dy * scale, 0.0f, static_cast<float>(bh - 1));
                QueueInput(WM_MOUSEMOVE, 0, PackCursorPos());
            }
        }
        for (const bool down : out.pointer.buttons) {
            if (down == s_gamepadCursorDown) continue;
            // The real left button holds the pointer (InputSink queues this batch's press after us): no second down.
            if (down && (s_mouseDown.load() || s_mouseLeftInBatch)) continue;
            QueueInput(down ? WM_LBUTTONDOWN : WM_LBUTTONUP, 0, PackCursorPos());
            s_gamepadCursorDown = down;
            s_mouseDown.store(down);
        }
        for (const int vk : out.keys) QueueVirtualKey(vk);
        // Keys style: the right stick scrolls under the pointer; the fraction of a pixel carries to the next batch.
        static float s_padScrollRest = 0.0f;   // GamepadSink only
        if (out.pointer.scrollY == 0.0f) {
            s_padScrollRest = 0.0f;
        } else {
            s_padScrollRest += out.pointer.scrollY;
            const int px = static_cast<int>(s_padScrollRest);
            if (px != 0) {
                s_padScrollRest -= static_cast<float>(px);
                QueueInput(kPadScrollMsg, static_cast<WPARAM>(static_cast<std::intptr_t>(px)), PackCursorPos());
            }
        }
        if (!out.events.empty() || out.reset || out.stateChanged || s_gamepadHostDirty || s_gamepadPacketView != owner) {
            SendGamepadPacket(owner, true, out);
            s_gamepadPacketView = owner;
        }
        if (out.mode != s_gamepadIn.mode) {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            // Unless SetViewGamepad (any thread) set it since BeginGamepadBatch read it: the API's mode wins.
            if (MlView* v = FindViewLocked(owner); v && v->gamepadMode == s_gamepadIn.mode) v->gamepadMode = out.mode;
        }
    }

    class InputSink final : public RE::BSTEventSink<RE::InputEvent*> {
    public:
        static InputSink* GetSingleton()
        {
            static InputSink sink;
            return &sink;
        }
        RE::BSEventNotifyControl ProcessEvent(
            RE::InputEvent* const* a_events, RE::BSTEventSource<RE::InputEvent*>*) override
        {
            GameTask::Scope pumpScope;   // posts from here go through the pump (MagelightGameTask.h)
            if (!a_events) return RE::BSEventNotifyControl::kContinue;
            const bool focused = s_focused.load() && !s_renderDead.load();
            // While UI mode is on, keyboard button events END here: the page
            // already received the key through the window proc, and every
            // sink registered after ours (other mods' hotkeys) would
            // otherwise act on a key that was typed into a text field. The
            // event chain is relinked in place — the engine's own sinks run
            // before ours and are unaffected (menu context handles them).
            // Mouse events pass through; the gamepad is GamepadSink's, which
            // runs before the engine's sinks (1.6.0).
            RE::InputEvent* prev = nullptr;
            for (auto* e = *a_events; e; ) {
                RE::InputEvent* next = e->next;
                const auto type = e->GetEventType();
                if (type == RE::INPUT_EVENT_TYPE::kButton) {
                    const auto* b = e->AsButtonEvent();
                    if (!b) { prev = e; e = next; continue; }
                    if (b->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {
                        const auto code = b->GetIDCode();
                        bool consumed = false;   // toggle key / Escape / a bound hotkey: never typed
                        if (b->IsDown()) {
                            if (code == s_toggleKey.load()) { ToggleVisible(); consumed = true; }
                            else if (focused && code == 0x01) {
                                // Escape through the SINK: the window-proc
                                // Escape needs OS focus (VR mirror window,
                                // alt-tabbed game). Idempotent with it.
                                if (EscapeCapturedByUiView()) {
                                    // The page owns Escape (0.28.0). On VR the sink IS the
                                    // key path, so hand it the key; on flat the window proc
                                    // already queued it.
                                    if (VR::IsVRRuntime()) {
                                        QueueInput(WM_KEYDOWN, VK_ESCAPE, Input::SyntheticKeyLParam(VK_ESCAPE, false));
                                        QueueInput(WM_KEYUP, VK_ESCAPE, Input::SyntheticKeyLParam(VK_ESCAPE, true));
                                    }
                                } else {
                                    SKSE::log::info("Magelight: Escape via sink — leaving UI mode");
                                    GameTask::Post([]() { SetUIMode(false); });
                                }
                                consumed = true;
                            }
                            else consumed = Api4::DispatchHotkey(code);   // v4 / manifest hotkey bindings
                        } else if (code == s_toggleKey.load() || code == 0x01) {
                            consumed = true;   // the release of a key we swallowed on press
                        }
                        // VR only: the window gets no key messages, so the page
                        // is typed from the engine device instead. On flat the
                        // window proc already does this — never both.
                        // Presses and releases are noted outside UI mode too, so a key held
                        // across an entry never repeats into a page that did not get its press.
                        if (VR::IsLive()) {
                            const bool deliver = focused && !consumed;
                            if (b->IsDown()) {
                                NoteVrKeyPress(code, deliver);
                                if (deliver) QueueScancodeAsText(code, KeyPhase::Down);
                            } else if (!b->IsPressed()) {
                                NoteVrKeyPress(code, false);
                                if (deliver) QueueScancodeAsText(code, KeyPhase::Up);
                            } else if (deliver && VrKeyRepeatDue(code, b->HeldDuration())) {
                                QueueScancodeAsText(code, KeyPhase::Repeat);
                            }
                        }
                        if (focused) {
                            // unlink: keys typed into a page stay in the page
                            if (prev) prev->next = next;
                            else *const_cast<RE::InputEvent**>(a_events) = next;
                            e = next;
                            continue;
                        }
                        prev = e; e = next;
                        continue;
                    }
                    // VR controllers (kVivePrimary..kWMRSecondary): record the
                    // edge for the VR presenter's laser (VR-3) and give the
                    // headset a way OUT of UI mode — B/Y on either hand closes,
                    // the VR twin of Escape. Events pass through untouched.
                    {
                        const int dev = static_cast<int>(b->GetDevice());
                        if (dev >= static_cast<int>(RE::INPUT_DEVICE::kVivePrimary) &&
                            dev <= static_cast<int>(RE::INPUT_DEVICE::kWMRSecondary)) {
                            const auto vrcode = b->GetIDCode();
                            VR::NoteButton(dev, vrcode, b->Value());   // analog LEVEL, not IsDown (see MagelightVR.h)
                            if (focused && b->IsDown() && vrcode == RE::BSOpenVRControllerDevice::Keys::kBY) {
                                // A mod that bound B/Y (bare or chorded) for the
                                // UI-mode view owns open/close on it — its callback
                                // fires from the hotkey tick, and running our exit
                                // as well turned one Grip+B/Y into close + re-open
                                // (SA, VR field log 2026-09-12 14:20). Everyone else
                                // keeps the built-in way out.
                                const auto uiView = static_cast<ViewId>(s_uiModeView.load());
                                if (VR::ViewBindsButton(uiView, VR::kButtonApplicationMenu)) {
                                    SKSE::log::info("Magelight: controller B/Y (device {}) — view {} binds it; built-in exit skipped", dev, uiView);
                                } else {
                                    SKSE::log::info("Magelight: controller B/Y (device {}) — leaving UI mode", dev);
                                    GameTask::Post([]() { SetUIMode(false); });
                                }
                            }
                            // M7: while a page is focused the trigger CLICKS the
                            // panel (via the laser's queued WM_LBUTTON*) and must
                            // not also swing the weapon — unlink it so no later
                            // sink sees it. B/Y stays linked (its exit is queued
                            // above); every other control passes through.
                            if (focused && vrcode == RE::BSOpenVRControllerDevice::Keys::kTrigger) {
                                if (prev) prev->next = next;
                                else *const_cast<RE::InputEvent**>(a_events) = next;
                                e = next;
                                continue;
                            }
                            prev = e; e = next;
                            continue;
                        }
                    }
                    if (!focused || b->GetDevice() != RE::INPUT_DEVICE::kMouse) { prev = e; e = next; continue; }
                    if (VR::IsLive()) { prev = e; e = next; continue; }   // VR: the laser is the only pointer
                    using MK = RE::BSWin32MouseDevice::Key;
                    const LPARAM pos = PackCursorPos();
                    switch (b->GetIDCode()) {
                    case MK::kLeftButton:
                        if (b->IsDown()) { QueueInput(WM_LBUTTONDOWN, 0, pos); s_mouseDown.store(true); }
                        else if (b->IsUp()) { QueueInput(WM_LBUTTONUP, 0, pos); s_mouseDown.store(false); }
                        break;
                    case MK::kRightButton:
                        if (b->IsDown()) QueueInput(WM_RBUTTONDOWN, 0, pos);
                        else if (b->IsUp()) QueueInput(WM_RBUTTONUP, 0, pos);
                        break;
                    case MK::kMiddleButton:
                        if (b->IsDown()) QueueInput(WM_MBUTTONDOWN, 0, pos);
                        else if (b->IsUp()) QueueInput(WM_MBUTTONUP, 0, pos);
                        break;
                    case MK::kButton3:   // the side buttons, back then forward (DIMOUSESTATE2 order)
                    case MK::kButton4: {
                        const WORD x = b->GetIDCode() == MK::kButton3 ? XBUTTON1 : XBUTTON2;
                        if (b->IsDown()) {
                            QueueInput(WM_XBUTTONDOWN, MAKEWPARAM(0, x), pos);
                            static std::atomic<bool> s_logged{ false };
                            if (!s_logged.exchange(true))
                                SKSE::log::info("Magelight: mouse button {} pressed over a page (sent as {})",
                                    b->GetIDCode() == MK::kButton3 ? 4 : 5, x == XBUTTON1 ? "back" : "forward");
                        } else if (b->IsUp()) {
                            QueueInput(WM_XBUTTONUP, MAKEWPARAM(0, x), pos);
                        }
                        break;
                    }
                    case MK::kWheelUp:
                        if (b->IsDown()) QueueInput(WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), pos);
                        break;
                    case MK::kWheelDown:
                        if (b->IsDown())
                            QueueInput(WM_MOUSEWHEEL,
                                MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), pos);
                        break;
                    default:
                        break;
                    }
                } else if (focused && type == RE::INPUT_EVENT_TYPE::kMouseMove && !VR::IsLive()) {
                    const auto* mm = e->AsMouseMoveEvent();
                    QueueInput(WM_MOUSEMOVE, 0,
                        PackCursorPos(mm ? mm->mouseInputX : 0, mm ? mm->mouseInputY : 0));
                } else if (type == RE::INPUT_EVENT_TYPE::kThumbstick) {
                    if (const auto* ts = e->AsThumbstickEvent()) {
                        const int dev = static_cast<int>(ts->GetDevice());
                        if (dev >= static_cast<int>(RE::INPUT_DEVICE::kVivePrimary) &&
                            dev <= static_cast<int>(RE::INPUT_DEVICE::kWMRSecondary)) {
                            VR::NoteThumbstick(dev, ts->xValue, ts->yValue);
                            // M7: don't let the stick turn/move the player while a
                            // page is focused (VR-3.1 will map it to scroll).
                            if (focused) {
                                if (prev) prev->next = next;
                                else *const_cast<RE::InputEvent**>(a_events) = next;
                                e = next;
                                continue;
                            }
                        }
                    }
                }
                prev = e; e = next;
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // The gamepad layer's sink (1.6.0), put AHEAD of the engine's own input sinks (PrependEventSink): a pad event the
    // router consumes is unlinked before MenuControls and PlayerControls see it, so the game never acts on a button
    // the page owns. The engine names pad buttons from the top input context, which while a page is open is the
    // Cursor Menu's: B never arrives as Cancel there, and the gameplay names it falls back to reach PlayerControls.
    // Every engine pad event passes the router whether or not a view holds the pad, or its press/release bookkeeping
    // breaks. Mouse events are only noted here (the device in use) and stay InputSink's.
    class GamepadSink final : public RE::BSTEventSink<RE::InputEvent*> {
    public:
        static GamepadSink* GetSingleton()
        {
            static GamepadSink sink;
            return &sink;
        }
        RE::BSEventNotifyControl ProcessEvent(
            RE::InputEvent* const* a_events, RE::BSTEventSource<RE::InputEvent*>*) override
        {
            GameTask::Scope pumpScope;   // posts from here go through the pump (MagelightGameTask.h)
            if (!a_events || !BeginGamepadBatch(*a_events == nullptr)) return RE::BSEventNotifyControl::kContinue;
            s_mouseLeftInBatch = false;
            RE::InputEvent* prev = nullptr;
            for (auto* e = *a_events; e; ) {
                RE::InputEvent* next = e->next;
                bool consumed = false;
                const auto type = e->GetEventType();
                if (type == RE::INPUT_EVENT_TYPE::kButton) {
                    const auto* b = e->AsButtonEvent();
                    if (b && b->GetDevice() == RE::INPUT_DEVICE::kGamepad) {
                        if (s_gamepadTuning.trace && (b->IsDown() || b->IsUp()))
                            SKSE::log::info("Magelight: gamepad button {} value {:.2f} user event '{}' contexts [{}]",
                                b->GetIDCode(), b->Value(), b->QUserEvent().c_str() ? b->QUserEvent().c_str() : "",
                                InputContextStackText());
                        const auto control = Gamepad::FromSkseKeycode(SKSE::InputMap::GamepadMaskToKeycode(b->GetIDCode()));
                        NoteGamepadControl(control, b->Value() >= 0.1f);
                        consumed = s_gamepadRouter.Button(control, b->Value());
                    } else if (b && b->GetDevice() == RE::INPUT_DEVICE::kMouse && b->IsDown()) {
                        if (b->GetIDCode() == RE::BSWin32MouseDevice::Key::kLeftButton) s_mouseLeftInBatch = true;
                        s_gamepadRouter.OtherDeviceActivity();
                        ReleaseGamepadCursorPress();   // before InputSink queues this button's own press
                    }
                } else if (type == RE::INPUT_EVENT_TYPE::kMouseMove) {
                    const auto* mm = e->AsMouseMoveEvent();
                    if (mm && (mm->mouseInputX || mm->mouseInputY)) s_gamepadRouter.OtherDeviceActivity();
                } else if (type == RE::INPUT_EVENT_TYPE::kThumbstick) {
                    const auto* ts = e->AsThumbstickEvent();
                    if (ts && ts->GetDevice() == RE::INPUT_DEVICE::kGamepad)
                        consumed = s_gamepadRouter.Stick(
                            ts->IsLeft() ? Gamepad::Control::LeftStick : Gamepad::Control::RightStick, ts->xValue, ts->yValue);
                }
                if (consumed) {
                    if (prev) prev->next = next;
                    else *const_cast<RE::InputEvent**>(a_events) = next;
                } else {
                    prev = e;
                }
                e = next;
            }
            EndGamepadBatch();
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // Menu traffic while UI mode is on: which engine menus open and close
    // around us. The cursor only moves while a kUsesCursor menu is topmost,
    // so a stray open/close is the first thing to look at when it freezes.
    // Every menu's depth is noted the first time it opens (NoteMenuDepth).
    // The engine sends these from the thread processing UI messages, which is
    // not always the main thread (field logs show job threads on AE).
    class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        static MenuSink* GetSingleton() { static MenuSink s; return &s; }
        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent* e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (e && e->opening) {
                if (auto* ui = RE::UI::GetSingleton()) NoteMenuDepth(e->menuName, MenuNamed(ui, e->menuName));
            }
            if (e) NoteMenuForGamepad(e->menuName, e->opening);
            if (e && e->menuName == RE::MainMenu::MENU_NAME) s_mainMenuOpen.store(e->opening);
            if (e && !s_focused.load() && e->menuName.c_str() &&
                GetTickCount64() - s_uiModeExitedMs.load() < 3000) {
                const std::string_view name(e->menuName.c_str());
                if (name == RE::CursorMenu::MENU_NAME || name == MagelightFocusMenu::MENU_NAME ||
                    name == MagelightOverlayMenu::MENU_NAME)
                    SKSE::log::info("Magelight: menu '{}' {} after UI mode ended", name, e->opening ? "OPENED" : "CLOSED");
            }
            if (e && s_focused.load()) {
                SKSE::log::info("Magelight: menu '{}' {} while UI mode is on",
                    e->menuName.c_str() ? e->menuName.c_str() : "?", e->opening ? "OPENED" : "CLOSED");
                // A reopened Cursor Menu is a new movie, shown: hide it now, not at the next once-a-second pass. The
                // sink can run off the main thread, so the hide is posted (and FrameWork repeats it a few frames);
                // with skipGameCursor the sprite is not drawn meanwhile anyway.
                // With the AdvanceMovie hook, a new movie may sit at the old one's address: its root is hidden again.
                if (e->opening && e->menuName.c_str() && CursorHiddenInAdvance() &&
                    std::string_view(e->menuName.c_str()) == RE::CursorMenu::MENU_NAME)
                    s_cursorRootStale.store(true);
                if (e->opening && e->menuName.c_str() && !CursorHiddenInAdvance() &&
                    std::string_view(e->menuName.c_str()) == RE::CursorMenu::MENU_NAME && !REL::Module::IsVR()) {
                    GameTask::Scope scope;
                    s_cursorRehideFrames.store(8);
                    GameTask::Post([]() { if (s_focused.load()) HideVanillaCursor(true); });
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // Game thread. If the engine dropped the CursorMenu under us while UI
    // mode is on, put it back (and say so) — without it the MenuCursor
    // position never updates and our cursor sprite stands still. Without
    // Hook_CursorAdvanceMovie it also keeps the vanilla sprite hidden (HideVanillaCursor).
    static void CursorMenuWatchdog()
    {
        if (!s_focused.load()) return;
        auto* ui = RE::UI::GetSingleton();
        auto* q  = RE::UIMessageQueue::GetSingleton();
        if (!ui || !q) return;
        if (!ui->IsMenuOpen(RE::CursorMenu::MENU_NAME)) {
            SKSE::log::warn("Magelight: CursorMenu is not open while UI mode is on — re-showing it");
            q->AddMessage(RE::CursorMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
        }
        if (s_focusMenuRegistered && !ui->IsMenuOpen(MagelightFocusMenu::MENU_NAME)) {
            SKSE::log::warn("Magelight: focus menu is not open while UI mode is on — re-showing it");
            q->AddMessage(MagelightFocusMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
        }
        // A second cursor over a page (field reports): the game's sprite shown again after it was hidden (a cursor
        // replacer's own script, another mod), or Windows' own cursor over the game. The first few each session.
        if (!REL::Module::IsVR()) {
            static int s_spriteReshown = 0;
            static int s_windowsShown = 0;
            bool shows = false;
            const void* movie = nullptr;
            const std::string sprite = DescribeCursorSprite(ui, &shows, &movie);
            if (!CursorHiddenInAdvance() && shows && movie && movie == s_hiddenCursorMovie.load() && s_spriteReshown < 5) {
                ++s_spriteReshown;
                if (GameCursorSkipped()) {
                    SKSE::log::info("Magelight: something showed the game's cursor again while a page holds the mouse "
                                    "({}); its draw is skipped, so it stays off the page", sprite);
                } else {
                    SKSE::log::warn("Magelight: something showed the game's cursor again while a page holds the mouse "
                                    "({}); hiding it again", sprite);
                }
            }
            static bool s_skipLogged = false;
            if (const auto skipped = s_gameCursorSkips.exchange(0); skipped && !s_skipLogged) {
                s_skipLogged = true;
                SKSE::log::info("Magelight: the game's cursor draw is being skipped while a page holds the mouse "
                                "({} draws in the last second)", skipped);
            }
            CURSORINFO ci;
            if (WindowsCursorOverGame(ci) && s_windowsShown < 3) {
                ++s_windowsShown;
                const auto* mc = RE::MenuCursor::GetSingleton();
                SKSE::log::warn("Magelight: Windows' own cursor shows over the game while a page holds the mouse "
                                "(showCursorCount {}, {} cursor)", mc ? mc->GetRuntimeData().showCursorCount : INT_MIN,
                                ci.hCursor == LoadCursor(nullptr, IDC_ARROW) ? "the arrow" : "another");
            }
            LogMenusOverPages(false);
        }
        HideVanillaCursor(true);
        // OCU re-sets OC_MENU_ACTIVE on every menu open/close; keep it clear
        // for as long as our page owns the pointer.
        if (s_hwnd && VR::SuppressRuntimeLaser())
            SetPropW(s_hwnd, L"OC_MENU_ACTIVE", reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(0)));
    }

    // ── Keyboard mute while UI mode is on ───────────────────────────────
    // The sink unlink above shields only the sinks registered after ours.
    // Mods with a "priority input hook" (Immersive Equipment Displays hooks
    // the dispatcher itself and opens its UI on Backspace, EnableInMenus or
    // not — field 2026-09-03) see the keys before any sink. The engine's
    // keyboard device turns DirectInput state into button events in its
    // Poll (BSIInputDevice vfunc 02); while a page is focused that poll is
    // skipped, so NO keyboard button event exists engine-wide — nothing for
    // a hook, a sink, or a Papyrus OnKeyDown to act on. The page keeps
    // typing through the window proc (WM_KEYDOWN / WM_CHAR are the OS, not
    // DirectInput), and Escape / bound hotkeys keep working through the
    // same path. Mouse and gamepad devices are untouched. While muted the
    // hook still DRAINS DirectInput's buffered queue into the engine's state
    // arrays (see the body) — skipping the read outright let the typed keys
    // replay into the game on unfocus.
    namespace {
        using KeyboardPollFn = void(*)(RE::BSWin32KeyboardDevice*, float);
        KeyboardPollFn s_origKeyboardPoll = nullptr;
        Input::LostKeys s_lostKeys;        // the poll's thread
        std::uint32_t   s_lostKeysSession = 0;

        std::string KeyWindowForLog(HWND h)
        {
            if (!h) return "none";
            char cls[64]{};
            GetClassNameA(h, cls, sizeof(cls));
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            return std::format("{:#x} {}{}", reinterpret_cast<std::uintptr_t>(h), cls,
                               h == s_hwnd ? " (the game's)" : pid == GetCurrentProcessId() ? "" : " (another process)");
        }

        // Where Windows sends keys right now. Any thread.
        void LogKeyFocus(const char* when)
        {
            static std::atomic<int> s_left{ 40 };
            if (s_left.fetch_sub(1) <= 0) return;
            GUITHREADINFO g{};
            g.cbSize = sizeof(g);
            const DWORD thread = s_hwnd ? GetWindowThreadProcessId(s_hwnd, nullptr) : 0;
            const bool read = thread && GetGUIThreadInfo(thread, &g);
            SKSE::log::info("Magelight: keyboard {}: foreground {}; window thread's focus {}, active {}, capture {}{}",
                            when, KeyWindowForLog(GetForegroundWindow()), KeyWindowForLog(g.hwndFocus),
                            KeyWindowForLog(g.hwndActive), KeyWindowForLog(g.hwndCapture),
                            read ? "" : " (thread state not readable)");
        }

        // Forward for Input::LostKeys.
        bool PassLostKey(std::uint8_t scan, bool down)
        {
            if (!s_hwnd || GetForegroundWindow() != s_hwnd) return false;
            const bool numLock = s_lostKeys.Passed() ? (s_passedBits.load() & Input::kKbNumLock) != 0
                                                     : (MlGetKeyState(VK_NUMLOCK) & 1) != 0;
            const Input::ScancodeKey key = Input::FromScancode(scan, numLock);
            if (!key.vk) return false;
            const LPARAM l = key.lparam | 1 | (down ? 0 : (static_cast<LPARAM>(3) << 30));
            if (!PostMessageW(s_hwnd, kPassedKeyMsg, key.vk, l)) return false;
            if (down && s_lostKeys.Passed() == 0) {
                LogKeyFocus("key lost");
                PostMessageW(s_hwnd, kKeyFocusMsg, 0, 0);
            }
            static std::atomic<int> s_left{ 30 };
            if (down && s_left.fetch_sub(1) > 0)
                SKSE::log::info("Magelight: key scan 0x{:02X} reached DirectInput but not the window - passed on", scan);
            return true;
        }

        // A change in what the device answers while muted: DIERR_NOTACQUIRED after an alt-tab, say.
        void LogKeyDeviceRead(std::int32_t hr)
        {
            static std::int32_t s_last = 1;
            static int s_left = 24;
            if (hr == s_last) return;
            s_last = hr;
            if (s_left-- > 0)
                SKSE::log::info("Magelight: keyboard device read answers {:#x}", static_cast<std::uint32_t>(hr));
        }

        void Hook_KeyboardPoll(RE::BSWin32KeyboardDevice* self, float dt)
        {
            // VR (0.17.3): the mute is NOT applied. Typing, Escape and the
            // close hotkey ride the window proc, which only delivers while
            // the game window owns OS focus — and the SkyrimVR desktop mirror
            // rarely does (field 2026-09-03: F10 opened the panel through the
            // sink, the mute then silenced the only path that could close
            // it). On VR the sink is the keyboard path: it handles Escape
            // and the view's own hotkey below and still unlinks every key
            // for the sinks after ours.
            if (s_focused.load() && !s_renderDead.load() && !VR::IsVRRuntime()) {
                // MUTED — but never idle. The engine reads DirectInput's BUFFERED
                // event queue (GetDeviceData, ten at a time) and applies it to
                // curState; a skipped Poll leaves every key typed into a page
                // queued in DirectInput, and the first real Poll after unfocus
                // replayed the lot into the game (field 2026-09-06: search-box
                // keystrokes opened journal/map/etc. the moment the menu closed).
                // Drain the queue ourselves and apply each event to BOTH state
                // arrays: the engine's held-key picture stays truthful, its
                // unmute diff has nothing to fire, and no button event is ever
                // generated for a key typed into a page. Both arrays carry
                // static offset asserts in CommonLib. (0.27.0)
                if (self) {
                    const std::uint64_t now = GetTickCount64();
                    // A key in the session's first read may already have been through the window, before Begin.
                    const auto session = s_keySession.load();
                    const bool firstRead = session != s_lostKeysSession;
                    if (firstRead) {
                        s_lostKeysSession = session;
                        s_lostKeys.Begin(s_windowKeyPresses.load());
                        LogKeyFocus("at UI mode entry");
                    }
                    auto& rd = self->GetRuntimeData();
                    if (auto* dev = reinterpret_cast<REX::W32::IDirectInputDevice8A*>(rd.dInputDevice)) {
                        REX::W32::DIDEVICEOBJECTDATA buf[32];
                        for (int guard = 0; guard < 16; ++guard) {
                            std::uint32_t n = 32;
                            const auto hr = dev->GetDeviceData(sizeof(REX::W32::DIDEVICEOBJECTDATA), buf, &n, 0);
                            LogKeyDeviceRead(static_cast<std::int32_t>(hr));
                            if (hr < 0 || n == 0) break;   // not acquired / input lost: nothing to drain
                            for (std::uint32_t i = 0; i < n; ++i) {
                                const std::uint32_t k = buf[i].ofs;
                                if (k >= 0x100) continue;
                                const bool down = (buf[i].data & 0x80) != 0;
                                rd.curState[k] = rd.prevState[k] = down ? 0x80 : 0;
                                if (!firstRead) s_lostKeys.Note(static_cast<std::uint8_t>(k), down, now);
                            }
                            if (n < 32) break;
                        }
                    }
                    if (s_keyFallback) {
                        const bool heard = s_lostKeys.WindowHeard();
                        s_lostKeys.Poll(s_windowKeyPresses.load(), now, PassLostKey);
                        if (!heard && s_lostKeys.WindowHeard() && s_lostKeys.Passed() > 0)
                            SKSE::log::info("Magelight: the window hears keys again, after {} passed on",
                                            s_lostKeys.Passed());
                    }
                }
                return;
            }
            if (s_origKeyboardPoll) s_origKeyboardPoll(self, dt);
        }

        void InstallKeyboardMute()
        {
            static bool s_done = false;
            if (s_done) return;
            s_done = true;
            // NOT ON VR (0.19.2). The mute exists to stop a key typed into a
            // page reaching other mods, and on VR it is already skipped in
            // Hook_KeyboardPoll (0.17.3: the window gets no key messages there,
            // so the sink IS the typing path). Installing it anyway left a
            // permanent vtable patch on a VR runtime for no behaviour at all —
            // and a vtable write is the one thing in our always-on path that
            // could corrupt memory if the address-library id resolved wrongly.
            // The sink's unlink still contains keys while a page is focused.
            if (REL::Module::IsVR()) {
                SKSE::log::info("Magelight: keyboard device poll NOT hooked (VR — the sink is the typing path)");
                return;
            }
            REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_BSWin32KeyboardDevice[0] };
            s_origKeyboardPoll = reinterpret_cast<KeyboardPollFn>(vtbl.write_vfunc(0x2, &Hook_KeyboardPoll));
            SKSE::log::info("Magelight: keyboard device poll hooked (muted while UI mode is on)");
        }
    }

    void RegisterInputSink()
    {
        InstallKeyboardMute();
        // Registered as early as the device manager exists (kInputLoaded):
        // sinks run in registration order, and the keyboard unlink above
        // only shields the sinks that come AFTER ours.
        static bool s_registered = false;
        if (s_registered) return;
        if (auto* mgr = RE::BSInputDeviceManager::GetSingleton()) {
            mgr->AddEventSink(InputSink::GetSingleton());
            mgr->PrependEventSink(GamepadSink::GetSingleton());   // ahead of MenuControls and PlayerControls
            s_registered = true;
            SKSE::log::info("Magelight: input sink registered (Page Up toggles UI mode)");
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink(MenuSink::GetSingleton());
        }
    }

    void ToggleVisible()
    {
        // UI-mode toggle (visible + focused + cursor + suspended game
        // controls, all together). Marshalled to the game thread — the sink
        // fires on the input-dispatch path. With no UI-mode view registered
        // (demo views off, no consumer called SetUIModeView) entering would
        // suspend controls over an empty screen — refuse instead. Exits
        // always go through so the key can never strand a stuck mode.
        GameTask::Post([]() {
            if (s_focused.load()) {
                SetUIMode(false);  // exit always works — never a stuck mode
                return;
            }
            const ViewId target = static_cast<ViewId>(s_toggleView.load());
            if (!IsViewValid(target)) {
                SKSE::log::info("Magelight: toggle ignored — no toggle view registered");
                return;
            }
            s_uiModeView.store(target);
            {
                std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
                QueueKeyboardThemeLocked();   // the hidden keyboard page takes this view's theme ahead of a show (0.31.9)
            }
            SetUIMode(true);
        });
    }

    void ForceExitUIMode()
    {
        // Safety valve for load boundaries: never carry a suspended-controls /
        // cursor-up state across a save load, a VR button hold included. Game thread.
        s_exitWithoutHold = true;
        SetUIMode(false);
        s_exitWithoutHold = false;
        EndControlsHold("load boundary", s_heldGen.load());
    }

    void EnterUIMode(ViewId view) { EnterUIModeEx(view, false); }

    // Posted UI-mode entries and exits that have not run yet (1.1.0). Any thread; a leaf lock, never held while
    // taking another.
    static std::mutex s_uiTicketMutex;
    static std::unordered_set<std::uint64_t> s_uiTickets;
    static std::uint64_t s_uiTicketSeq = 0;   // under s_uiTicketMutex

    std::uint64_t NewUIModeTicket()
    {
        std::lock_guard<std::mutex> lk(s_uiTicketMutex);
        const std::uint64_t ticket = ++s_uiTicketSeq;
        s_uiTickets.insert(ticket);
        return ticket;
    }

    bool CancelUIModeTicket(std::uint64_t ticket)
    {
        if (!ticket) return false;
        std::lock_guard<std::mutex> lk(s_uiTicketMutex);
        return s_uiTickets.erase(ticket) != 0;
    }

    // Game thread, first thing in a ticketed task: false when the ticket was withdrawn before the task ran.
    static bool TakeUIModeTicket(std::uint64_t ticket)
    {
        if (!ticket) return true;
        std::lock_guard<std::mutex> lk(s_uiTicketMutex);
        return s_uiTickets.erase(ticket) != 0;
    }

    void EnterUIModeEx(ViewId view, bool pauseGame, bool noTextEntry, std::uint64_t ticket)
    {
        GameTask::Post([view, pauseGame, noTextEntry, ticket]() {
            if (!TakeUIModeTicket(ticket)) {
                // Withdrawn by CancelUIModeRequest: no entry, and so no UIModeEntered or UIModeRefused.
                SKSE::log::info("Magelight: EnterUIMode({}) withdrawn before it ran", view);
                return;
            }
            if (s_focused.load()) {
                // One surface at a time — callers gate on IsUIModeActive()
                // (SA's mutual-exclusion contract); refuse rather than steal.
                SKSE::log::warn("Magelight: EnterUIMode({}) refused — UI mode already active on view {}",
                    view, s_uiModeView.load());
                Emit(HostEvent::UIModeRefused, view, 0, 0, "UI mode already active");
                return;
            }
            const char* refusal = nullptr;
            {
                std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
                MlView* v = FindViewLocked(view);
                if (!v || v->destroyPending) refusal = "no such view";
                else if (v->clickThrough) refusal = "click-through views cannot take UI mode";
            }
            if (refusal) {
                SKSE::log::warn("Magelight: EnterUIMode({}) — {}", view, refusal);
                Emit(HostEvent::UIModeRefused, view, 0, 0, refusal);
                return;
            }
            s_uiModeView.store(view);
            {
                std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
                QueueKeyboardThemeLocked();   // the hidden keyboard page takes this view's theme ahead of a show (0.31.9)
            }
            SetUIMode(true, true, pauseGame, noTextEntry);
        });
    }

    void ExitUIMode(std::uint64_t ticket)
    {
        SKSE::log::info("Magelight: UI mode release asked (view {}{})", static_cast<std::uint64_t>(s_uiModeView.load()),
                        ticket ? ", the holder's own release" : "");
        // Deliberately does NOT hide the view (Magelight.h contract — SA's
        // Free Look keeps the view rendered while the game owns input;
        // callers that want a hide pair this with ShowView(view, false)).
        GameTask::Post([ticket]() {
            if (!TakeUIModeTicket(ticket)) {
                // Reclaimed by its own mod's next request (kUIModeFlagReclaim): UI mode stays, no UIModeExited.
                SKSE::log::info("Magelight: ExitUIMode withdrawn before it ran (the holder asked again)");
                return;
            }
            SetUIMode(false, /*hideView=*/false);
        });
    }

    bool IsUIModeActive()
    {
        return s_focused.load() && !s_renderDead.load();
    }

    ViewId GetUIModeView()
    {
        return static_cast<ViewId>(s_uiModeView.load());
    }

    std::uint32_t GetToggleKey()
    {
        return s_toggleKey.load();
    }

    std::filesystem::path RuntimeDirPath()
    {
        return s_runtimeDir;
    }

    std::filesystem::path CacheDirPath()
    {
        if (auto dir = SKSE::log::log_directory()) return *dir / "Magelight-cache";
        return {};
    }

    bool DevModeEnabled()
    {
        return s_devMode.load();
    }

    void RequestMemoryReport()
    {
        s_memReportRequested.store(true);
    }

    bool InspectorAvailable()
    {
        std::error_code ec;
        return std::filesystem::exists(s_runtimeDir / "inspector" / "Main.html", ec);
    }

    // ── Version gate ──────────────────────────────────────────────────────
    static std::atomic<std::uint64_t> s_gateView{ 0 };
    static void GateDomReady(ViewId view) { InteropCall(view, "setGate", Api4::TooOldJson()); }
    static void GateDismiss(const char*) { if (const ViewId v = static_cast<ViewId>(s_gateView.load())) ShowView(v, false); }

    void ShowVersionGateIfNeeded()
    {
        static bool shown = false;
        if (shown) return;
        const std::string json = Api4::TooOldJson();
        if (json.empty()) return;
        shown = true;
        const ViewId id = CreateView("views/gate/index.html", 40, 120, 640, 200, &GateDomReady,
                                     /*clickThrough=*/true, /*startVisible=*/true);
        if (!id) return;
        SetViewLayer(id, 3);
        RegisterJSListener(id, "gateDismiss", &GateDismiss);
        s_gateView.store(id);
        SKSE::log::warn("Magelight: version gate shown — {}", json);
    }

    void SetUIModePause(bool pause)
    {
        // IN PLACE — never hide/re-show the focus menu (0.26.10 did, and the
        // holder's close detection read the CLOSED as the engine taking the
        // menu away: SeverActions' Live Stage auto-closed the whole UI the
        // moment it asked to unpause, field 2026-09-06). The engine's pause is
        // UI::numPausesGame, bumped by menus carrying kPausesGame on open and
        // dropped on close — so flip the flag on the LIVE menu and move the
        // counter by hand; the eventual close then balances against the flag
        // it sees at that moment.
        const bool was = s_focusMenuPause.exchange(pause);
        if (!s_focused.load() || was == pause) return;   // unfocused: the next Create picks it up
        // The RE::UI part on the GAME thread (review 2026-09-09): RequestUIMode
        // is any-thread by contract and its sibling posts a task; walking the
        // menu map and moving numPausesGame from a worker thread would race the
        // engine's own open/close bookkeeping. Queued AFTER a view switch on the
        // same request, so the pause lands on the view that ends up holding.
        GameTask::Post([pause]() {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) return;
            auto menu = ui->GetMenu(MagelightFocusMenu::MENU_NAME);
            if (!menu) { SKSE::log::info("Magelight: UI mode pause -> {} (focus menu not open; next show carries it)", pause); return; }
            using F = RE::UI_MENU_FLAGS;
            const bool has = menu->menuFlags.all(F::kPausesGame);
            if (pause && !has) {
                menu->menuFlags.set(F::kPausesGame);
                ++ui->numPausesGame;
            } else if (!pause && has) {
                DropFreezeWorld(menu.get(), "pause off");   // never a freeze over a running game
                menu->menuFlags.reset(F::kPausesGame);
                if (ui->numPausesGame > 0) --ui->numPausesGame;
            }
            SKSE::log::info("Magelight: UI mode pause -> {} in place (numPausesGame={})", pause, ui->numPausesGame);
            if (pause) ApplyFreezeWorld("pause on");
        });
    }

    void SwitchUIModeView(ViewId view)
    {
        GameTask::Post([view]() {
            if (!s_focused.load()) {
                Emit(HostEvent::UIModeRefused, view, 0, 0, "switch: UI mode is not active");
                return;
            }
            // As a request at the main menu: the view it moves to becomes a main-menu page.
            if (s_mainMenuOpen.load() && MainMenuPagesAvailable()) SetViewMainMenu(view, true);
            const char* refusal = nullptr;
            {
                std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
                MlView* v = FindViewLocked(view);
                if (!v || v->destroyPending) refusal = "switch: no such view";
                else if (v->clickThrough) refusal = "switch: click-through views cannot take UI mode";
                else if (!ViewShows(*v)) refusal = "switch: view does not show here";
            }
            if (refusal) {
                SKSE::log::warn("Magelight: SwitchUIModeView({}) — {}", view, refusal);
                Emit(HostEvent::UIModeRefused, view, 0, 0, refusal);
                return;
            }
            const ViewId old = static_cast<ViewId>(s_uiModeView.load());
            if (old == view) { Emit(HostEvent::UIModeSwitched, view); return; }
            s_uiModeView.store(view);
            {
                std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
                QueueKeyboardThemeLocked();   // the keyboard page, up or not, follows the new view's theme (0.31.9)
            }
            ApplyFreezeWorld("view switch");   // follows the new view's preference (the inspector has none)
            ShowView(view, true);
            QueueInput(0, 1, 0);   // re-run the focus marker: key focus follows s_uiModeView
            s_tooltipLeaveAll.store(true);
            InteropCall(old, "__uimode", "0");
            InteropCall(view, "__uimode", "1");
            strncpy_s(s_uiModeLabel, Api4::ViewLabel(view).c_str(), _TRUNCATE);
            SKSE::log::info("Magelight: UI mode switched from view {} to {}", old, s_uiModeLabel);
            Emit(HostEvent::UIModeSwitched, view);
        });
    }

    void SetUIModeExitCallback(UIModeExitFn cb)
    {
        // Registrations accumulate (one per function pointer); v1-v3 have
        // no unregister — the v4 per-mod event sink replaces this surface.
        if (!cb) return;
        std::lock_guard<std::mutex> lock(s_uiModeExitMutex);
        if (std::find(s_uiModeExitCbs.begin(), s_uiModeExitCbs.end(), cb) ==
            s_uiModeExitCbs.end()) {
            s_uiModeExitCbs.push_back(cb);
        }
    }

    void NotifyWorldReady()
    {
        if (!s_worldReady.exchange(true)) {
            SKSE::log::info("Magelight: world ready — Ultralight and the pages start on the next in-game frame, if not already");
        }
    }

    ViewId CreateView(const char* htmlPath, int x, int y, int w, int h,
                      DomReadyFn onDomReady, bool clickThrough, bool startVisible,
                      const char* sessionName, NetLevel netLevel)
    {
        // w == 0 && h == 0 = FULLSCREEN: the view tracks the backbuffer size
        // (sized on the render thread before materialization, for SA's
        // implicitly-fullscreen self-scaling pages).
        const bool fullscreen = (w == 0 && h == 0);
        if (!fullscreen && (w <= 0 || h <= 0)) return 0;
        auto v = std::make_unique<MlView>();
        v->id = s_nextViewId.fetch_add(1);
        v->htmlPath = htmlPath ? htmlPath : "";
        v->root = PageRootFor(v->htmlPath);
        NotePageRoot(v->root, netLevel);   // 0.28.1: MlFileSystem serves it too, and writes its CSP
        v->x = x; v->y = y; v->w = w; v->h = h;
        v->fullscreen = fullscreen;
        v->visible = startVisible;
        v->loadNow = startVisible;   // visible at creation counts as shown: skips the load queue
        v->clickThrough = clickThrough;
        v->onDomReady = onDomReady;
        v->sessionName = sessionName ? sessionName : "";
        v->netLevel = netLevel;
        const ViewId id = v->id;
        v->order = s_nextViewOrder.fetch_add(1);
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            s_views.push_back(std::move(v));
            SortViewsLocked();
        }
        if (fullscreen)
            SKSE::log::info("Magelight: view {} registered ('{}' fullscreen{})",
                id, htmlPath ? htmlPath : "", clickThrough ? ", click-through" : "");
        else
            SKSE::log::info("Magelight: view {} registered ('{}' {}x{} at {},{}{})",
                id, htmlPath ? htmlPath : "", w, h, x, y, clickThrough ? ", click-through" : "");
        return id;
    }

    bool IsViewValid(ViewId view)
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        return v && !v->destroyPending;
    }

    void ShowView(ViewId view, bool show)
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) ShowViewLocked(*v, show);
    }

    void SetViewCutout(ViewId view, int x, int y, int w, int h)
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            if (w <= 0 || h <= 0) { v->cutW = v->cutH = 0; return; }
            v->cutX = static_cast<float>(x); v->cutY = static_cast<float>(y);
            v->cutW = static_cast<float>(w); v->cutH = static_cast<float>(h);
        }
    }

    void SetViewScale(ViewId view, float scale)
    {
        // Below 1, Ultralight paints only the top-left scale-squared part of the page (0.8 draws the top-left 64% of
        // each side) unless the compositor is on (kEnableCompositor). With it on the floor stays until a scale below
        // 1 has run in game; a smaller page takes a CSS transform instead.
        if (scale < 1.0f)
            SKSE::log::info("Magelight: view {} device scale {:.3f} raised to 1.000 (below 1 is not supported)", view, scale);
        scale = std::clamp(scale, 1.0f, 3.0f);
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        if (!v || v->destroyPending) return;
        v->deviceScale = scale;
        if (!v->ul) {
            // Not loaded yet (CreateViewEx passes ViewDesc::uiScale here), or hibernated: the next load creates its
            // View at this scale. A line of its own, so a live change is never read into one.
            SKSE::log::info("Magelight: view {} device scale {:.3f} (applies at its next load)", view, scale);
            return;
        }
        // Applied by ApplyPendingResizes on the render thread, the only thread that may call Ultralight
        // (invariant 1); this is the API caller's thread. It makes no call when the View already has this scale.
        v->scaleDirty  = true;
        v->boundsDirty = true;
        SKSE::log::info("Magelight: view {} device scale {:.3f}", view, scale);
    }

    bool RebuildViewAtScale(ViewId view, float scale)
    {
        if (scale < 1.0f)
            SKSE::log::info("Magelight: view {} rebuild scale {:.3f} raised to 1.000 (below 1 is not supported)", view, scale);
        scale = std::clamp(scale, 1.0f, 3.0f);
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        if (!v || v->destroyPending) return false;
        v->deviceScale = scale;
        if (!v->ul) {
            // Not loaded yet, or dormant: the next load creates its View at this scale. A page that loaded before
            // reloads then, so ViewReloaded follows its DOM ready as for any rebuild.
            if (v->dormant) { v->reloadedFlag = true; v->rebuilt = true; }
            SKSE::log::info("Magelight: view {} device scale {:.3f} (applies at its next load)", view, scale);
            return true;
        }
        if (std::fabs(v->liveScale - scale) < 0.0005f) {
            // Already the View's scale: withdraw a pending rebuild or SetViewScale aimed elsewhere.
            v->rebuildPending = false;
            v->scaleDirty = false;
            SKSE::log::info("Magelight: view {} already at device scale {:.3f} - no rebuild", view, scale);
            return true;
        }
        v->scaleDirty = false;   // no live scale change on this View: ApplyPendingRebuilds replaces it
        v->rebuildPending = true;
        SKSE::log::info("Magelight: view {} rebuild at device scale {:.3f} requested", view, scale);
        return true;
    }

    void SetViewHibernate(ViewId view, std::uint32_t idleMs)
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) v->hibernateMs = idleMs;
    }

    void SetViewBounds(ViewId view, int x, int y, int w, int h)
    {
        if (w <= 0 || h <= 0) return;
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->fullscreen = false;  // explicit bounds end backbuffer tracking
            v->x = x; v->y = y;
            if (v->w != w || v->h != h) {
                v->w = w; v->h = h;
                v->boundsDirty = true;  // render thread applies the Resize
            }
        }
    }

    // Names the host's page script owns (see RegisterJSListener in Magelight.h).
    static bool RefuseReservedListenerName(ViewId view, const char* name)
    {
        if (!(name[0] == '_' && name[1] == '_') && std::strcmp(name, "magelight") != 0) return false;
        static std::mutex s_mx;
        static std::set<std::string> s_logged;
        bool log = false;
        {
            std::lock_guard<std::mutex> lk(s_mx);
            log = s_logged.size() < 64 && s_logged.insert(name).second;
        }
        if (log) SKSE::log::warn("Magelight: listener name '{}' refused (view {}) — 'magelight' and names starting "
                                 "with '__' belong to the host's page script (logged once per name)", name, view);
        return true;
    }

    bool RegisterJSListener(ViewId view, const char* name, JsListenerFn callback)
    {
        if (!name || !*name || !callback) return false;
        if (RefuseReservedListenerName(view, name)) return false;
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->listeners[name] = MlView::Listener{ callback, nullptr, nullptr };
            // A registration after window-object-ready (SA's, from OnDomReady)
            // marks the view: the render thread re-creates __mlNative and every
            // listener shim, leaving window.magelight and its subscriptions alone.
            v->shimsDirty = true;
            return true;
        }
        return false;
    }

    void InteropCall(ViewId view, const char* functionName, const std::string& argument)
    {
        if (!functionName || !*functionName) return;
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) QueueOutboundLocked(*v, { functionName, argument, false });
    }

    void EvalJS(ViewId view, const std::string& script, JsResultFnInternal fn, void* user)
    {
        bool queued = false;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            if (MlView* v = FindViewLocked(view); v && !v->destroyPending && v->evals.size() < 64) {
                v->evals.push_back({ script, fn, user });
                queued = true;
            }
        }
        if (!queued && fn) fn(view, "", "Magelight: view is gone or its eval queue is full", user);
    }

    void InvokeJS(ViewId view, const std::string& script)
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) QueueOutboundLocked(*v, { script, {}, true });
    }

    void SetUIModeView(ViewId view)
    {
        s_uiModeView.store(view);
        s_toggleView.store(view);  // the toggle key opens this view
    }

    // ── v4 internals ────────────────────────────────────────────────────────

    bool RegisterJSListenerEx(ViewId view, const char* name, JsListenerExFn callback, void* user)
    {
        if (!name || !*name || !callback) return false;
        if (RefuseReservedListenerName(view, name)) return false;
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->listeners[name] = MlView::Listener{ nullptr, callback, user };
            v->shimsDirty = true;
            return true;
        }
        return false;
    }

    bool DestroyView(ViewId view)
    {
        // Refuse while UI mode is up on this view. A queued EnterUIModeEx
        // that lands after this point sees destroyPending and refuses, so
        // s_uiModeView can never name a destroyed id.
        if (s_focused.load() && s_uiModeView.load() == view) return false;
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        if (!v || v->destroyPending) return false;
        v->visible = false;
        v->destroyPending = true;
        NoteMainMenuPagesLocked();
        v->kbTheme.reset();
        if (s_toggleView.load() == view) s_toggleView.store(0);
        if (s_uiModeView.load() == view) s_uiModeView.store(0);
        return true;
    }

    bool ReloadView(ViewId view)
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        if (!v || v->destroyPending) return false;
        v->reloadPending = true;
        v->domReady = false;   // hold outbound calls for the new page
        v->prepainted = false;
        v->paintableAt = 0;
        v->settleRepaint = false;
        v->settleRepainted = false;
        v->loadFailed = false;
        return true;
    }

    bool NavigateView(ViewId view, const char* urlOrPath)
    {
        if (!urlOrPath || !*urlOrPath) return false;
        std::string u = urlOrPath;
        if (u.rfind("file://", 0) != 0) {
            const bool absolute = u.size() > 1 && u[1] == ':';
            // A relative one keeps its '?' and '#' as URL syntax (a fragment-only Navigate keeps the page); only its
            // text is made UTF-8.
            u = absolute ? PathToFileUrl(PathText::FromUtf8(u)) : ("file:///" + PathText::ToUtf8(PathText::FromUtf8(u)));
        }
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        if (!v || v->destroyPending) return false;
        v->navigateUrl = u;
        v->domReady = false;   // hold outbound calls for the new page
        v->prepainted = false;
        v->paintableAt = 0;
        v->settleRepaint = false;
        v->settleRepainted = false;
        v->loadFailed = false;
        return true;
    }

    void SetViewLayer(ViewId view, int layer)
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->layer = layer;
            v->order = s_nextViewOrder.fetch_add(1);
            SortViewsLocked();
        }
    }

    // ── 0.28.0 ──
    void SetViewEscapeCapture(ViewId view, bool capture)
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) v->escapeCapture = capture;
    }

    // ── 0.31.0 ──
    bool FreezeWorldAvailable()
    {
        return !REL::Module::IsVR() && s_freezeWorldCfg.load();
    }
    int ConsoleLogLevel()
    {
        const int cfg = s_consoleLogCfg.load();
        return cfg >= 0 ? cfg : (s_devMode.load() ? kConsoleLogAll : kConsoleLogWarnings);
    }
    void SetViewFreezeWorld(ViewId view, bool freeze)
    {
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(view);
            if (!v || v->freezeWorld == freeze) return;
            v->freezeWorld = freeze;
        }
        SKSE::log::info("Magelight: view {} world freeze {}", view, freeze ? "requested" : "withdrawn");
        if (s_focused.load() && static_cast<ViewId>(s_uiModeView.load()) == view)
            GameTask::Post([]() { ApplyFreezeWorld("view preference"); });
    }
    bool LoadStaggerEnabled()
    {
        return s_loadStagger.load();
    }
    bool SetViewMainMenu(ViewId view, bool on)
    {
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(view);
            if (!v || v->destroyPending || v->isInspector) return false;
            v->mainMenu = on;
            NoteMainMenuPagesLocked();
        }
        SKSE::log::info("Magelight: view {} main-menu page {}{}", view, on ? "on" : "off",
                        MainMenuPagesAvailable() ? "" : " (needs the page thread: it stays off this session)");
        return true;
    }

    bool ViewOpensAtMainMenu(ViewId view)
    {
        if (!s_mainMenuOpen.load() || !MainMenuPagesAvailable()) return false;
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        const MlView* v = FindViewLocked(view);
        return v && v->mainMenu;
    }

    bool MainMenuPagesAvailable() { return PageThreadActive(); }

    bool SetViewPrepaint(ViewId view, bool on)
    {
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(view);
            if (!v || v->destroyPending || v->isInspector) return false;
            if (v->prepaint == on) return true;
            v->prepaint = on;
            if (!on) v->settleRepaint = false;
        }
        SKSE::log::info("Magelight: view {} prepaint {}", view,
                        on ? "on - painted while hidden after it loads, and once more when its page settles" : "off");
        return true;
    }

    bool SetViewGamepad(ViewId view, const Gamepad::ViewConfig& config)
    {
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(view);
            if (!v || v->destroyPending || v->isInspector) return false;
            v->gamepad = config;
            v->gamepadMode = config.startInCursor && config.allowCursor ? Gamepad::Mode::Cursor : Gamepad::Mode::Navigation;
            v->gamepadDirty = true;
        }
        SKSE::log::info("Magelight: view {} gamepad {}{}", view,
                        !config.enabled ? "off" : config.style == Gamepad::Style::Navigation ? "on (navigation)" : "on (keys)",
                        config.enabled && config.allowCursor ? ", cursor allowed" : "");
        return true;
    }

    bool GetViewGamepad(ViewId view, Gamepad::ViewConfig* config, bool* capturing, Gamepad::Mode* mode,
                        std::uint64_t* generation)
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        const MlView* v = FindViewLocked(view);
        if (!v || v->isInspector) return false;
        if (config) *config = v->gamepad;
        if (capturing) *capturing = s_gamepadOwner.load() == view;
        if (mode) *mode = v->gamepadMode;
        if (generation) *generation = s_gamepadGeneration.load();
        return true;
    }

    bool GamepadAvailable()
    {
        EnsureHostSettings();
        return !REL::Module::IsVR() && s_gamepadTuning.enabled;
    }

    bool GamepadConnected()
    {
        return s_gamepadConnected.load();
    }

    Gamepad::Glyphs GamepadGlyphs()
    {
        EnsureHostSettings();
        return s_gamepadTuning.glyphs;
    }

    bool PageThreadActive()
    {
        EnsureHostSettings();
        return s_pageThreadMode.load() != PageThread::Mode::Off;
    }

    bool SetViewLoadOnShow(ViewId view, bool onShow)
    {
        bool started = false;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(view);
            if (!v || v->destroyPending) return false;
            if (v->loadOnShow == onShow) return true;
            v->loadOnShow = onShow;
            started = v->ul || v->dormant;   // a page already loaded (a hibernated one wakes on show anyway)
        }
        if (started)
            SKSE::log::info("Magelight: view {} load-on-show {} - its page already loaded, nothing changes", view,
                            onShow ? "set" : "cleared");
        else
            SKSE::log::info("Magelight: view {} load-on-show {}", view,
                            onShow ? "set - first load waits for a show" : "cleared");
        return true;
    }
    // The page's CSP follows on its next load (ReloadView): a loaded document keeps the policy it was served.
    void SetViewNetworkLevel(ViewId view, NetLevel level)
    {
        std::filesystem::path root;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(view);
            if (!v) return;
            v->netLevel = level;
            root = v->root;
        }
        NotePageRoot(root, level);
    }

    void ShowInspectorFor(ViewId page, bool show)
    {
        if (show) { if (s_inspectorVisibleFor.load() != page) s_inspectorRequest.store(page); }
        else      { s_inspectorHideRequest.store(page); }
    }

    bool IsInspectorVisibleFor(ViewId page) { return s_inspectorVisibleFor.load() == page; }

    void SetViewOrder(ViewId view, std::uint64_t order)
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->order = order;
            // keep RaiseView's "above everything" promise
            std::uint64_t cur = s_nextViewOrder.load();
            while (cur <= order && !s_nextViewOrder.compare_exchange_weak(cur, order + 1)) {}
            SortViewsLocked();
        }
    }

    std::uint64_t GetViewOrder(ViewId view)
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        const MlView* v = FindViewLocked(view);
        return v ? v->order : 0;
    }

    void SetViewScrollStep(ViewId view, int px)
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) v->scrollStep = std::clamp(px, 1, 1000);
    }

    void SetViewSounds(ViewId view, const char* open, const char* close)
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->soundOpen  = open  ? open  : "";
            v->soundClose = close ? close : "";
        }
    }

    void RaiseView(ViewId view)
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->order = s_nextViewOrder.fetch_add(1);
            SortViewsLocked();
        }
    }

    bool GetViewInfo(ViewId view, ViewInfo& out)
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        if (!v || v->destroyPending) return false;
        out.x = v->x; out.y = v->y; out.w = v->w; out.h = v->h;
        out.visible = v->visible; out.fullscreen = v->fullscreen; out.clickThrough = v->clickThrough;
        out.layer = v->layer;
        out.htmlPath = v->htmlPath;
        out.deviceScale = v->deviceScale;
        return true;
    }

    void GetDisplaySize(int& w, int& h)
    {
        w = s_backbufferW.load();
        h = s_backbufferH.load();
    }

    bool IsRenderDead()
    {
        return s_renderDead.load();
    }

    const char* RenderDeadReason()
    {
        return RenderDeadReasonText();
    }

    std::filesystem::path GameRootPath()
    {
        return GameRoot();
    }

    bool PathIsUnderGameRoot(const std::filesystem::path& p)
    {
        return PathIsUnder(p, GameRoot());
    }

    bool PathIsUnderDir(const std::filesystem::path& p, const std::filesystem::path& base)
    {
        return PathIsUnder(p, base);
    }

    void SetHostEventSink(HostEventFn fn)
    {
        s_hostEventSink.store(fn);
    }

    // ── Texture-backed images (public API) ──────────────────────────────────

    bool IsGpuAccelerated()
    {
        return s_gpuActive;
    }

    // The name becomes a file name AND a page-visible identifier — keep it
    // to a safe alphabet.
    static bool SafeImageName(const std::string& n)
    {
        if (n.empty() || n.size() > 64) return false;
        for (const char c : n) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            if (!ok) return false;
        }
        return true;
    }

    ImageId RegisterTextureImage(const char* name, ID3D11ShaderResourceView* srv,
                                 std::uint32_t width, std::uint32_t height)
    {
        const std::string n = name ? name : "";
        if (!SafeImageName(n) || !srv || !width || !height) return 0;
        if (!s_gpuRegExt) {
            SKSE::log::warn("Magelight: RegisterTextureImage('{}') — GPU backend lacks external-texture support", n);
            return 0;
        }
        // The .imgsrc the page references in place of an image URL: two
        // lines, magic + identifier (ImageSource.h).
        std::error_code ec;
        const auto dir = s_runtimeDir / L"images";
        std::filesystem::create_directories(dir, ec);
        const auto file = dir / (n + ".imgsrc");
        {
            std::ofstream f(file, std::ios::binary | std::ios::trunc);
            if (!f) {
                SKSE::log::error("Magelight: RegisterTextureImage('{}') — cannot write {}", n, Magelight::SysInfo::ForLog(file));
                return 0;
            }
            f << "IMGSRC-V1\n" << n;
        }
        srv->AddRef();
        auto im = std::make_unique<MlImage>();
        im->id = s_nextImageId.fetch_add(1);
        im->name = n;
        im->w = width;
        im->h = height;
        im->srv = srv;
        im->url = PathToFileUrl(file);
        const ImageId id = im->id;
        {
            std::lock_guard<PageThread::OwnedMutex> lk(s_imagesMutex);
            s_images.push_back(std::move(im));
        }
        SKSE::log::info("Magelight: image '{}' queued as image {} ({}x{})", n, id, width, height);
        return id;
    }

    void UpdateTextureImage(ImageId image, ID3D11ShaderResourceView* srv)
    {
        if (!srv) return;
        std::lock_guard<PageThread::OwnedMutex> lk(s_imagesMutex);
        MlImage* im = FindImageLocked(image);
        if (!im || im->removed) return;
        srv->AddRef();
        if (im->pendingSrv) im->pendingSrv->Release();
        im->pendingSrv = srv;
    }

    void InvalidateImage(ImageId image)
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_imagesMutex);
        if (MlImage* im = FindImageLocked(image)) im->dirty = true;
    }

    void UnregisterImage(ImageId image)
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_imagesMutex);
        if (MlImage* im = FindImageLocked(image)) im->unregister = true;
    }

    const char* ImageUrl(ImageId image)
    {
        std::lock_guard<PageThread::OwnedMutex> lk(s_imagesMutex);
        MlImage* im = FindImageLocked(image);
        return im ? im->url.c_str() : "";
    }

}  // namespace Magelight

// ── VR keyboard delivery hook (optional) ────────────────────────────────────
// The host's own on-panel VR keyboard (raised on text-field focus), the
// SteamVR overlay keyboard, and OpenComposite's WM_OC_CHAR window message all
// type into a focused page already — this export needs NONE of them and is a
// generic hook on top: an external tool (a custom keyboard, an accessibility
// aid, a VR runtime that prefers a named entry point) can resolve
// GetProcAddress(GetModuleHandle("Magelight.dll"), "Magelight_DeliverChar" /
// "Magelight_DeliverVKey") and feed characters in (printable chars as wchar_t,
// a surrogate pair as two calls; Backspace/Enter/arrows as a Windows VK). They
// reach the UI-mode view as the host's own VR keyboards type: text as an edit
// (QueueTypedText; \t, \r and \n as the Tab and Enter keys), a VK as its key
// down and up (and the character Enter and Tab type). Gated: only while a page
// is focused AND the VR presenter is live; dropped otherwise. Any thread (the
// queue is mutex-guarded).
namespace {
    void DeliverCharImpl(wchar_t ch)
    {
        if (!Magelight::VR::IsLive()) return;
        if (!Magelight::s_focused.load() || Magelight::s_renderDead.load()) return;
        if (ch < 0x20 && ch != L'\t' && ch != L'\r' && ch != L'\n') return;
        thread_local wchar_t t_high = 0;   // a caller's high surrogate, until its low half comes
        wchar_t units[2]{};
        if (const std::size_t n = Magelight::EditState::PairUnit(t_high, ch, units)) Magelight::QueueTypedText(units, n);
    }
    void DeliverVKeyImpl(int vk)
    {
        if (!Magelight::VR::IsLive()) return;
        if (!Magelight::s_focused.load() || Magelight::s_renderDead.load()) return;
        Magelight::QueueVirtualKey(vk);
    }
}
extern "C" __declspec(dllexport) void Magelight_DeliverChar(wchar_t ch) { DeliverCharImpl(ch); }
extern "C" __declspec(dllexport) void Magelight_DeliverVKey(int vkCode) { DeliverVKeyImpl(vkCode); }

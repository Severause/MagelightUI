# Magelight UI — Claude Code Project Instructions

Magelight is a GPU-rendered web UI host for Skyrim SE/AE and VR: an SKSE plugin that
hooks `IDXGISwapChain::Present`, runs Ultralight 2.0 (2.0.0-beta.2 Free, WebKit 615) on the game's D3D11 device
through our own driver DLL, composites views over the game, routes input, and
exposes a versioned C ABI (`api/MagelightUI_API.h`) that other SKSE plugins consume. The
first consumer is SeverActions; the goal is a public framework.

Repo: `https://github.com/Severause/MagelightUI`. Work on a branch and open a PR; the
maintainer reviews and merges. Run `git remote -v` before any push and push only to the
remote you mean. No double quotes in commit messages; trailer
`Co-Authored-By: Claude <model> <noreply@anthropic.com>`.

## Layout

```
src/Magelight.cpp/.h        the host (hook, thread rules, views, input, cursor, images, events)
src/MagelightApi4.cpp/.h    the v4 mod registry (identity, events, UI-mode owner, results)
src/MagelightApiExport.cpp  the exported C ABI tables (v1..v4) — ORDER = struct member order
src/MagelightManifest.cpp   Data/Magelight/<ModId>/manifest.json loader (docs/MANIFEST.md)
src/MagelightPapyrus.cpp    the Papyrus tier natives (papyrus/Magelight.psc, docs/PAPYRUS.md)
src/MagelightDevWatch.cpp   devMode hot reload (folder watchers -> ReloadView)
src/MagelightFonts.cpp      the font loader: wraps the SDK's Windows loader (compiled in) with a last resort that always loads (src/fonts: DejaVu Sans, an RCDATA resource)
src/MagelightVR.h           the VR presenter's interface to the host (docs/VR_PRESENTER.md)
src/MagelightVR/            the VR presenter: Runtime, Overlays, Laser, Input, Bindings, Lifecycle; State.h = the state they share and its rules
src/GamepadCore.h           the gamepad layer's rules (1.6.0): who holds the pad, dead zones, repeat, held buttons, the keys style, the page packet; engine-free
src/GamepadScript.h         the page core's gamepad helper, window.magelight.gamepad (focus scopes, prompts)
src/PageThreadCore.h        the page thread's rules (1.7.0): when it may run (Decide), the handoff of page frames to the game thread
papyrus/Magelight.psc       the script consumers compile against (staged as Scripts/Magelight.pex)
views/gate/                 the version-gate notice page
examples/                   manifest example mods (build.ps1 -Examples stages them)
api/MagelightUI_API.h       the PUBLIC header consumers vendor (SA: Native/src/MagelightUI_API.h)
gpu/                        MagelightGPU.dll — D3D11 GPUDriver adapted from the SDK's reference driver (Zlib), C ABI only
extern/ultralight/          the Ultralight SDK, not committed (platform/ = its Zlib reference sources); extern/appcore-ref/ = the 1.4 driver's LGPL base, history only
frontend/, views/           React demo (Vite, es2022) + probe/badge pages (dev surfaces)
assets/, interface/         loose runtime files, the blank focus-menu SWF (the cursor is drawn in code: src/MagelightCursorArt.h)
tools/desktop-harness/      run our driver (or AppCore's) on the desktop — no game needed
tools/font-test/            the stock font loader vs ours on imitated broken systems (build.bat builds and runs it)
tools/hosttheme-test/       src/HostThemeCore.h's keyboard-theme colour rules (build.bat builds and runs it)
tools/tooltip-test/         src/TooltipCore.h's coalescing, clipping and payload rules (build.bat builds and runs it)
tools/memreport-test/       src/MemoryReportCore.h against captured GetMemoryUsage reports (build.bat builds and runs it)
tools/bridge-test/          src/JsBridgeCore.h and the page's __mlNative on a CPU view (build.bat builds and runs it)
tools/visibility-test/      View::set_visible on CPU and GPU views: hidden pages, the prepaint, the frame order SyncViewVisibility relies on (build.bat)
tools/input-test/           src/InputCore.h's key events on the flat and VR paths, the polled-keys exemption, the releases, the leave and the pointer guard (build.bat; MinHook from the build dir)
tools/ime-test/             src/ImeCore.h's candidate-list parsing and '__ime' payload rules (ime_test), the Editor calls the native composition relies on (imeprobe); build.bat builds and runs both
tools/ime-box-test/         the page core's IME candidate box in Ultralight's own WebKit: a reading the Editor composed, zoom, an iframe editor, device scale, the opt-outs (build.bat; needs node)
tools/editor-test/          src/EditStateCore.h's text-focus rules on real EditableState reports, typed text as the host fires it, views/keyboard's layouts (build.bat)
tools/gamepad-test/         src/GamepadCore.h's ledger, router, keys style and packet (build.bat)
tools/gamepad-page-test/    the gamepad helper in Ultralight's own WebKit: focus by geometry under zoom and device scale, scopes, editing, stale packets (build.bat)
tools/fiber-gc-test/        the page fiber's gate (invariant 1): src/PageStackCore.h's VM lock in Magelight's frame model, must crash without it; --thread: pages on the page thread, which must pass without it (build.bat); FiberRun.h runs the other tools on the fiber (MG_PAGE_FIBER=1) or the page thread (MG_PAGE_THREAD=1)
tools/stack-test/           MgStackTest (the in-game stack tests, package.ps1 zips it) and stack_probe (pages on the game's stack, with the host's JavaScript zones)
tools/deferred-test/        the GPU driver on the page thread's deferred context against the immediate context, byte for byte, MSAA, WARP's emulated lists, a View freed while held (build.bat)
tools/pagethread-test/      src/PageThreadCore.h: Decide, and the handoff (no page frame dropped, the game thread never waits, a stuck page included) (build.bat)
tools/load-test/            MgLoadTest, the in-game load kit for pageThread off and on (package.ps1 zips it; README has the protocol)
docs/                       MANIFEST.md, CPP.md, SDK.md, PAPYRUS.md, VR_PRESENTER.md
build.ps1                   build → C:\b\mgl (or MG_BUILD_DIR), stage → <that>\stage (namespaced runtime patch)
```

## Invariants (each cost a debugging arc — do not regress)

1. **Ultralight lives on ONE thread**: created on the game's main thread (= in-game present
   thread) after `kPostLoadGame`/`kNewGame` (`s_worldReady`, `s_mainThreadId`); frames from any
   other thread skip all Ultralight work (`s_ulThreadId`). Violations fastfail with no crashlog. Since 1.3.0
   the frame work runs on an 8 MB fiber of that thread (`UlFiberMain`, `"ulFiberMB"`; never less, `kMinFiberMb`:
   under about 6 MB JavaScriptCore's default 5 MB script limit lies past the fiber's end, so its error path commits
   into the fiber's own 64 KB stack guarantee and a page's first uncaught error is 0xC00000FD), and WebKit knows the
   fiber's stack as the thread's: an Ultralight call anywhere outside `FrameWork` would run on the game's
   stack against the fiber's bounds (script limits and the collector's stack scan both wrong). Ultralight 2.0's
   collector helper threads also scan the thread between frames, with those bounds, and crash
   (`MgWCore.dll+0x2B34EA`, or a ud2 at `+0x1866F30`), so the thread holds the page VM's lock whenever it is off
   the fiber (`s_vmLock`, `src/PageStackCore.h`): taken on the fiber at the end of the first frame in which the VM
   exists, never released. Never release it, and never touch it (or any WebKit code) off the fiber: where it is
   first taken is where WebKit counts script depth from, and WebKit keeps its per-thread state in fiber-local
   storage (WebCore imports `FlsAlloc`), so off the fiber the lock's owner is another thread to it. A new way off
   the fiber must leave with the lock held. With `ulFiberMB` 0, or when WebCore
   lacks an export the lock needs, pages run on the game's stack with JavaScriptCore's stack zones set past the
   thread's stack guarantee before `Renderer::Create` (`ApplyGameStackReserve`; `ReserveFor` gives a script 384 KB,
   room for a 32768-argument `apply` and no more, or layout from a script at its limit overflows): without
   them a guarantee (CrashLogger 1.23+ holds 64 KB) turns the first error a page leaves uncaught into 0xC00000FD
   (`MgWCore.dll+0x173F003`). Both reach WebCore through undocumented MSVC export names: re-run
   `tools/fiber-gc-test` (build.bat) at every SDK update; it must still crash without the lock and pass with it,
   CPU and GPU, and `tools/stack-test`'s `stack_probe` checks the zones on the game's stack. The other tools'
   checks hold on the fiber too: run them again with `MG_PAGE_FIBER=1` set (`tools/fiber-gc-test/FiberRun.h`).
   With `"pageThread"` (1.7.0, invariant 27) none of this applies: Ultralight is created on Magelight's page
   thread, whose stack is its own in a frame and between frames, so there is no fiber, no game-stack zones, and
   the collector crash the lock prevents cannot happen (`fiber-gc-test --thread --lock none` must pass). The page
   thread still holds the lock between frames, for speed only (an idle frame about 13 us against 235). The mode
   is decided at render init, before the renderer exists: the fiber and the page thread are never both used, and
   the tools run on it with `MG_PAGE_THREAD=1`.
2. **Listener callbacks fire on the render thread** (JS listeners, DOM ready, console, load
   failed): the game's main thread inside Present, or the page thread with `pageThread` (invariant 27). Game state = `GameTask::Post` (`MagelightGameTask.h` says why), never a raw
   `SKSE::GetTaskInterface()->AddTask`; mods use `PostGameTask`. A new main-thread entry point
   (a hook, a sink, an engine menu) holds a `GameTask::Scope` so its posts go through the pump. UI-mode entry/exit runs on the
   game thread (`SetUIMode` is an SEH-wrapped function: no locals with destructors inside it).
3. **Namespaced runtime**: never ship stock Ultralight DLL names (coexistence with other Ultralight-based UI mods; Windows
   binds imports by base name). `build.ps1` byte-patches same-length names; re-verify at every
   SDK bump, and re-capture `tools/memreport-test`'s fixtures (the memory line parses
   `GetMemoryUsage()`'s text, whose layout is not an API). `C:\b\mgl\MagelightGPU.dll` (the build dir's) is the unpatched copy the desktop harness uses.
   Every runtime import of `Magelight.dll` is delay-loaded, and its modules exist in the process only once
   `PreloadRuntime` (SKSEPluginLoad) has loaded each by full path. The runtime folder is never on the search
   path, so a delay-load binds only to a module that list already loaded: a new runtime DLL goes on it. When
   the preload fails the plugin stays loaded but inert, so no namespace-scope object may run Ultralight code
   when constructed or destroyed. The constructor of a class deriving from an SDK class can call the base's
   imported one (a static `FontLoaderWin` did, from DllMain, failing the plugin's load on every install), and
   the bridges' and listeners' base destructors are imports that would run from DLL_PROCESS_DETACH, raising
   0xC06D007E at process exit. Build such objects in `CreateUltralight` and never free them, or hold a stateless
   one in a `constinit NeverDestroyed<T>`. The API stays live while inert (consumers still call `CreateView`), so
   nothing those calls build may reach the runtime either: an SDK struct held by value (`MlView::edit`, an
   `EditableState`) is brace-initialised, because a `UExport` struct with member initialisers has an imported
   default constructor. `tools\check_stage.ps1` loads the staged host with the runtime out of reach and fails if
   it does not load, if a process that loaded it does not exit cleanly, or if `CreateView` there fails.
4. **Ultralight's platform code stays where the SDK puts it.** The SDK's Zlib reference sources
   (`extern/ultralight/platform`, never committed) reach the build two ways only: the D3D11 driver,
   adapted in `gpu/` (its own DLL, `MagelightGPU.dll`, reached only through the C ABI in
   `gpu/MagelightGpuApi.h`; never include driver internals in the host or copy driver code into
   `src/`; the Zlib notice stays and changes carry `// MG:` markers), and `fonts/FontLoaderWin.cpp` +
   `fonts/TextAnalysisSource.cpp`, compiled unmodified into `Magelight.dll` from the SDK folder
   (CMakeLists.txt). Nothing else of `platform/` is copied or built: `shaders/generated` is
   proprietary to Ultralight (use with Ultralight only; the driver reads it through an include path),
   `filesystem/FileSystemWin.cpp` is BSD and `fonts/FallbackFont.h` Apache-2.0. No `src/` file
   includes `<AppCore/...>` and AppCore.dll is not shipped: 2.0's imports CoreMessaging.dll and the
   WinRT api-sets, which no other runtime DLL needs, and `PreloadRuntime` fails on any DLL it cannot load.
5. **Own FileSystem** (`MlFileSystem`): fixed MIME table (AppCore's has no `.mjs`, and module
   scripts refuse a non-JS MIME), query-string strip, percent-decode, absolute drive-letter paths,
   the page-root union, and the Content-Security-Policy injected into every page. `resources/` must
   resolve.
6. **Mouse rides the SKSE input sink + MenuCursor**; the window never gets WM mouse messages.
   `ToggleControls(..., storeState=false)`; `ForceExitUIMode` on every load boundary. The Windows cursor is
   not what a page follows, so while a page holds the mouse it is clipped to the game window (`ConfineCursor`,
   flat only) and let go on exit and on focus loss, or a freed one clicks on another monitor too.
7. **Own engine menu** (`MagelightFocus`, kUsesCursor) on UI-mode entry — the engine drives
   MenuCursor only while a cursor-using menu is topmost. On VR an exit with a controller button
   down that was pressed while the page was open keeps that menu and the suspended controls until
   those buttons have been up 100 ms (`EndControlsHold`, 1.5 s at most): it is the menu context that
   keeps the closing press from the game. On flat the same holds for a view that held the gamepad
   (1.6.0) and a pad button pressed while it did (`s_heldPad`), as a second line: GamepadSink already
   ends the release of the B that closed the page before the engine sees it. A view that did not hold
   the pad never arms it. A kHide is read as the game closing the menu only if the
   menu is still closed once the queue has run (`CheckFocusMenuClosed`). The engine opens and closes
   the Cursor Menu with it: queue no Cursor Menu message of your own while it is registered, open the
   overlay (no kUsesCursor) before it, and keep kAssignCursorToRenderer on the overlay, or the engine
   rebuilds the cursor, which cursor mods can crash on. Another menu opening over a page makes the engine close
   and reopen the Cursor Menu as well, so its ProcessMessage refuses kHide while `s_focused` and the focus
   menu is open (`InstallCursorMenuPin`, `"pinGameCursor"`): the exit's hide arrives after `s_focused` is cleared,
   and a focus menu the game closed itself is off the stack before its cursor hide is read. Never read or write
   the Cursor Menu movie's ActionScript variables from a task: the game updates that movie on another thread, and
   a mod reading it from its input hook crashed when Magelight did. On flat its own AdvanceMovie hides it
   (`Hook_CursorAdvanceMovie`, after the game's advance, as PrismaUI does); `HideVanillaCursor`'s `_root` writes
   run only if that hook was not installed.
8. **es2022 bundles** (the SDK's WebKit 615); `crossorigin` stripped; `base: './'`.
9. **Blank pages must name themselves**: console listener + `OnFailLoading` → log + event.
   Console errors and warnings reach the log by default, gated only by the player's
   `consoleLog`; the `ConsoleMessage` event always gets every message in full. Plain
   `console.log`/`info`/`debug` stay out of the log by default because pages log secrets.
   A script exception reaches no console listener on its own, so a script the host runs for a mod
   reports a throw itself: `InvokeJS` and an `InteropCall` handler through `ReportScriptError` (the log
   line, a level-2 `ConsoleMessage`, the devMode banner), the page core's install as an error line.
   `DrainBridgeQueues` locks the page's JS context per call and calls a mod's EvalJS callback, or
   emits, only after that lock is released. A script a mod supplies (`EvalJS`, `InvokeJS`, Papyrus
   `Eval`/`EvalAsync`) runs unnamed (`JsBridge::Evaluate` with nullptr): a `magelight:*` name makes
   every relative `import()` in it, or in a function it defines, fail to resolve. Only scripts the
   host writes are named (`magelight:bridge`, `magelight:dispatch`); the host's own messages to the
   page core go as InteropCalls on reserved channels (`__tooltip`, `__uimode`), never raw scripts.
10. **Public ABI is append-only** (see `.claude/agents/review-abi.md`). The export table's
    initializer order must equal the struct's member order — a swapped pair compiles fine and
    calls the wrong function in every consumer.
11. **Texture images are session-long**: never unregister an image a page may still reference;
    `UpdateTextureImage` re-points, `InvalidateImage` per change, dirty-gated (an unconditional
    per-frame invalidate repaints the fullscreen page every frame — the frozen-cursor bug).
12. **Known engine defect** (Ultralight core, both drivers): a rounded box with differing border
    sides that is not fully opaque draws wedge triangles. Not ours to fix; pages keep such boxes
    opaque or drop the radius. Repro: `tools/desktop-harness/pages/translucent-borders.html`.
13. **Views are erased only in `ApplyPendingLifecycle`** (render thread): registry entry erased
    BEFORE the View RefPtr drops; LoadURL/Reload/release on live pages happen OUTSIDE
    `s_viewsMutex` (they run page handlers synchronously, which re-enter the locking listeners).
14. **UI-mode ownership is decided on the game thread** (the host's entry task) and mirrored into
    the v4 registry from the Entered/Exited/Refused events — never claimed ahead of entry.

## Build, deploy, test

- Build: `powershell -ExecutionPolicy Bypass -File build.ps1` (toolchain discovery: VS BuildTools
  18 is invisible to vswhere; the script has the fallback). Output `C:\b\mgl`, stage
  `C:\b\mgl\stage`; `MG_BUILD_DIR` moves both, and `tools\check_stage.ps1` / `tools\package.ps1`
  default to the same stage, so a second checkout can build without the shared directory.
- Frontend: `cd frontend && npm run build` → `views/app/` (committed; DLL build needs no node).
- Deploy the stage to your test targets, game closed (`Get-Process SkyrimSE`): Steam
  `...\Skyrim Special Edition\Data` and/or your mod-manager mod folder (e.g.
  `<modlist>\mods\Magelight UI`). Zip: `tools\package.ps1 -Version <ver> -Suffix -dev`
  (never `Compress-Archive`: it writes `\` into entry names).
- Settings: `My Games\Skyrim Special Edition\SKSE\Magelight.json` (`toggleKey`, `demoViews`, `stallWatchdog`/`stallThresholdMs`, `exceptionTelemetry`, `blockPolledKeys`, `skipGameCursor`, `clipCursor`, `pinGameCursor`,
  `imageProbe`, `forceCpu`, `cursorFile/Height/HotspotX/Y`, `cursorForce`, `modCursors`, `freezeWorld`, `freezeWorldSkipCapture`,
  `consoleLog`, `loadStagger`/`loadBudgetMs`, `msaa`, `memoryLogMinutes`, `ulFiberMB`, `pageThread`, `gamepad` (`enabled`, `glyphs`, `trace`, the stick and repeat tuning); `animationTimerDelay` is ignored on 2.0, whose
  Free edition caps every View at 60 frames a second). Log: `...\SKSE\Magelight.log`.
- **Desktop first**: any rendering question goes to `tools/desktop-harness` before a game cycle
  (our driver vs AppCore's on the same page; `inject.js` for live CSS bisects).
- Version: CMake `project(... VERSION x.y.z)` is the single source (`PLUGIN_VERSION*` macros).
  Bump it for every deployable change; the zip name follows it.

## Review

`/code-review` (`.claude/commands/code-review.md`) fans out to the rubrics in `.claude/agents/`:
threading, bugs, abi, gpu-driver, packaging, frontend, docs, simplicity. Run it before opening
a PR that touches `src/`, `api/`, `gpu/`, `build.ps1` or the SDK.

15. **Hotkeys never open over the game's own input.** `DispatchHotkey` refuses to
    enter UI mode while an engine menu is up, the console is open or
    `textEntryCount > 0`, and while a page has key focus only the ACTIVE view's
    own binding fires (to close). Anything looser turns a letter typed into a
    text field into another mod's panel opening.

16. **Ultralight load-listener callbacks can fire synchronously inside the call
    that triggers them.** `OnBeginLoading` runs INSIDE `LoadURL`, which
    `MaterializeViews` calls under `s_viewsMutex`; a listener that takes the
    same (non-recursive) mutex throws `system_error`, the SEH guard disables
    the overlay, and the whole UI is dead for the session (0.16.0 field log).
    So `OnBeginLoading` and the EditorListener (invariant 25) take only the
    leaf `s_editMutex`, under which nothing else runs: no Ultralight call and
    no other lock. `s_viewsMutex` is taken by `OnDOMReady` and
    `OnFailLoading`, which are asynchronous, and by `OnWindowObjectReady`,
    which can fire inside a script run (a View's first one comes with the
    first script that runs there, invariant 25), so the host runs no script
    (`LockJSContext`, `JsBridge::Evaluate`) under `s_viewsMutex`. Other state
    a synchronous callback needs is set by the caller that requested the
    load. `OnPageSettled` has only ever fired inside
    `Renderer::Update` (never inside `LoadURL`, `Reload` or `Render`), but the
    header promises no call it runs in, so it takes the lock with
    `try_to_lock` and, when it is held, skips its work and logs that it did
    (a lost settle never comes back: the page settles once per navigation).
    Section H of `tools/visibility-test` checks that it fires inside `Update`;
    re-run it at every SDK bump. `ApplyPendingLifecycle` also relies on the main-frame
    `OnBeginLoading` firing inside the `LoadURL` or `Reload` that starts a
    load: a call that fired none kept its document (only the fragment added
    or changed; no DOM ready follows), so it puts `domReady` back
    (`s_mainLoadsBegun`). A load that began and fails keeps its document too
    (`OnFailLoading` comes in a later `Update`, with no window object or DOM
    ready before it): `OnFailLoading` puts `domReady` back for a host request
    (`MlView::hostLoad`). A host `LoadURL` or `Reload` stops a load still under
    way with no callback at all, which is why a request made while another is
    pending keeps the first one's record. Re-check all of that at every SDK
    bump too (`tools/editor-test` H23): a load that began later would hand
    calls to a page still loading, and a failure or a stop reported another
    way would hold the calls for good or let them through early. `View::Resize`
    (either form) and `set_device_scale` run no page script inside the call
    (the page lays out in the next `Update`; its `resize`, `matchMedia` and
    `ResizeObserver` handlers run in `Render`), which is why
    `ApplyPendingResizes` may call them under `s_viewsMutex`; section G of
    `tools/visibility-test` checks it on a visible and a hidden View, with
    each of those handlers provoked, and if it ever fails the calls move out
    of the lock as `ApplyPendingLifecycle`'s do. `ApplyPendingResizes` makes
    no call when the size and scale are the View's own: on 2.0 a `Resize` at
    a fractional scale lays the page out again even then, rounding its CSS
    size up where a View created so rounds down.
17. **A key typed into a page must not exist anywhere else in the engine.**
    While UI mode is on, the engine keyboard device's Poll (BSIInputDevice
    vfunc 02) is skipped, so no keyboard button event is ever generated —
    not for SKSE input sinks, not for mods that hook the input dispatcher
    ahead of every sink (Immersive Equipment Displays opened its UI on
    Backspace typed into a search field and froze the cursor, 2026-09-03),
    not for Papyrus `OnKeyDown`. The text-entry flag is raised too, for the
    mods that check it. Typing, Escape and bound hotkeys all ride the window
    proc (WM_KEYDOWN / WM_CHAR are the OS, not DirectInput), so they keep
    working. The mouse device is untouched, and so is the gamepad but for a
    view that opted in to the gamepad layer (1.6.0): while it holds the pad
    (`src/GamepadCore.h`), every pad event ends at GamepadSink, unlinked like a
    typed key, and each release goes where its press went (a press the game saw
    keeps its release; one the page owned ends there too). GamepadSink is
    PREPENDED to the device manager's sinks, ahead of MenuControls and
    PlayerControls: menu context does not keep the pad from the game (the
    Cursor Menu's context is on top while a page is open, so B never arrives as
    Cancel and X still reads as Ready Weapon), and the B close is the router's
    own (`BatchOut::close`), not the focus menu's Cancel, which the focus menu
    ignores while the UI-mode view holds the pad. The input dispatch runs on
    whichever thread the engine's input job lands on, one dispatch at a time.
    Every engine pad event passes the router whether or not a view holds the
    pad, or that bookkeeping breaks. A plugin that polls
    user32 `GetAsyncKeyState` / `GetKeyState` itself gets keyboard keys as up
    (the Polled keys block in Magelight.cpp), except the UI-mode owner's DLL,
    Magelight and non-plugin modules. Two things the sink
    layer alone could NOT do, which is why the mute lives at the device:
    a sink cannot precede a dispatcher hook, and unlinking events only
    shields sinks registered later. An embedding mod's own engine menu
    (SA's live-view carrier) opens ABOVE the focus menu and so receives the
    menu-mode user events; it must swallow them.
18. **The world freeze lives and dies with the pause.** `kFreezeFrameBackground`
    on `MagelightFocus` is set only while that menu carries `kPausesGame` and
    the game is paused, and is cleared BEFORE `kPausesGame` on every unpause
    (`ApplyFreezeWorld` / `DropFreezeWorld`): a freeze over a running game hangs
    it. Never add `kTopmostRenderedMenu` to `MagelightFocus`: menus under it stop
    drawing, `MagelightOverlay`'s PostDisplay (the UI-pass composite) among them,
    and the page falls back to Present, which frame generation drops. Never on VR.
    The flag-only freeze, the HUD drawing under it and skip-capture are inferred
    from the engine and the PrismaUI recipe, not yet run in game: the UI-pass
    stop/resume log lines (`NoteUiPassState`) are how a test confirms them.
19. **The last-resort font must load.** WebCore's `lastResortFallbackFont`
    cannot go on without the font of `FontLoader::fallback_font()`: Ultralight 1.4
    dereferenced it without a null check (an access violation inside a frame,
    and the overlay off for the session: 0.31.2 field log, `MgWCore.dll
    +0xFB7F77`, a PC where Arial could not be loaded), and 2.0.0-beta.2 fails a
    check there and stops at a breakpoint (`WebCore.dll +0x10113F` offline). The font loader is
    `MagelightFonts::CreateLoader`'s, never the bare platform one: it checks the
    last resort at startup and serves the bundled DejaVu Sans when nothing else
    loads. `tools/font-test` reproduces the crash on the stock loader, on the game's stack and, with
    `MG_PAGE_FIBER=1`, on the page fiber pages run on by default (invariant 1).
20. **The VR keyboard page never sees what a mod sent.** It is shared by every mod and its
    keys go to the UI-mode view (`__key`), so a theme (`SetViewKeyboardTheme`, a page's
    `__hosttheme`) is checked in full by `src/HostThemeCore.h` before it is stored, a part with
    one bad value changes nothing (a page message's other part still applies), and the keyboard gets a JSON the host builds from
    the stored numbers (`QueueKeyboardThemeLocked`). The keyboard page itself may not set one.
    The focused field's kind goes the same way: fixed literals from `EditState::FieldJson`
    (`inputmode`, `enterkeyhint`, multi-line, password), never the page's text.
    Each message carries the whole state (shown + theme + field), replaces any still queued, and is
    queued under `s_viewsMutex` with the store or visibility change it reports.
21. **The vectored exception handler never logs.** `VectoredLogger` runs before CrashLogger for
    every severe exception, a crash included: it records into `s_seenFaults` and signals
    `s_faultEvent`, nothing else (no `SKSE::log`, no heap, no `GetModuleHandleEx`, no lock of its
    own), and never handles an exception. `DrainFaultReports` on the watchdog thread writes the
    lines. Logging from the handler could hang a crash on Magelight's own log lock.
22. **Every View is created with `kEnableCompositor` (on).** With Ultralight 2.0's compositor
    off, a device scale below 1 paints only the page's top-left scale-squared part, and
    `needs_paint()` stays true on an idle page, so `PresentedView::repainted` would make the VR
    presenter upload every frame. A new `CreateView` site sets it too. The host never calls
    `Renderer::Recycle(Full)` or `PurgeMemory`: on Free both drop every live page's compiled script.
    `FrameWork` calls `Render` at least once a second while no view is shown: 2.0 creates and frees GPU
    resources only inside `Render`, so a View released while nothing is shown (hibernation) would keep its
    video memory until the next show.
23. **A View's Ultralight visibility follows `MlView::visible`, synced before the input and again
    before Render** (`SyncViewVisibility`, render thread; the second call sits between `Update` and
    `RefreshDisplay`, because a View shown after `RefreshDisplay` runs no `requestAnimationFrame` in
    that Render and would show its old texture for a frame). `Render()` paints only the Views
    Ultralight sees visible, and the composite draws a view only when it is both wanted and visible to
    Ultralight, never on `v.visible` alone. A view that loads hidden is hidden before its page exists
    (`LoadViewLocked`). The prepaint is the only place a hidden View is shown, for exactly one Render,
    at the same point in the frame (`TakePrepaintView`: a prepaint view's first paint, and one more when
    its page settles, since 2.0 decodes a page's images only once it is painted; never in VR, so
    `OnPageSettled` asks for none there). That one is latched until the next DOM ready or host
    request that kept its page (`settleRepainted`; the page's own fragment changes never reset it): a
    fragment change is a navigation that settles again with no DOM ready, and
    the shown frame runs the page's rAF, so an unlatched repaint loops on a page that sets `location.hash`
    in rAF. Never call `RenderOnly`
    (deprecated; it paints no hidden View).
    `tools/visibility-test` checks all of this (section H the settle: only after a paint, inside `Update`,
    not again after a resize, again after a fragment change); re-run it at every SDK bump.
24. **Every key event is made by `Input::MakeKeyEvent`** (`src/InputCore.h`) with the key bits captured when
    its message was queued (`InputMsg::keys`), but for the Char of a surrogate pair, which the constructor cannot
    carry: `DrainInputQueue` builds that one by hand, its public `modifiers` from the same bits less the Windows
    keys (a made event never reports `metaKey`). Every key the host makes up from a bare virtual key carries
    `Input::SyntheticKeyLParam`. Ultralight 2.0's Windows KeyEvent constructor reads Shift, Ctrl, Alt and Caps
    Lock from the calling thread's keyboard state while it runs and ignores the public `modifiers` and
    `is_auto_repeat` once it has its platform event, and in VR the window thread sees no key message: a bare
    constructor loses every modifier there (Ctrl+A selected nothing), and a made-up key without the extended
    bit reads as the keypad (`Numpad4`). Never read a made event's public fields: they are unfilled. The
    constructor reads through `GetKeyState`, which the polled-keys filter hooks; it leaves those reads alone only
    because WebCore sits in the runtime folder (`Input::IsForeignPluginPath`). Each pointer source (the flat
    mouse, each VR hand: `InputMsg::source`) has its own press and hover (`Input::PointerRoutes`): while a source
    holds a button, its pointer events go to the view it was pressed on, and a view a source leaves gets
    `MouseMoved(-1,-1)`, its leave, once no other source points at it and no press is held there. A new way of
    choosing the target (per-element hit testing, a VR laser naming its view) keeps both, and a new pointer source
    gets its own `PointerSource`: one shared by two pointers sends one's moves to the view the other holds (a held
    key on the keyboard page sees its `mouseleave` and stops repeating) and a leave to the view the other points at.
    So every pointer source queues the release of each press it sent, also when it loses the pointer while UI mode
    stays on (the laser's `ReleaseTrigger` on a lost pose or controller): until it comes, the pressed view gets
    every pointer event of that source (only UI mode ending, the game losing focus or the view going away free it
    otherwise). `tools/input-test` checks all of this (the routing as `PointerRoutes`, with two hands), the hooked
    reads from the runtime folder and from another plugin's; re-run it at every SDK bump.
25. **Text focus is the EditorListener's report, acted on once a frame.** `MlEditorListener` only
    records `{View*, EditableState, inside the focus marker}` under the leaf `s_editMutex`: it fires
    inside whatever Ultralight call caused it (a click, a key, `Focus` / `Unfocus`, a script,
    `Update`, `Render`, `LoadViewLocked`'s `LoadURL` under `s_viewsMutex`). `DrainEditableStates`,
    right after `DrainInputQueue` and before the bridge drain and the second visibility sync, applies
    the reports to `MlView::edit` and lets `EditState::Tracker` (`src/EditStateCore.h`) decide the VR
    keyboards and the IME: only for the UI-mode view, never raising for the host's own `View::Focus`.
    A replaced page reports nothing, so every path that drops a page or its View calls
    `ForgetEditStateLocked`: before a View is released or destroyed, and at the main-frame window
    object of a document a load brought (`MlLoadListener::OnWindowObjectReady`), which is how a
    host `Reload` or `LoadURL` that replaces a document and a page's own navigation (a link,
    `location`, a reload, a form's submit) are caught; in a View that has had no window object,
    as soon as a host `Reload` or `LoadURL` has begun a load (`DocLoads::HasWindow`). A
    fragment-only navigation and a load that fails keep the page and its field. The window object
    needs `EditState::DocLoads`: only in a View that has had a window object does each new
    document's come as it begins, before its scripts and its reports. A View's first one comes only
    when a script first runs there: during the load it can follow the page's autofocus report (a
    `defer` or end-of-body script), after DOM ready it is the host's first script, so it is never a
    new document's; a load that fails keeps its page, and a released View is forgotten (`Released`:
    a new View can get its address). A new path must reset too, or the keyboard stays up for a field
    that is gone. A reset forgets the caret kept for a click on a scrolled field only when it is the
    UI-mode view's (`s_editResets`), never for a load elsewhere. Views are created with
    `initial_focus = false` and only the UI-mode view has View focus (the focus marker unfocuses the
    view UI mode left). VR typing goes through `QueueTypedText`: printable runs are
    `Editor::InsertText`, while Backspace, Enter and Tab stay key events (`QueueVirtualKey`), because
    pages act on their keydown (an Enter from `Execute(InsertNewline)` fires none). `tools/editor-test`
    checks all of it, the keyboard page's layouts included; re-run it at every SDK bump.
26. **No OS IME window.** The Windows IME windows do not draw over the game: players converted
    blind. While UI mode is on, `WM_IME_SETCONTEXT` reaches DefWindowProc with lParam 0 and the
    candidate list comes by `WM_IME_NOTIFY` (those never passed on). The composition goes into the field
    through the Editor (an edit op, invariant 25's drain), and the page hears of each change on
    `__ime` (`src/ImeCore.h`: the composition, its clauses, the candidate page and the anchor, the
    Editor's bounds in viewport CSS px) from that same drain, after the op it reports has run, so
    the page core draws the list unless the page opts out (`data-ml-ime="off"`, or a cancelled
    `magelight:ime`). The window thread only reads the IME and queues ops: it calls no Ultralight and
    no InteropCall, and its IME handlers throw nothing (the window proc's SEH guard turns the overlay
    off on an escaping exception). 2.0's `View::Unfocus` leaves a composition open, so the focus marker
    finishes it first (`ImeFinishComposition`) and the IME drops its copy (kImeCtlMsg 3, which closes
    its list too); a new place that takes View focus away does the same. Escape while composing is the
    IME's alone (`Ime::EscapeGuard`): never a UI-mode exit or a page key, the Escape the composition
    ended on included (Windows can end it while translating that key: `s_imeEndMsgTime`). Restoring an `ISC_SHOW*` bit brings back an OS window the player cannot see, under
    the drawn box. A number field takes no IME (as a browser): Ultralight reports one as Text, so
    `DrainEditableStates` asks the page (`EditState::kNumberFieldScript`) on a report of a single-line field
    in the UI-mode view, outside `s_viewsMutex` and only in a View that has had a window object (the
    script must not bring a View's first one, invariant 25); the answer also gives the VR keyboard its
    decimal keypad. `tools/ime-test` and `tools/ime-box-test` check it, on the fiber too; re-run them
    at every SDK bump.
27. **The page thread (`pageThread`, 1.7.0, flat only).** With it on, Magelight's page thread (`PageThreadMain`)
    owns Ultralight from `Renderer::Create` on and is the render thread of invariants 2, 13, 16 and 22-26: every page
    part of the frame (`PageWorkBegin`, `MaterializeImages`, Update, the second sync, the prepaint, Render,
    `CollectView`, `TickMemoryReport`) runs there (`PageThreadFrame`) and nowhere else. `MagelightGPU.dll` gets a
    deferred context of the game's device (contract 3) and only the page thread calls it; the game's immediate
    context is used on the game thread alone, which at Present (`ThreadModeFrame`) runs every finished page frame's
    command list in order (none is ever dropped: each carries uploads later frames build on), then draws the newest
    frame's views that are still wanted, then lets the next page frame start (`PageThread::Handoff`). The game
    thread never waits on the page thread: no join, no wait on its frames, and no lock held across page script (the
    host runs none under `s_viewsMutex`, invariant 16), so a page stuck in a script freezes only itself (the stall
    watchdog names it). The locks the page work holds across Ultralight or driver calls (`s_viewsMutex`,
    `s_imagesMutex`) are `PageThread::OwnedMutex`es: built /EHsc, a fault the page thread's SEH guard catches runs
    no destructor, so the guard gives back what its thread still holds (`ReleasePageLocksAfterFault`) or the game's
    next lock hangs; a new lock held across such calls on the page side must be one too. A packet's
    `PresentedView`s hold SRV references of their own and never `ul` (a View's
    reference count is Ultralight's); a recorded list keeps every resource it uses alive until it is released, so
    a View Ultralight frees meanwhile needs no retire queue (`tools/deferred-test`). Engine work at Present
    (`GameBookkeeping`, the overlay menu, the UI pass, the cursor quad) stays on the game thread. The mode is decided
    at render init (`StartPageThread`, `PageThread::Decide`): VR, a single-threaded device, a refused deferred
    context or a failed thread turn it Off with a log line, and the next frame starts Ultralight on the fiber. With
    the setting off the frame is unchanged. Today's single thread no longer serializes the page parts against the
    input sink, the window procedure, game tasks, Papyrus natives and API calls on the game thread: state they share
    is atomic or under its mutex, and new shared state must be too. `tools/deferred-test`, `tools/pagethread-test`
    and `fiber-gc-test --thread` are its gates; re-run them, and the page-facing tools with `MG_PAGE_THREAD=1`, at
    every SDK bump.

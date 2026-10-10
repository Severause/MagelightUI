# Magelight UI — changelog

Versions are `MAJOR.MINOR.PATCH`. The C++ ABI is **append-only**: a field is
only ever added to the end of a struct, never reordered or retyped, so a
plugin built against an older header keeps working against a newer host. Gate
any call added after your minimum on `hostVersionNumber >= PACKED` (packed =
`MAJOR*10000 + MINOR*100 + PATCH`), or ask `QueryCapability("name")`.

The **"added in"** column is the version to put in `minHost` (manifest) /
`minHostVersion` (C++ `RegisterMod`) if your mod needs that feature.

## API feature history

| Added | Gate | Feature |
|---|---|---|
| 0.31.7 | 3107 | `RebuildViewAtScale(view, scale)` — change a loaded page's device scale by releasing its View and loading the page again in a new one created at that scale (the page reloads; `ViewReloaded` follows); `QueryCapability("rebuildscale")` |
| 0.31.5 | 3105 | The network policy is a Content-Security-Policy written into every page: loopback = `localhost` / `127.0.0.1` over `http(s)` and `ws(s)` only (`[::1]` and other `127.x` no longer reach), a page cannot `fetch()` its own files, and a view shows only its own HTML files; `QueryCapability("csp")` |
| 0.31.1 | 3101 | `SetViewCursorTint(view, tint)` with `CursorTint` — the host's drawn cursor (arrow, hover glow, I-beam) and the VR laser dot in your colours over a view; `QueryCapability("cursortint")` |
| 0.31.0 | 3100 | `SetViewFreezeWorld(view, freeze)` — the game skips its 3D world render behind a paused page (flat only); Papyrus `SetFreezeWorld`; `QueryCapability("freezeworld")` |
| 0.31.0 | 3100 | Page console lines in `Magelight.log` are opt-in (`Magelight.json` `consoleLog`); `QueryCapability("consolelog")` says what the log records (0-3); the `ConsoleMessage` event is unchanged |
| 0.31.0 | 3100 | Staggered first page loads (`Magelight.json` `loadStagger`, `loadBudgetMs`; `QueryCapability("loadstagger")`); `SetViewLoadOnShow(view, onShow)` and manifest `loadOnShow` — no page load until the view is first shown; `QueryCapability("loadonshow")` (not in every unreleased 0.31.0 build: gate on 3101 or the capability) |
| 0.31.0 | 3100 | Per-view cursors: `SetViewCursor(view, desc)` with `CursorDesc`, manifest `cursor` (per view and mod-wide), Papyrus `SetCursor` / `ClearCursor`; CSS `cursor: none` hides the host cursor over a page on flat; `Magelight.json` `cursorForce`, `modCursors`; `QueryCapability("cursor")` (not in every unreleased 0.31.0 build: gate on 3101 or the capability) |
| 0.30.1 | 3001 | `PostGameTask(fn, user)` — post game-thread work from any thread; from a callback inside a frame it never waits on SKSE's task lock |
| 0.30.0 | 3000 | `NetworkPolicy::FileOnly` (the new default), manifest `"network": "file" \| "loopback"`, Papyrus `SetNetworkPolicy(modId, policy)`; the Papyrus tier acts only on script-owned mods |
| 0.29.0 | 2900 | `PlayUISound(view, name)`, `SetViewSounds(view, open, close)`; page `magelight.sound()` / `__sound`, `data-ml-sound` markup, manifest `sounds`, Papyrus `PlaySound`; `QueryCapability("sound")` |
| 0.28.2 | 2802 | `SetNetworkPolicy(mod, policy)` — per-mod escape from the file + loopback sandbox; `QueryCapability("networkpolicy")` |
| 0.28.0 | 2800 | `SetEscapeCapture`, `ShowInspector` / `IsInspectorVisible`, `SetViewOrder` / `GetViewOrder`, `SetScrollStep`; loopback `http(s)` allowed; capabilities `escapecapture` `loopback` `vieworder` `scrollstep` |
| 0.27.0 | — | Native IME (CJK composition) on flat; `QueryCapability("ime")` |
| 0.26.11 | 2611 | `RequestUIMode` on the holding view retargets `kUIModeFlagPause` in place |
| 0.26.9 | 2609 | `SetViewScale(view, scale)`; `ViewDesc::uiScale` is the real Ultralight device scale and round-trips through `GetViewInfo` |
| 0.26.8 | 2608 | `FindView(mod, name, &id)` — resolve a view name to its id from C++ (the Papyrus tier already had it) |
| 0.26.8 | — | Manifest robustness (a mistyped scalar warns instead of throwing); capability list single-sourced |
| 0.26.6 | 2606 | `kUIModeFlagNoTextEntry` (a page with no text field asks the host not to raise the engine text-entry gate — on Skyrim VR that gate summons OpenComposite's keyboard) |
| 0.26.4 | — | Host pinning: a page reads only its own `Data/Magelight/<Mod>/` folder and the runtime dir (removed in 0.31.5: `file:` reads never reached the hook, so it never held; pages read the union of page roots) |
| 0.26.3 | — | OpenComposite (OCU) detected and render-model tip-aim skipped there (its stub pops a dialog) |
| 0.26.0 | 2600 | `SetVRButtonListener` — raw controller button edges for press-to-bind chord UIs |
| 0.21.0 | 2100 | `BindVRHotkeyCallback` — a VR controller chord that calls YOU instead of running a built-in action |
| 0.18.0 | 1800 | VR presenter: `SetViewVRPlacement` / `GetViewVRPlacement` / `RecenterVRView` / `BindVRHotkey`, `VRPlacementDesc`, manifest `vr`/`vrHotkey` |
| 0.16.0 | 1600 | `SetViewCutout` (compositor cutout), `SetViewHibernate` (release a hidden view's texture); Papyrus `SetCutout`, `SetHibernate` |
| 0.15.0 | — | `window.magelight` page bridge installed on every page; SDK (`@magelight/sdk`, `@magelight/react`, vite plugin, `create-magelight-view`) |
| 0.14.0 | 1400 | `EvalJS` — run script and read the result / catch the exception back; Papyrus `EvalAsync` |
| 0.13.0 | 1300 | Papyrus tier (`Magelight.psc`: `RegisterMod`, `CreateView`, `FindView`, `ShowView`, `RequestUIMode`, `Call`, `Eval`, `BindHotkey`, `RegisterListener` and the rest); the version gate page |
| 0.12.0 | — | `BindHotkey` registry; manifest `hotkey` |
| 0.11.0 | — | Sessions / manifest docs |
| 0.10.0 | — | API v4 (`MagelightApi4`): per-mod registration, texture images, UI mode, events |

## 0.31.8

- **Pages show behind a swapchain wrapper without a `Magelight.json`.** When the game's swapchain is
  not dxgi's own (a `d3d11.dll` or `dxgi.dll` proxy such as ENB's or ReShade's, or another wrapper in
  front of it), `"composite": "auto"` now draws pages in the game's own UI pass, as it already did
  behind NVIDIA Streamline and Skyrim Upscaler. Not behind a frame-generation swapchain on Direct3D 12
  (Community Shaders' frame generation, for one), which keeps drawing at Present into the engine's
  framebuffer, nor with NVIDIA Smooth Motion when engine mode is off. Drawn at Present, a page could be lost: a wrapper may
  present a buffer other than the one Magelight draws into, so a hotkey paused the game and nothing
  appeared (a LoreRim report: a `d3d11.dll` proxy with ShadowBoost and SSE Display Tweaks hooking
  Present), and the cure was `"composite": "ui"` by hand. Click-through HUD pages draw at Present
  while no interactive page is open, and the UI pass still falls back to Present when the game's menus
  are hidden or it finds nothing to draw on. While an interactive page is up, Escape does not open the
  Journal (the carrier menu that hosts the UI pass is open), as behind an upscaler. `"composite": "present"` restores the old behaviour. The log line says why:
  `composite 'auto' - views draw in the game's UI pass ('MagelightOverlay') - the game swapchain is a
  wrapper, not dxgi's own`.

## 0.31.7

- **`RebuildViewAtScale(view, scale)`: a new device scale for a loaded page without a live re-layout.**
  `SetViewScale` on a page that has loaded calls Ultralight's `set_device_scale`, which re-lays the
  page out inside the frame; that crashed the game for SeverActions three times out of three, and it
  has not been verified since it moved to the render thread in 0.28.5. The new call does what
  hibernation already does every session: at the view's next frame the host releases its View and
  texture, keeps the record (bounds, visibility, UI mode, listeners, cursor, sounds, session, VR
  placement) and loads the page again in a new View created with `initial_device_scale` at the new
  scale, in the same frame when the view is visible, so the page lays out and rasterizes at that DPI
  from its first paint. Calls sent meanwhile wait for the new page's DOM ready, and `onDomReady`,
  `ViewDomReady` and `ViewReloaded` fire again. The cutout is kept in view pixels: send it again at
  the new scale. A hidden view reloads at once unless it hibernates or loads on show (then when
  shown), a view with no page yet takes the scale at its first load, the scale its View already has
  is a no-op (and withdraws a pending rebuild), and a page hosting a Web Inspector is not rebuilt.
  Clamped 1.0..3.0 like `SetViewScale`. The UI-mode view can be rebuilt in place: the focus marker
  runs again on the new View, and a focused text field's IME, keyboard and VR keyboard claims are
  dropped with the old page. Gate on `hostVersionNumber >= 3107` or `QueryCapability("rebuildscale")`;
  the page sees `capabilities.rebuildScale`.
- **VR: a hibernated view keeps its placement.** Waking from hibernation (and a rebuild) used to
  create the new overlay at the layer default, so a `SetViewVRPlacement` choice was lost and
  `GetViewVRPlacement` answered nothing until the view was shown again. The effective placement now
  stays with the view, and a panel placed when it was released reappears where it was.

## 0.31.6

Diagnostics and build hygiene, after a review of langfod's fork.

- **`Magelight.log` keeps its own first lines.** `SKSE::Init` ran CommonLib's log setup after ours,
  which reopened (truncated) the same file and replaced the logger, so the "loading (built ...)" line
  never reached the log and the format was CommonLib's. Magelight now keeps its own logger: each line
  carries the date, the level letter and the thread id, and the level is `info` unless
  `Magelight.json` `"logLevel"` says otherwise.
- **The top of the log says what the game runs on:** Skyrim SE/AE/VR and its version, SKSE's version,
  Windows (build, update revision and release, or the Wine version under Proton), the CPU and RAM,
  and at kDataLoaded every graphics adapter with its VRAM and driver version (the NVIDIA number too),
  the game's own marked. DXGI is loaded from the system folder by full path, so a proxy `dxgi.dll` is
  never asked, and no game device is touched.
- **Paths in the log show the user folder as `%USERPROFILE%`**, so a posted log does not name the
  Windows user. Ultralight's own startup lines (its cache folder) are covered too.
- **A Windows user name the ANSI code page cannot spell no longer ends the game at load.** The log file
  was opened by a narrow path, which throws for such a name, and the failure ended the game; it is now
  opened by its wide path, and every logged path is written as UTF-8.
- **The crash telemetry logs every distinct fault, with its stack.** It used to stop after the first
  eight exceptions of a session, which one mod faulting in a loop could spend in a millisecond. It now
  logs each fault site once (up to 64), keyed on the raw address, and under it the faulting thread's
  calls as `module+offset`, up to 23 frames; a call through a null pointer is walked from its caller. A
  stack overflow gets the one line only, and when the overflowed thread has too little stack left to
  write it, another thread writes it a moment later: logging there could overflow it again and kill a
  process that handles it. CrashLogger's own probes while it writes a report are skipped.
- **A `MagelightGPU.dll` from another build is refused.** The GPU backend now reports its C ABI revision,
  the size of Ultralight's draw state and commands, and the Ultralight version it was built against
  (`MgGpu_GetInfo`), and the host compares them with its own. A mod manager could pair this
  `Magelight.dll` with another mod's `MagelightGPU.dll`; one built for another Ultralight SDK reads every
  draw command at the wrong offsets. A mismatch, including any `MagelightGPU.dll` from before 0.31.6, is
  one log line, and pages draw on the CPU instead.
- **No throwaway device behind a `dxgi.dll` proxy (ReShade).** To draw inside dxgi's own Present behind a
  swapchain wrapper (`presentHook: "late"`, or Smooth Motion with engine mode off), Magelight finds it
  through a throwaway device. Behind a `dxgi.dll` proxy that device binds to the proxy, so the search
  could only find the proxy's swapchain again: it is now skipped with one log line, and pages draw in the
  vtable hook.
- Build: the Ultralight headers are a system include (their hundred-odd unreferenced-parameter
  warnings are gone), CommonLib is a precompiled header for the host (an edit to `Magelight.cpp`
  rebuilds in about 8 s instead of 15), CommonLib is built without xbyak, the unused `directxtex` and
  `xbyak` packages (and Xbyak's notice) and the dead colorglass registry are gone, the port declares
  itself static-only, CI restores the vcpkg binary archives of unchanged packages after a manifest
  change and keeps only the archives the build used, and the garbled dashes in CMakeLists.txt (one of
  them in the missing-SDK error) are fixed.

## 0.31.5

Found while testing Magelight against the Ultralight 2.0 beta; every item is a 1.4 fix.

- **The network policy is a Content-Security-Policy now.** Ultralight 1.4 hands the host's request
  hook only `http(s)` loads: synchronous XHR, WebSockets and `file:` reads never reach it, so a
  file-only page could open a WebSocket, a document's synchronous XHR could read any file on the
  disk, and the per-page file pin never ran. The host now writes the mod's policy into every page it
  serves (`CspFor`), stops a main-frame navigation to anything but the view's own HTML files (a
  `file:` SVG or a `data:` document would run with no policy), and keeps the request hook as the
  second line. Loopback is `localhost` and `127.0.0.1` over `http(s)` and `ws(s)`; CSP cannot name
  `127.x.x.x` or `[::1]`, so a page that reached its server at `[::1]` switches to `localhost`. A
  page cannot `fetch()` its own files any more (Vite's `modulePreload` polyfill logs one refusal
  per page; set `build.modulePreload.polyfill = false`, which `@magelight/vite-plugin` does), and
  file: iframes between pages are refused: every `file:///` document shares one origin, so a
  frame would lend a looser mod's policy. Known limit: `<link rel="preload" as="fetch">` still
  sends one GET to any host. The docs now say that pages are not kept out of each other's
  folders: the file system serves the union of the page roots and cannot tell which page asks.
  `QueryCapability("csp")` answers 1.
- **The game's pipeline state survives a Magelight frame.** `StateBackup` now saves and restores the
  index buffer, pixel and vertex shader resource slots 0-3, the vertex constant buffer, the scissor
  rects and the unordered-access views, and captures before `Render` (which creates geometry and
  binds an input layout). A harness run with sentinel state had shown those slots left changed.
- **A missing border segment at `msaa` 4.** The core can name the texture behind the bound render
  target as a draw's source; binding it resolved the target mid-layer, so the layer's last draws
  were lost. The driver skips such a bind.
- **`requestAnimationFrame` runs at the frame rate.** Ultralight's rAF timer (1/60 s) aliased against
  the per-frame pump: 40 callbacks a second on a 60 Hz game, 48 at 144 Hz, with alternating 17 and
  33 ms gaps. `Magelight.json` `animationTimerDelay` (seconds, default `0.001`) sets it. CSS
  animations and transitions were never affected.
- **A runtime from another Ultralight version leaves the host inert** instead of crashing: a mod
  manager can install `Magelight.dll` from one mod over the runtime DLLs of another. The log names
  both versions.
- Comments and docs corrected: Ultralight's own file system has a static MIME table (the registry
  claim was wrong; `.mjs` and query strings are why Magelight has its own), the request hook runs on
  the render thread, and with no `cache_path` WebKit writes page storage under the game root, not in
  memory.

## 0.31.4

- **VR: closing a page with a controller button no longer hands that button to the game.** A page
  closes on the press of the button that closes it (a mod's chord such as Trigger + Y, or the B/Y
  back button), and the game got its menu context and
  controls back while the button was still held, so it acted on it: closing with Trigger + Y also
  did whatever Y does in the game. When a page closes with a controller button down that was
  pressed while the page was open, Magelight's engine menu and the suspended controls now stay
  until those buttons have been up for 100 ms (1.5 s at most; a button resting down from before
  the page opened, or pressed after the close, does not count, and a paused page keeps the game
  paused that long). The log says so: `controls held off until the controller
  buttons are released`, then `controls restored (controller buttons released)`. A load ends the
  hold at once. Flat play is unchanged.
- The VR presenter is now six source files (`src/MagelightVR/`) instead of one; no behaviour change.

## 0.31.3

- **A PC whose last-resort font cannot be loaded no longer turns the UI off.** When none of the
  fonts a page asks for loads, Ultralight falls back to the font loader's last resort (Arial) and
  uses it without checking that it loaded, so on a PC where Arial is missing or cannot be read the
  first such page crashed inside Ultralight. Magelight caught the crash and switched its overlay
  off for the rest of the session, which players saw as the UI randomly turning off. The log shows
  it as `Magelight VEH: exception 0xC0000005 ... (module MgWCore.dll +0xFB7F77)` followed by
  `SEH exception ... in frame work — overlay disabled`. Magelight now checks at startup that the last
  resort loads (Arial, then Segoe UI, Tahoma, Verdana, Calibri, Microsoft Sans Serif, Times New
  Roman) and otherwise uses DejaVu Sans, which `Magelight.dll` carries (about 740 KB), so text
  always has a font. The log names the font in use (`fonts: last-resort font 'Arial'`) and says
  why when Arial had to be replaced.
- **A font in a folder with a non-ASCII name loads.** Ultralight opens font files through the
  ANSI code page, so a font stored under such a name (one installed for a Windows user whose name
  is not ASCII, for example) never loaded. Magelight now reads those files itself.
- **An installed font that cannot be loaded is logged**, once per family (`fonts: '<family>' is
  installed but cannot be loaded`): when Windows' font loader hands nothing over for it, or when
  Magelight read its file itself (the last-resort font, a non-ASCII path) and could not use it. A
  family a page names that the PC does not have stays out of the log, as before.
- `tools/font-test` runs the stock loader and Magelight's against a page on imitations of broken
  systems (no fonts at all, unreadable font files, a font in a non-ASCII folder). The stock loader
  crashes at the field log's offset (`WebCore.dll +0xFB7F77`, checked); Magelight's draws every
  page, and on a normal system it measures the same text the same as the stock one.

## 0.31.2

- **`toggleKey` takes a key name.** `Magelight.json` `"toggleKey"` now accepts the same names as
  `hotkeys` (`"F3"`, `"PageDown"`, `"Insert"`) as well as a DirectInput scancode, and `0` or
  `"none"` turns the toggle key off. A number that is not a key DirectInput reports now logs a
  warning, and one in Windows' F1-F12 key-code range (112-123) names the scancode that was meant: a
  player who wrote `114` for F3 got a toggle key that never fired, with no message. `hotkeys`
  entries get the same warning.

## 0.31.1

- **A mod can recolour Magelight's cursor over its pages.**
  `SetViewCursorTint(view, tint)` takes five colours (the arrow's lit and
  shaded facets, the outline, the hover glow and the I-beam) and the host
  draws its own cursor in them over that view: the same shape, glow,
  press-shrink and sharpness at every resolution, at the player's
  `cursorHeight`, with no image files. The VR laser dot takes the lit colour
  for its core and the outline colour for its rim. A colour with alpha 0 keeps
  the host's and any other alpha is drawn opaque; `nullptr` gives every colour
  back. The view's own cursor images still win; on flat the player's
  `cursorFile` replaces the drawn cursor, and `"cursorForce": true` or
  `"modCursors": false` drop the tint as they drop images. Each tint's cursor
  is built once per resolution and kept (four at a time). C++ only (no
  manifest key or Papyrus call). The cursor art was checked offline (the
  host's colours draw byte-identical to 0.31.0, a tint changes colour only);
  not yet run in game.
- Version gate: 0.31.0 was never released, and its test builds lack parts of
  the 0.31.0 appendix (`SetViewLoadOnShow`, `SetViewCursor`, Papyrus
  `SetCursor` / `ClearCursor`). Gate those, and the tint, on
  `hostVersionNumber >= 3101`, or keep `>= 3100` together with
  `QueryCapability("loadonshow")` / `("cursor")` == 1 as docs/CPP.md does.
  `SetViewFreezeWorld` is in every 0.31.0 build.

## 0.31.0

- **The world can stop rendering behind a fullscreen page.** A mod calls
  `SetViewFreezeWorld(view, true)` once for a view; whenever that view holds
  UI mode with the game paused (`kUIModeFlagPause`), the game is expected to
  show a frozen frame instead of drawing its 3D world. This is the Journal's
  freeze flag without the Journal's `kTopmostRenderedMenu`, which would stop
  Magelight's UI-pass drawing; the flag alone has not been run in game yet.
  How much frame time it gives back is to be measured. The host puts the
  freeze on and takes it off itself: it starts with the pause, and it stops
  before the pause does when the view is unpaused in place, another view
  takes UI mode (the inspector included), UI mode closes, a load starts, or
  another mod lets the game run. The HUD is expected to keep drawing under
  the page, so use it for opaque pages. Papyrus: `SetFreezeWorld(view,
  freeze)`. `QueryCapability("freezeworld")` is 1 on flat Skyrim; on VR the
  call returns `Unsupported` (untested there, so excluded as a precaution). A
  player can turn it off for every mod with `"freezeWorld": false` in
  `Magelight.json`, which also gives back 0.30.6's menu flags.
- `Magelight.json` `freezeWorldSkipCapture` (default `true`): in UI-pass
  composite, meant to keep Magelight's drawing out of the frame the game
  captures as the frozen background, so a translucent page cannot show a
  stale copy of itself underneath. Set it to `false` if a page flickers when
  the freeze starts. No effect with composite `present` or `engine`, or when
  `freezeWorld` is `false`.
- `Magelight.log` notes when the UI pass stops drawing while a page is open
  (its views then draw at Present, which frame generation drops) and when it
  comes back, with the world freeze's state.
- **Page console output in `Magelight.log` is opt-in.** Every `console.log`
  of every page used to be written to the log, and a page that logged its own
  settings put the start of an API key into a log players post with bug
  reports. The log now records a page's warnings and errors only;
  `console.log`, `info` and `debug` lines are written with `"devMode": true`
  or with `"consoleLog": "all"` in `Magelight.json` (`"errors"` keeps errors
  only, `"none"` nothing; an explicit `consoleLog` wins over devMode). Each
  logged message is cut at 2 KB with its full length noted, and a view writes
  at most 20 lines a second, followed by a count of the lines left out. The
  `ConsoleMessage` event still delivers every message, in full, to the mod
  that owns the view, and the devMode error banner is unchanged, so a mod
  that forwards its console lines itself keeps working.
  `QueryCapability("consolelog")` answers what the log records (0 none,
  1 errors, 2 warnings and errors, 3 all), for a mod that wants its own
  lines and should forward them itself when the answer is below 3. There is
  no manifest key or API to turn a mod's lines on: what goes into the log is
  the player's choice. The settings line in the log names the level in effect.
  Not yet run in game or in the desktop harness.
- **Pages load over several frames after a save loads.** The host used to
  start every registered view's page in the frame the world became ready, so
  every page's parse and first script run landed in the same few frames (in
  one field log, 16 views cost about 0.75 s of frames and a 96 ms frame). The
  pages of views that have not been opened now start one per frame, in the
  order they were created, and the next only once the previous page reached
  DOM ready or failed (or 100 ms passed) and the last frame's Ultralight
  update took under `loadBudgetMs` (default 8). A batch lasts at most about
  a second: whatever is still waiting then starts together. A view that is
  visible at creation (`startVisible`), is shown or enters UI mode before its
  page loaded loads at once. DOM ready still arrives some time after
  `CreateView`, never inside it; calls made before it (listeners,
  `InteropCall`, `InvokeJS`, `EvalJS`) are still delivered after it. What
  changes for a mod is how long after a load a page it has not opened becomes
  ready: up to about a second plus the page's own load. A mod that waits for
  DOM ready before it shows a view (or refuses to show it until then) waits
  for the queue too, so showing the view is the way to have it at once.
  `Magelight.log` sums each batch (`staggered load - N views over F frames in
  T ms`). A player turns it off with `"loadStagger": false` in
  `Magelight.json`; `QueryCapability("loadstagger")` says whether it is on.
  Not yet run in game: the frame-time gain and the batch length are to be
  measured, and the batch line is how a test reads them.
- `SetViewLoadOnShow(view, true)` (manifest `loadOnShow`): a view's page is
  not loaded at all until the view is first shown, for panels that are rarely
  opened. It decides only a load that has not started, so call it right after
  `CreateViewEx`, before the world loads; a loaded page stays loaded. Such a
  view's DOM ready comes after its first show. Opt-in: nothing changes for a
  mod that does not call it. `QueryCapability("loadonshow")` is 1.
- A view destroyed before its page loaded is no longer loaded first only to
  be torn down in the same frame.
- **A mod can bring its own cursor.** Until now the cursor was the host's,
  for every view of every mod; only the player could swap it, for one still
  image everywhere (`cursorFile`). A view can now carry its own images, one
  for each page state: `arrow`, `pointer` (over `cursor: pointer`, `grab` and
  `grabbing`) and `text`, each with its hotspot in image pixels, a height at
  1080p, and an opt-in press shrink. A missing pointer image uses the arrow
  image; a missing text image keeps the host's I-beam. Declare it in the
  manifest (`"cursor"` on a view, or at the top level as the default for
  every view of the mod), from C++ with `SetViewCursor(view, &desc)` (one
  call per state, `nullptr` clears) or from Papyrus with `SetCursor` /
  `ClearCursor`. Images are PNG or DDS (anything Windows' image decoder
  reads), at most 256x256, relative to the mod folder (`Data/Magelight/<Mod>/`
  for a page there, else the page's own folder). They decode on a worker the
  first time they are drawn, never inside a frame; until then, and when one
  fails (logged once), the host cursor shows. `QueryCapability("cursor")` is
  1. A mod that sets no cursor gets the same cursor as before; the one
  difference is that a page's hover state is now forgotten when UI mode
  closes (below), so after reopening, the glow over a button waits for the
  first mouse move. None of the cursor changes has been run in game yet.
- **`cursor: none` now means "the page draws its own pointer".** Over a page
  whose CSS cursor is `none`, or a view whose cursor is `"none"`, the host
  draws no cursor on flat (before, the arrow was drawn over the page's own,
  so such a page showed two). The host cursor comes back the moment the
  pointer leaves that view, and every page's cursor is forgotten when UI
  mode closes, so a page cannot leave the player without a pointer. A CSS
  `cursor: url(...)` whose image loaded shows the view's own arrow image
  when it has one (the host cannot read the page's image), else the host
  arrow, as before. Neither SeverActions nor SkyrimNet uses either.
- The player keeps the last word: `"cursorForce": true` in `Magelight.json`
  draws the host cursor (their `cursorFile`, else the drawn art) everywhere,
  over mod images and over pages that hide the pointer; `"modCursors": false`
  ignores mod images but still lets a page that draws its own pointer hide
  the host's. With neither, a view's own cursor wins over its mod's default,
  which wins over the host cursor, so a mod's cursor now shows instead of a
  player's `cursorFile` over that mod's views.
- In VR the laser end stays a dot. With `vr.cursorDot` false it shows the
  view's own image (sized from the panel like the arrow) when it has one,
  else the arrow or `cursorFile` as before; `cursor: none` and `"none"` are
  ignored there, because the laser has to show where it points.
- **Anti-aliased page shapes.** The GPU driver draws pages with 4x MSAA again
  (Ultralight's reference driver uses 8x; Magelight had turned it off).
  Ultralight fills SVG paths and other non-rectangular shapes as plain
  triangles and leaves their edges to MSAA, so without it every curve and
  diagonal was stair-stepped; boxes, rounded corners and text were smooth
  either way. `Magelight.json` `"msaa"` sets the samples (1 off, 2, 4, 8);
  each count above 1 adds that many copies of every page target in video
  memory (about 15 MB each at 2560x1440) while the page exists, hidden or not,
  until the view hibernates: at 4x about 60 MB per full-screen page, plus about
  the same again for each full-size layer the page composites (a
  transform-scaled shell is one). A count the graphics card cannot do steps
  down, and a fractional number such as `8.0` is read. A multisampled target resolves
  into the plain texture the compositor and Ultralight sample, so nothing that
  reads a page changes. `MagelightGPU.dll` gains the optional export
  `MgGpu_SetSampleCount`; an older backend keeps working without MSAA. The
  CPU path (`forceCpu`) is unchanged. `Magelight.log` names the count in
  effect (`MSAA 4x`) and the settings line lists `msaa`. Run in game on flat
  at 4x: SVG edges are smooth.
- `build.ps1` builds into `MG_BUILD_DIR` when it is set (default `C:\b\mgl`).

## 0.30.6

- **A second, Windows mouse pointer after closing a page (fixed).** Leaving a
  page forced the Windows cursor on instead of putting back what was there
  before. On some installs (most likely those where Skyrim does not hold the
  mouse exclusively, such as `bBackgroundMouse=1` in `Skyrim.ini`) it then
  stayed on screen beside the game's pointer until the game window was
  minimized and restored. Magelight no longer touches the Windows cursor at
  all; the game keeps it hidden as it always does. `Magelight.log` now notes
  the cursor's state shortly after the first page open and close of a session,
  and after up to five later closes that leave the Windows pointer over the game.
- **VR: arrows, Delete, Insert, Home, End, Page Down, numpad Enter and numpad
  Divide now type into pages.** These keys were dropped in VR typing, where the
  game's keyboard device feeds the page. Pause is passed on as Pause. (Page Up
  is the host toggle key and leaves UI mode, unless `Magelight.json` sets
  `toggleKey`.)
- **VR: held keys repeat.** A held key sent a key release every frame instead
  of repeating; it now repeats at the Windows keyboard delay and rate, so
  holding an arrow or Backspace moves or deletes as on flat.
- **VR: the numpad types digits with NumLock on**, and Shift, Ctrl and Alt
  reach pages as the same key codes the flat window sends.
- No API changes; the npm packages stay at 0.30.0.

## 0.30.5

- **No UI with Community Shaders frame generation (fixed).** Community Shaders'
  frame-generation swapchain answers `IDXGISwapChain1` with an 18-slot vtable.
  Magelight wrote its Present1 hook into slot 22, past the end, over the
  interface id that swapchain's `GetDevice` compares. `GetDevice` then failed,
  and the renderer gave up at start ("render-thread init failed"). On other
  Community Shaders versions the same write broke the device interfaces other
  plugins ask for, a crash risk. Magelight now checks that IDXGISwapChain1's
  slots are code before patching. A swapchain whose vtable is short, or that
  refuses its device, is treated as untrusted:
  - no Present1 hook and no late composite;
  - the engine's device when the swapchain refuses its own;
  - the overlay drawn into the game's own framebuffer view, which is
    Community Shaders' UI buffer while frame generation runs, so pages
    stay sharp and are not interpolated;
  - a back buffer never released unless the swapchain gave a reference (an
    over-release freed the game's buffer on Community Shaders 1.6-1.8.3).
- **NVIDIA Smooth Motion: pages show steadily instead of freezing the game.**
  Behind Streamline the swapchain hands out its own wrapper device, and
  Magelight drew with it; under Smooth Motion (the driver's frame generation,
  `NvPresent64.dll`) mixing those wrappers with the engine's own objects
  removed the graphics device, and the game froze until it was killed. With
  Smooth Motion loaded, Magelight now runs in engine mode: it renders with the
  engine's own device and context and draws from the engine's end-of-frame
  call into the target the engine has bound, never at Present, so Smooth
  Motion's generated frames carry the page too. `Magelight.json`
  `"composite": "engine"` forces the mode on any flat setup. If a game
  version moves that engine call, it falls back to drawing late, inside dxgi's
  own Present (pages flicker, nothing freezes; not behind an ENB or ReShade
  `d3d11.dll`).
- **A lost graphics device stops the overlay.** Magelight checks the device
  after every frame; once it is gone it logs the reason, turns the renderer off
  and drops UI mode, so the game is never left paused under a menu that cannot
  draw.
- **A UI pass with nothing bound hands pages to Present.** Thirty passes in a
  row with no render target switch the composite to Present for the session,
  rather than leaving an open page invisible.
- **A disabled renderer says why.** Every failed start step is logged (a device
  call with its HRESULT),
  and the reason travels with the `RenderDead` event and the `RenderDead` error
  text of `CreateViewEx` and `RequestUIMode`. A HUD notice pointing to
  `Magelight.log` shows once the HUD is up.
- The log names the present layers it recognises: NVIDIA's driver present
  layer (Smooth Motion), Streamline, Community Shaders' frame-generation proxy,
  and Skyrim Upscaler.
- No API changes; the npm packages stay at 0.30.0.

## 0.30.4

- **Pages invisible with NVIDIA Streamline upscaling (fixed).** Behind
  Streamline's swapchain (`sl.interposer.dll`: Community Shaders' and Open
  Shaders' DLSS and frame generation), a page opened (sound, paused game) but
  never showed, because frame generation drops what is drawn at Present.
  `composite` `auto` now draws in the game's UI pass when the game swapchain is
  Streamline's, not only when Skyrim Upscaler is loaded.
- **Two cursors at once with the UI-pass composite (fixed).** Only the Windows
  cursor was ever hidden; the game's own menu cursor kept drawing, under the page at
  Present but on top of it in the UI pass. Its cursor movie is now transparent while
  a page has the mouse (any reskinned cursor too) and visible again on exit. (The
  Windows cursor was not restored on exit but forced on; fixed in 0.30.6.)
- **A new cursor, drawn in code.** The flat cursor is a faceted steel arrowhead
  rasterized at the exact size it is drawn, so it is crisp at any resolution, and
  it follows the page: a brass glow fades in over anything clickable (CSS
  `cursor: pointer`), it shrinks while you click, and it becomes an I-beam over
  text. It is 24 px tall at 1080p (was 36): 48 px at 4K. The College pin image
  (`cursor.png`) no longer ships; `Magelight.json` `"cursorFile"` still loads your
  own image (its `cursorHotspotX/Y` now default to 0, the top-left pixel), and
  `"cursorHeight"` still sets the size. In VR with `"cursorDot": false` the laser
  end shows the plain arrow instead of the pin.
- No API changes; the npm packages stay at 0.30.0.

## 0.30.3

Three fixes for 0.30.2.

- **Crash at startup with Community Shaders, or minutes into play with ENB or
  an upscaler (fixed).** 0.30.2 asked the game's swapchain for its device on
  every frame, to skip swapchains on other devices, and released it. A swapchain
  wrapper that hands the device out without adding a reference lost one of the
  game's own each time, and the device was freed: within a second at a loading
  screen, about 8 minutes into a loaded game (an access violation in
  `d3d11.dll`, with or without `Magelight.json`). The vtable hooks now compare
  the swapchain with the game's own; only the dxgi detours, which see dxgi's own
  swapchain, ask for its device.
- **Crash at startup with ENB (fixed).** Behind a swapchain proxy (ENB's or
  ReShade's `d3d11.dll`, Skyrim Upscaler), 0.30.2 created a throwaway Direct3D
  device to find dxgi's own Present, and with ENB that killed the game with no
  crash log. `presentHook` `auto` now draws in the vtable hook behind a proxy, as
  0.30.1 did; only `"presentHook": "late"` still looks past one.
- **Pages cut off at low resolutions (fixed).** Below a device scale of 1,
  Ultralight drew only the top-left part of a page (scale squared of it: 64% at
  0.8), which hit screens under about 1067 pixels tall. `ViewDesc::uiScale` and
  `SetViewScale` now clamp to 1.0..3.0 (was 0.5..3.0) and log the raise; a page
  that wants to be smaller scales itself with a CSS transform.
- No API additions; the npm packages stay at 0.30.0.

## 0.30.2

Compatibility with mods that wrap the game's window or its frame output: a crash fix, and the
pages drawing behind upscalers and frame generation.

- **Crash with mods that subclass the game window (fixed).** Skyrim's window is
  ANSI, and many SKSE plugins subclass it with `SetWindowLongPtrA` and call the
  previous procedure directly. Magelight's input subclass is a Unicode one, so
  such a plugin got a handle it could only call through `CallWindowProc`, and a
  direct call crashed inside USER32 (an address like `0xFFFF...`). An ANSI shim
  now sits on top of the input subclass and hands every later subclass a real
  function.
- **Pages behind another Present hook.** Magelight now also hooks `Present1`,
  and when another plugin hooked Present first (an upscaler, a camera mod), it
  draws inside dxgi's own Present instead, after that plugin, so nothing draws
  over the pages. Behind a swapchain proxy it finds dxgi's own Present through a
  throwaway swapchain. It steps back to the plain hook when that Present is never
  reached. `Magelight.json` `"presentHook"`: `auto` (default), `late`, `vtable`.
- **Pages with Skyrim Upscaler and frame generation.** Skyrim Upscaler's HUD Fix
  keeps the game's UI apart from the scene and drops anything drawn at Present,
  so the pages opened (sound, paused game) but stayed invisible. Magelight can now
  draw the pages in the game's own UI pass: an invisible engine menu, open only
  while a page the player can click is visible, draws them into the UI's render
  target. When that pass does not run (loading screens, the console's `tm`), the
  pages are drawn at Present again within two frames. Click-through HUD pages
  still draw at Present, so they stay hidden behind the HUD Fix: the menu would
  keep Escape from opening the Journal for as long as one is up. `Magelight.json` `"composite"`: `auto`
  (default: the UI pass when Skyrim Upscaler is installed, never on VR),
  `present`, `ui`.
- **Diagnostics.** The log names the plugin whose Present hook Magelight found,
  the game swapchain (size, format, swap effect, flags), each swapchain presented
  and the plugin that presented it, and the UI-pass render target; every ten
  seconds it counts presents to swapchains other than the game's, while there are
  any.
- Third-party: MinHook (BSD-2-Clause), in `NOTICES.txt` and
  `THIRD_PARTY_LICENSES.txt`.
- No API changes; the npm packages stay at 0.30.0.

## 0.30.1

A fix for a hang on Skyrim VR, and the API call that lets a mod avoid the same one.

- **No more SKSE task posts from inside Present.** On Skyrim VR, SKSE also
  drains its task queue on game job threads and holds the queue's lock until
  the queue is empty. Magelight posted game-thread work straight to SKSE from
  inside the game's Present (GameThread events, EvalJS results, the Papyrus
  tier's events, sounds and cursor work) and from the main thread's input,
  window-message, engine-menu, SKSE-message and Papyrus handling. Such a post waited for that lock, and when the job
  being drained waited for the frame to finish, the game froze for good. It
  showed on the first load of a large save, when every view loads in the same
  frames as the save's post-load work. Those entry points now hold a scope,
  and every post made inside one goes to a queue that a host thread hands to
  SKSE, in order. Posts made elsewhere (inside SKSE tasks, from other threads)
  go straight to SKSE as before, so work a mod sequences
  after a UI-mode call keeps its order.
- **`PostGameTask(fn, user)`** (v4, gate 3001). The same handoff for mods: call
  it instead of SKSE's `AddTask` from a JS listener, a VR callback or, for a
  `RenderThread` mod, an event or EvalJS result.   Hang-safe from the host's
  callbacks; anywhere else it posts straight to SKSE, so a mod's own
  main-thread entry points still need their own handoff. A fault in `fn` is logged with its module
  and skipped. On an older host, post from a thread of your own. The threading
  contract in `MagelightUI_API.h`, [docs/CPP.md](docs/CPP.md) (with an example)
  and the C++ examples now say so.
- The cursor re-assert that follows a UI-mode entry no longer hides the
  cursor after an exit that landed first.
- **Stall watchdog.** The first stall sample of a session now logs every other
  thread's stack too (up to 64 threads, 24 frames each), so a hang names the
  thread holding things up, not only the one waiting. Each walk stays inside
  that thread's own stack.
- The npm packages stay at 0.30.0: the page contract did not change.

## 0.30.0

Three breaking changes to the sandbox, made before the first public release
because tightening them later would break mods that had come to rely on
them. No fields reordered; `NetworkPolicy` gains a value and `Magelight.psc`
one native.

- **Changed: the release is three plain zips, not a FOMOD.** The main
  download is the runtime alone, in the game's Data layout, so a mod
  manager installs it with no options to get wrong; the Web Inspector
  and the host's dev pages ship as `MagelightUI-<ver>-DevTools.zip` and
  the example mods as `MagelightUI-<ver>-Examples.zip`, both for mod
  authors. `tools/package.ps1` replaces `package_fomod.ps1`, and the
  `fomod/` folder is gone.
- **Breaking: network access is file-only by default.** A page reads files
  under its own mod folder and the host's runtime dir and reaches nothing
  over the network. Before 0.30.0 every page could fetch `http(s)` on
  loopback (`localhost`, `127.x.x.x`, `[::1]`) with no opt-in, so a
  data-only manifest mod could talk to any service running on the machine.
  A mod that relied on its own local server now opts in, per mod:
  - C++: `SetNetworkPolicy(mod, NetworkPolicy::LoopbackOnly)` for loopback,
    `NetworkPolicy::Any` for any host and scheme (unchanged), and the new
    `NetworkPolicy::FileOnly` for the default. The enum is now
    `{ LoopbackOnly = 0, Any = 1, FileOnly = 2 }`; the existing values keep
    their numbers. Gate `FileOnly` on `hostVersionNumber >= 3000`.
  - Manifest: a top-level `"network": "file"` (the default) or
    `"loopback"`. `"any"` is refused with a log line: internet reach stays a
    plugin's call.
  - Papyrus: `bool Magelight.SetNetworkPolicy(string modId, string policy)`
    takes `"file"` or `"loopback"` (any case) for a script-owned mod;
    `"any"`, an unknown word or a plugin-owned mod returns false.

  Each change is logged once per mod. Views created through the v1-v3
  `CreateView` belong to no mod: they are file-only now and cannot opt in.
  A page that needs loopback or `Any` must be created with `CreateViewEx`
  under a mod registered through v4 `RegisterMod`.
- **Breaking: the Papyrus tier acts only on script-owned mods.** A mod
  registered through the C++ API (`RegisterMod`/`RegisterModEx`, including
  a manifest mod a DLL adopted) is plugin-owned; a mod registered by
  Papyrus `RegisterMod`/`CreateView`, or declared by a manifest no DLL
  adopted, is script-owned. Papyrus `RegisterMod`, `CreateView`,
  `ReleaseUIMode` and `SetNetworkPolicy` refuse a plugin-owned slug (false
  or 0, one log line), and `DestroyView`, `ShowView`, `RequestUIMode`,
  `ReloadView`, `Call`, `Eval`, `EvalAsync`, `BindHotkey`, `SetCutout`,
  `SetHibernate`, `RegisterListener` and `PlaySound` do nothing (false or 0)
  on a plugin-owned mod's view. `FindView`, `IsVisible`, `IsUIModeActive`,
  `GetUIModeView`, `GetLastError`, `GetVersion`/`GetVersionString` and
  `IsReady` still answer for any mod. Before 0.30.0 a script could reuse a
  plugin's slug and create views under it (inheriting its network reach and
  storage), and could drive, `Eval` into and read the storage of any
  plugin's views. Among script-owned mods nothing changes: Papyrus cannot
  tell scripts apart, so any script still drives any script-owned mod's
  views, and their page storage is only as private as the script tier.

  Storage jars are shared by name, so the same refusals cover a
  script-owned mod whose jar a plugin uses: a manifest mod with
  `"session": "default"` (the jar of every v1-v3 view), a mod whose
  modId equals a plugin's custom `sessionName`, or a mod whose jar a plugin
  used in an earlier session: the host records every jar a plugin uses (its
  modId or its custom session name) in `Magelight-cache\plugin jars.txt`
  and keeps scripts out of those jars in later sessions, even with that
  plugin uninstalled, since the jar's data stays on disk. A manifest whose
  modId equals a plugin's custom session name is refused at load, and a plugin
  registering a session a script-owned mod already uses is warned in the
  log. A relative `CreateView` path must now exist under
  `Data/Magelight/<modId>/` (it used to fall back to the host's runtime
  folder). Each refusal is logged once per function and target, not once
  per function.
- **Breaking: a manifest's `modId` must equal its folder name** (letter
  case aside). A manifest naming another `modId` is not loaded, and the log
  names its path; leave `modId` out or give the folder's name. A folder
  whose name is not a valid slug (`[A-Za-z0-9_.-]`, 1..32 characters;
  `My HUD`, say) can no longer be rescued with an explicit `modId`: rename
  the folder. Before 0.30.0 a manifest in any folder could claim any slug,
  and a plugin registering that slug later adopted its views. A manifest
  now claims only its own folder's slug; the folder is the identity (see
  docs/MANIFEST.md).
- **Fixed (security): a page acts only as its own view.** The native
  bridge took the view id from the page's own argument, so a page could
  call `__mlNative('<another view id>', ...)` and fire that view's JS
  listeners (which then ran with that view's id), play sounds or change
  its Escape capture as it, and send keys through the VR keyboard's
  channel into whichever page held UI mode. A script could reach any
  plugin listener this way by evaluating that call in a page of its own.
  The bridge function now carries its page's view id and refuses a call
  made as any other view; only the host's keyboard page may send keys.
- **Fixed (security): the inspector is sandboxed like a page.** The
  DevTools view `ShowInspector` opens had no request filter, so it could
  read every file the host serves (its runtime folder, every mod folder
  under Data\Magelight and every registered page folder) and reach any
  host. It now reads only the host's runtime
  folder (where its own files are) and the inspected page's folder, and
  reaches nothing over the network.
- **Fixed: a view no longer keeps a stale network level** when
  `SetNetworkPolicy` runs while another thread creates a view of the same
  mod (Papyrus calls now run on parallel VM threads).
- **Fixed: a listener registered after the page loaded no longer replaces
  `window.magelight`.** A late `RegisterJSListener`/`RegisterJSListenerEx`
  or Papyrus `RegisterListener` (from a DOM-ready handler, or on every game
  load) re-ran the whole page bridge, which built a new `window.magelight`.
  `@magelight/sdk` and every `magelight.on` subscription stayed on the old
  object, so `useChannel`, `useUIMode` and host events stopped arriving from
  then on. The bridge core and its document listeners now install once per
  page; a later registration re-creates `__mlNative` and every listener's
  `window.<name>` shim, and keeps `window.magelight` and its subscriptions.
- **Fixed: `data-ml-sound` click sounds no longer repeat.** The same re-run
  added the click delegation again, so each late registration made a click
  play once more.
- **Changed: `magelight` and listener names starting `__` are the
  host's.** A listener under such a name replaced part of the host's page
  script (`window.magelight`, `__mlNative`, the reserved channels) and broke
  the page. `RegisterJSListenerEx` now returns `InvalidArgument` for them
  (`GetLastErrorMessage` says why), the v1-v3 `RegisterJSListener` drops
  them, and Papyrus `RegisterListener` returns false; the log names each
  refused name once.
- **Changed: example and doc hotkeys moved off the game's own keys and off
  each other.** F5 is Skyrim's Quicksave, F9 its Quickload and F12 Steam's
  screenshot key, and a HUD toggle leaves the key press to the game too.
  `Magelight.Badge`: badge F6, panel F7. `Magelight.ReactConfig` stays F8
  and the CppPanel example F13. The README Quick Start uses F11, the
  `create-magelight-view` template F10 and the docs/PAPYRUS.md minimal
  example Home (DIK 199). PageUp (DIK 201) is the host's default toggle key
  and is refused as a mod hotkey.
- **Fixed: a `<label>` wrapping its control plays its `data-ml-sound`
  once.** The label re-dispatches the click on the control, which played
  the sound a second time.
- **npm packages (0.30.0).** `@magelight/sdk`'s ESM import specifiers carry
  `.js`, so it loads under native Node ESM and `moduleResolution: nodenext`.
  `host.sound` no longer falls back to `send('__sound')` on hosts before
  0.29.0 (they have no such channel and logged a warning per call).
  `@magelight/react`'s `HostGate` installs the browser mock before its first
  check, so a page in a plain browser renders instead of the fallback. The
  `create-magelight-view` template dedupes React, so a `--local` scaffold
  no longer loads two copies and renders blank.
- **Packaging.** The standalone zip writes
  `SKSE/Plugins/Magelight/VERSION.txt`, the stamp SeverActions writes into
  its bundled copy. The host's test pages (`views/app`, `views/probe`,
  `views/*.html`, used only by the `demoViews` and `imageProbe` settings)
  moved from the required core to the Developer Tools group.
  `THIRD_PARTY_LICENSES.txt` is also at the root of the zip. `build.ps1`
  refuses an Ultralight SDK other than the build of record
  (`1.4.0b.081c48b`).
- **Example: `Magelight.InspectTarget`.** Its native is now
  `bool Open(Actor)`, which fills, shows and focuses the plugin-owned view
  (it was `Fill`, with the script showing the view, which 0.30.0 refuses).
- **Docs.** Papyrus ModEvent handlers take the standard four arguments
  (`string eventName, string strArg, float numArg, Form sender`); the
  two-argument form documented before does not match what the host sends.
  The host keeps mods, views, listeners and hotkeys in memory only, so a
  Papyrus mod registers again on every game load and finds its views with
  `FindView` rather than a saved id. JS listener callbacks
  (`RegisterJSListener`/`RegisterJSListenerEx`) run on the render thread,
  not on the mod's callback thread: copy the argument and `AddTask` before
  touching game state.

## 0.29.0

Two functions appended to the v4 table; one new capability.

- **Runs on Skyrim 1.7.99 / 1.7.104.** The CommonLib pin moved to MinLL/CommonLibVR
  4.39.5, which reads the format-5 address library the 1.7.99+ runtimes ship; earlier
  builds could not load on them. No API change; the VR build is unaffected.
- **UI sounds.** Ultralight has no media stack — `<audio>` and Web
  Audio do nothing — so the host now plays through the game's
  own audio (`BSAudioManager`, game thread), which also respects the player's
  UI volume. Names are the vanilla `UIMenu*` descriptors — `ok`/`click`,
  `cancel`, `prevnext`, `focus`/`hover`, `open`, `close`, `inactive`, resolved
  by FormID (VR has no reliable EditorIDs) — or `Plugin.esp|0xFormID` for any
  SNDR a mod ships — and every vanilla `UI*`/`ITM*` descriptor by EditorID
  (`UIJournalOpen`, `UISelectOn`, `ITMGoldUpSD`: 134 names from a generated
  table, so a page can pick the exact sound the vanilla menu uses there);
  `none` opts an element out. An unknown name logs once and stays silent. Four ways in:
  the markup convention `data-ml-sound="click"` / `data-ml-sound-hover="focus"`
  (the injected bridge delegates both; hover throttled to one per ~60 ms per
  view, page-driven sounds only while the view is visible); the page verb
  `magelight.sound(name)` (SDK `host.sound`, React `useSound()`); the C++
  `PlayUISound(view, name)` and Papyrus `Magelight.PlaySound(view, name)`; and
  per-view open/close sounds on UI-mode enter/exit — `SetViewSounds(view, open,
  close)` or manifest `"sounds": { "open": "open", "close": "close" }`, off
  unless asked for. `QueryCapability("sound")`. No file playback by design
  (SECURITY.md).
- **Flat cursor a little smaller.** Default `cursorHeight` 44 → 36 px at 1080p (still scales
  with resolution; `Magelight.json` overrides as before). VR's `vr.cursorScale` is unchanged.
- **Only visible views are rendered.** The present loop calls `Renderer::RenderOnly`
  with the visible views instead of `Render()`, which painted every dirty page —
  a hidden popup with a spinner or a closed dashboard with a blinking caret was
  repainted every frame whenever any other view kept the loop rendering. Hidden
  pages keep their dirty flag and paint on their next Show.
- **VR: the built-in B/Y exit yields to a mod that bound B/Y.** While a view is
  in UI mode, B/Y on either controller leaves UI mode (0.17.3). When the view's
  own mod has bound B/Y as a VR hotkey (bare or chorded, `BindVRHotkey` /
  `BindVRHotkeyCallback` / manifest `vrHotkey`) the built-in exit now stands
  down and the binding alone decides — before, one Grip+B/Y press ran both, and
  whichever game-thread task landed first decided whether the menu closed or
  closed-and-reopened 12 ms later (SeverActions, Skyrim VR field log
  2026-09-12: the menu could not be dismissed). Views whose mod did not bind B/Y
  keep the built-in way out. New internal `VR::ViewBindsButton`; no ABI change.
- **Removed: the PrismaUI compatibility surface.** The `PrismaVR_DeliverChar`
  / `PrismaVR_DeliverVKey` DLL exports are gone (OpenComposite's keyboard
  reaches a focused page through the window message instead);
  `Magelight_DeliverChar` / `Magelight_DeliverVKey` remain. The
  `window.__prismaNativeImeFocusChanged` page shim is gone too: the bridge
  detects text-field focus by itself, and a page that still calls the
  global unguarded now throws.

## 0.28.2

One function appended to the v4 table.

- **Per-mod network policy.** `SetNetworkPolicy(mod, NetworkPolicy::Any)` lets a
  mod's pages reach any host over any scheme — the reach a page has in a browser —
  for that mod only; every other mod keeps file + loopback. Set
  it once after `RegisterMod`; it covers views created before and after, and the
  log names the mod that opted in. `QueryCapability("networkpolicy")`. Added for
  SkyrimNet's dashboard (model catalogs on openrouter.ai, presets and plugin
  indexes on raw.githubusercontent.com, whatever the user set as provider).

## 0.28.0

The host-side prerequisites for running SkyrimNet's in-game dashboard on
Magelight. No fields reordered; six functions appended to the v4 table.

- **Loopback allowed.** Pages may fetch `http(s)://127.0.0.1`, `localhost`
  and `[::1]` (a mod's own local server); every other network request stays
  refused. `QueryCapability("loopback")`.
- **Escape ownership.** `SetEscapeCapture(view, true)` (or, from a page,
  `magelight.send('__escapecapture','1')`) delivers Escape to the page as a
  key instead of leaving UI mode — a modal or an editing control closes
  itself. Honoured on both Escape paths (window proc on flat, the input sink
  on VR).
- **Inspector API.** `ShowInspector(view, show)` / `IsInspectorVisible`.
- **View order.** `SetViewOrder` / `GetViewOrder` (explicit z within the
  layer; `RaiseView` still means "above everything").
  `QueryCapability("vieworder")`.
- **Scroll step.** `SetScrollStep(view, px)` — pixels per wheel notch.
  `QueryCapability("scrollstep")`.
- **Keyboard mute drains DirectInput.** While a page has focus on flat, the
  keyboard mute now drains the device's buffered queue each poll instead of
  skipping the poll — keys typed into a page no longer replay into the game
  (journal, map) the moment the menu closes.
- Console messages need no new API: they already arrive as the
  `ConsoleMessage` host event (level, line, text).


## 0.27.0

- **Native IME (CJK input).** The window's
  IME context attaches only while a text field in the UI-mode view has focus
  (the bridge detects focus, so pages need no code) and detaches otherwise,
  so gameplay keys never raise a composition. Composition text is drawn
  inline in the field (it rides as the selection; the commit arrives as
  ordinary characters and replaces it, a cancel deletes it). The OS candidate
  list opens at the caret the page reports. Keys the IME consumed
  (VK_PROCESSKEY) are swallowed before the page sees them, so Enter, Space
  and Escape mid-composition go to the IME, not to your handlers. Characters
  outside the BMP arrive as one event instead of two broken halves.
  `magelight:ime` window event `{state, text}` for
  indicators; `QueryCapability("ime")`. Flat only.


## 0.26.12

- **VR: the laser-end pointer is a point.** A small white disc with a dark
  rim replaces the 32px arrow sprite at the end of the laser, drawn at
  `vr.cursorScale` x the view height (default now 0.012, was 0.05; floor
  0.004) with a centred hotspot. `"cursorDot": false` in the `vr` block
  brings the arrow (or your custom cursor PNG) back. The desktop cursor is
  unchanged.


## 0.26.11

- **RequestUIMode retargets the pause flag on a same-view re-request, in
  place.** A mod that already holds UI mode on a view can re-request it with
  or without `kUIModeFlagPause` to unpause / pause the game without leaving
  the mode: the live focus menu's `kPausesGame` and the engine's pause
  counter are adjusted directly. Cursor, controls and text entry are
  untouched, and no menu is hidden or re-shown — 0.26.10 did it by hide +
  show, and a holder's close detection read that as the engine taking its
  menu away (SeverActions auto-closed on the spot); withdrawn. Before 0.26.10
  the re-request returned Ok and silently kept the old pause. Internal
  `SetUIModePause`.

## 0.26.9

- **Real device scale.** `ViewDesc::uiScale` is Ultralight's device scale
  (the page's `devicePixelRatio`), and `SetViewScale(view, scale)` changes
  it on a live view; the render thread applies it. Clamped 0.5..3.0; cutout
  and image rects are in view pixels (CSS px x scale). Gate on
  `hostVersionNumber >= 2609`.

## 0.26.8

- **`FindView(mod, name, &id)`** appended to the v4 table — the C++ twin of
  the Papyrus FindView, for driving a manifest-declared view from a DLL.
  Gate on `hostVersionNumber >= 2608`.
- Manifest robustness (a mistyped scalar warns and falls back instead of
  aborting the file; unknown keys are logged with a did-you-mean) and the
  `MagelightUI_Mod.h` header-only wrapper.

## Runtime / packaging (no ABI change)

- **0.28.7** — vcpkg baseline moved from 2022-12-20 to 2025-12-12 (`ee12231b`, the commit upstream CommonLibSSE-NG 4.17.0 builds against): fmt 9.1.0 → 12.1.0, spdlog 1.11 → 1.16, DirectXTK/DirectXTex 2022 → 2025-10-27, xbyak 6.60 → 7.28, nlohmann-json 3.11 → 3.12. fmt 9.1.0 no longer compiles on MSVC 14.51 (VS 18's STL removed `stdext::checked_array_iterator`), which broke CI and would have broken the next local Visual Studio update; the overlay port now carries upstream's dependency floors so the library can't be paired with 2022 dependencies again. No source changes were needed; CI builds on the VS 18 image again.
- **0.28.6** — trust-boundary hardening (no ABI change). Slug validation refuses dot-only values, a trailing dot and the Windows device basenames (CON, PRN, AUX, NUL, COM1-9, LPT1-9) — a manifest-only mod could point its session storage outside the cache root. Papyrus `CreateView`: an absolute `htmlPath` must resolve inside the game root; a non-absolute one must be genuinely relative (no root name or root directory — a root-relative or UNC path replaces the base on join) and free of `..`, and the probed file must still sit inside `Data\Magelight\<modId>\`; the C++ tier keeps unrestricted absolute paths. Mod-supplied view and listener names are JSON-quoted before landing in generated page script. The trust model is documented (`docs/CPP.md`, `api/MagelightUI_API.h`): views are addressable, not owned.
- **0.28.5** — review fixes: the stall watchdog's heartbeat is stamped at the present hook's entry, so loading screens and menus (which present from a foreign thread) no longer read as stalls and burn the sample budget; the watchdog captures the thread context under suspension and walks the stack only after `ResumeThread` (the walk takes ntdll function-table locks a suspended thread may hold); `SetViewScale` is applied by the render thread instead of re-laying out WebKit on the caller's thread; the loopback allow-list accepts `127.x.x.x` as a dotted quad only (a `127.` hostname prefix passed); the 0.26.11 pause retarget does its `RE::UI` work on the game thread; the engine `Cancel` path honours `SetEscapeCapture`; the 0.28.0 view setters share the v4-view gate and record a `GetLastErrorMessage` on refusal; `GetViewInfo` returns the live `uiScale`; `host.caps` gains `networkPolicy` and the header/mock lists are synced; manifest `vr`/`vrHotkey` blocks use the non-throwing reads; `MagelightUI_Mod.h` compiles (`DomReadyFn`) and its snippet names a real key (F13, 0x64).

- **0.28.4** — a hibernated view keeps its controller binding. Hibernation (a view hidden past its `SetViewHibernate` budget) released the overlay through the same path a destroy takes, and that path also dropped the view's VR hotkey — so a popup bound to a chord went deaf 30 s after it was last hidden and only answered again after a rebind (SeverActions' quick wheel: worked once per session or per rebind, then never). Only a destroy unbinds now; the chord fires on the dormant view and wakes it.
- **0.28.3** — stall watchdog: when no frame has been presented for 1.5 s while the world is live, the thread that presented the last frame (and the main thread, if different) is suspended, its stack walked with the OS unwinder, resumed, and the frames logged as `module+offset` — a process dump's headline, captured automatically, for the freezes and soft stalls testers cannot otherwise document. One sample per 5 s, 12 per session; `Magelight.json` `"stallWatchdog": false` disables, `"stallThresholdMs"` tunes.
- **0.28.1** — a page living OUTSIDE `Data\Magelight` (an absolute path, e.g. a dashboard shipped under another mod's own folder) now actually loads: the per-view pin already granted its own folder, but the view-blind file-system layer beneath it only served `Data\Magelight` and refused every read, so the view came up empty. That layer now serves the union of registered page roots.
- **0.26.7** — review fixes: VR chord latch/debounce/seed correctness, laser hit-test clamp, thread-safe `GetPlacement`, `SetViewVRPlacement(nullptr)` restores the layer default, `EvalJS` context no longer leaks on view destroy.
- **0.26.5** — `Magelight.json` `fontHinting` / `fontGamma` (text rasterization tuning).
- **0.26.x** — `tools/package_fomod.ps1 -Tester` (runtime-only zip); the probe page moved behind `-Examples`.

For the full commit history see the git log; the API contract itself lives in `api/MagelightUI_API.h`.

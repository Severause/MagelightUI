# Troubleshooting

Everything below starts in **`Magelight.log`**, in `Documents\My Games\<game>\SKSE\`, where
`<game>` is `Skyrim Special Edition` (Steam SE/AE), `Skyrim Special Edition GOG` (GOG) or
`Skyrim VR`. On a PC whose Documents folder is in OneDrive, that is
`OneDrive\Documents\My Games\...`. The host logs one line per view it creates, per manifest it
reads, per page that fails to load, and per hotkey press. Read it before anything else, and copy
it before you start the game again: each launch overwrites it. Line one names the running
version (`Magelight v1.1.0 loading`); the lines after it name the game (SE, AE or VR, its version and
SKSE's), Windows (or the Wine under Proton), the CPU and RAM, and, once the game's data has loaded,
every graphics adapter with its driver version, the one the game renders on marked. Paths in the
log show your user folder as `%USERPROFILE%`. Each line starts with the date and time, a level
letter (`I`, `W`, `E`) and the thread id.

## Nothing from any Magelight mod appears

- **`the Ultralight runtime under ... is version '...', but this Magelight.dll was built for ...`**
  (0.31.5): a mod manager paired `Magelight.dll` from one mod with the runtime DLLs of another
  (two mods bundle Magelight and neither wins every file). The host stays off for the session
  rather than running a runtime it was not built for. Let ONE copy win every Magelight file: in MO2
  the newer copy lowest in the left pane.
- **`failed to load <runtime DLL> (GetLastError=126)`**: Windows could not load the Ultralight
  runtime. With no Visual C++ redistributable at all the line names `Magelight1Core.dll` (the
  first one loaded); with an old one it usually names `MgWCore.dll`. First check that `Magelight1Core.dll`, `MgWCore.dll` and
  `Magelight1.dll` are in `Data\SKSE\Plugins\Magelight\` (a missing file gives the same code;
  a leftover `MgACore.dll` from an older install is never loaded and does no harm). If they are, install or repair the **Microsoft Visual C++ Redistributable
  2015-2022 (x64)**, latest version: the runtime needs `MSVCP140_2.dll` and
  `VCRUNTIME140_1.dll`, which older builds of it lack.
- **`Ultralight runtime not found — overlay disabled this session`**: the runtime folder is
  missing or incomplete. Reinstall Magelight UI, and if another mod also ships Magelight, see
  "Two copies of Magelight" below.
- **No `Magelight.log` at all**: SKSE did not load the plugin. Check the requirements for your
  runtime (SKSE64 or SKSEVR, and the matching Address Library) and SKSE's own log.

## Blank page?

The number one question for any web-in-game UI. In order of likelihood:

1. **The page did not load.** Look for `view N page '...' missing — inline banner fallback` or
   `view N FAILED to load - ...` (the mod also gets a `ViewLoadFailed` event). The path is
   printed exactly as resolved. Manifest and Papyrus paths are relative to the mod folder (a
   Papyrus page that is not there is refused); C++ paths are relative to
   `Data/SKSE/Plugins/Magelight/` unless they start with a drive letter.
2. **Vite put `crossorigin` on the script tag.** Over `file://` the page origin is null and the
   module is refused silently. Use `@magelight/vite-plugin`, or strip the attribute and set
   `base: './'` yourself.
3. **Modern syntax.** Ultralight's WebKit (615, in 1.4 and 2.0) parses through `es2022`; newer is
   unverified. Target `es2022` (the vite-plugin's default) — and do not lower the Ultralight
   SDK without dropping to `es2019` (1.3-era WebKit 610 rejected `?.`/`??`).
4. **A JS exception during mount.** With `"devMode": true` in `Magelight.json` it paints a red
   banner on the page; with or without it, it is in the log as `[view N console/ERROR]`. An
   exception thrown inside a call the mod made into the page is logged by the call that ran it
   (1.5.0): `[view N InvokeJS/ERROR]` (C++ `InvokeJS`, Papyrus `Eval`) or
   `[view N InteropCall('receivePageData')/ERROR]` (the page function the mod called), with the
   page file, line and column the error was made at and the stack. `line 3:31 of an unnamed
   script` there means no stack frame named a script when it was made, so it was made in code
   that runs unnamed: usually the mod's own script (its stack frames end in `@`, with no file),
   though a page's eval or `new Function` code that script called directly reads the same way.
   With no place on that first line but stack frames below it, a page file was on the stack and
   unnamed code it reached made the error: eval or `new Function` code, or a function of the
   mod's own that the page's code called (a callback, whose frame is a bare `@` above the page's).
   The first frame with a file is where the page reached that code, not the line that failed.
   With no place and no frames, the value thrown was not an Error made by script: a thrown
   string or object, or a DOMException from a DOM call (`querySelector` with a bad selector reads
   `SyntaxError: The string did not match the expected pattern.`), and only its text is known.
   An `Error` subclass is made at its constructor's `super(...)` call, and for a class with no
   constructor of its own (`class E extends Error {}`) that is JSC's built-in one, so whoever
   owns the class it reads `line 1:28 of an unnamed script` when the mod's script makes it and
   no place when page code does: give the class a constructor to have a line in your script.
   `InteropCall('__uimode')` or `InteropCall('__tooltip')` at `magelight:dispatch` means the
   page replaced `window.magelight` without its `_dispatch`, so the host's own messages fail.
   `[view N EvalJS/caught]` (devMode only) is an exception the mod's `EvalJS` or `EvalAsync` asked
   to catch. `the bridge script (page core) failed on view N` means the page has no
   `window.magelight` at all.
5. **It loaded but is transparent.** Pages start with a transparent background; if your CSS
   sets `html { background: transparent }` and nothing else paints, you see the game. Give the
   root element a background.
6. **The view exists but is hidden.** `startVisible` defaults to false; a manifest panel shows
   only when its hotkey (or a DLL/script) opens it. Check the `hotkey <scancode> -> view N`
   lines.

## A page cannot reach its server

Since 0.30.0 a page reaches nothing over the network unless its mod opts in: loopback (a server
on this machine) through the manifest (`"network": "loopback"`), Papyrus `SetNetworkPolicy` or
the mod's DLL; any other host only through the DLL. The log names a mod's policy when the mod
changes it. Since 0.31.5 the policy is a Content-Security-Policy written into the page, so a
refused request shows in the page console as `Refused to connect to ...` (in `Magelight.log` with
`consoleLog` at `warnings` or above); the log's own `blocked network request` line covers only
what the policy let through. Loopback means `localhost` or `127.0.0.1`: a page that used `[::1]`
or another `127.x` address switches to one of those. A page cannot `fetch()` or XHR its own files
either (import or inline the data; a Vite build sets `build.modulePreload.polyfill = false`, which
`@magelight/vite-plugin` does, or it logs one refused fetch per page). Views created through the
v1-v3 C++ `CreateView` belong to no mod: they are file-only and cannot opt in.

## The hotkey does nothing

- A refused binding is logged with the mod and the reason:
  `BindHotkey: scancode N is bound by <mod>` (the first binder keeps a key), or
  `BindHotkey: that scancode is the host's own toggle key` (PageUp unless `toggleKey` in
  `Magelight.json` says otherwise). A manifest adds its file and view name to the line.
- A hotkey never *opens* a page while an engine menu, the console, or a game text field is
  active (`hotkey ignored — an engine menu, the console or text entry is open`), and while a
  page has key focus only that page's own key fires (to close it).
- The user may have rebound or disabled it in `Magelight.json` (`"hotkeys"`); the log says
  `hotkey for 'Mod/view' rebound ...` or `disabled by Magelight.json`.

## I can move but the page stays / I cannot move

UI mode = cursor up + game controls suspended + keyboard to the page. To leave it: Escape,
unless the page has taken Escape for itself (a dialog closing itself); the page's own hotkey,
which always closes it; or the host toggle key (PageUp by default), which always leaves UI mode.
On VR, B or Y leaves it unless the mod bound those buttons itself. On VR the game gets its
controls back only once the buttons pressed to close the page are released (`controls held off until
the controller buttons are released`, then `controls restored (...)`, 1.5 s at most), so the
closing press never reaches the game. Leaving UI mode does not hide
a page by itself (a mod may want it visible, HUD-style); a hotkey close does hide. If controls
stay suspended after a crash-to-menu, load again: the host resets UI mode on every load.

## Two mods fight over the screen

One view holds UI mode at a time, across all mods. The second requester is told `Busy`, the
holder hears `FocusDenied`, and `kUIModeFlagQueue` waits its turn instead. This is by design; a
mod that must be on top uses the `popup` or `system` layer for stacking, not UI mode.

## Two copies of Magelight

A mod such as SeverActions can ship its own copy of Magelight. If you also install Magelight UI
by itself, the copy that wins the file conflict is the one that runs; in MO2 that is the mod
lower in the left pane. Let the newer copy win, and let it win every file: a `Magelight.dll` from one
copy over the runtime DLLs of another is refused at load (0.31.5; the log names both versions)
and the host stays off. A `MagelightGPU.dll` built for another Ultralight SDK or GPU contract, or from
before 0.31.6, is refused too (0.31.6): pages still show, drawn on the CPU, which costs frame time, and
the log says `MagelightGPU.dll does not match this Magelight.dll - ...` with what differs. Line one of `Magelight.log` names the running version, and a "Magelight
UI needs updating" page means an older copy won.

## Text fields do not type / paste

Ctrl+C/V/X work as of 0.10.2. If typing lands in the game instead of the page, the view is not
the UI-mode view: only the focused view receives keys.

In VR, Magelight's keyboard rises when a field of the page holding UI mode takes the focus. Closed with its
Close key, it comes back when you click inside that field again; a click elsewhere on the page leaves it down,
and a field with `inputmode="none"` never raises it (the page has its own keypad). From 1.5.0 `Magelight.log` has
a line for each change: `text focus view N {"mode":"text",...} -> keyboard up` (`null` when no field
wants a keyboard; on flat the line ends `-> IME on` or `off`; the first 32 of a session). If a page reacts to Backspace, Enter or the arrows but not to
letters from a VR keyboard, it reads letters from `keypress` or `keydown` in a text field: since 1.5.0 typed text
reaches a field as `input` events, as on a phone, and only those keys are key events.

## A key or a mouse button reads wrong in a page

From 1.5.0 a page's key events carry `code`, `location` and a `key` from the player's own keyboard
layout, on flat and in VR, and Alt and F10 reach the page. `code` names the key's place on a US keyboard,
as in a browser (the key labelled A on a French AZERTY keyboard reads `KeyQ`), so a page that binds a
hotkey by turning `code` into a Windows key code binds the key at that place on a US keyboard: on
AZERTY, pressing A binds Q. A capture written that way does so until it reads `keyCode` (unchanged)
first; report such a page to its mod. If a page sees Ctrl, Shift or Alt missing from
its key events (Ctrl+A selects nothing), report it with `Magelight.log`: the page's events read those keys
through Magelight's runtime, which the polled-keys filter leaves alone by its folder's name
(`SKSE\Plugins\Magelight\`). `metaKey` is always false. The
first press of mouse button 4 or 5 over a page in a session is logged (`mouse button 4 pressed over a
page (sent as back)`); with no such line the game never reported the button. Those buttons, and a
middle click, give the page `auxclick`, never `click` or `dblclick`, so they do not press buttons, follow
links or run double-click handlers.

## Performance

Each visible view costs a texture and a paint when it changes; idle pages cost nothing. Keep
HUD widgets small, avoid `position: fixed` full-screen containers on HUD layers, and prefer
`startVisible: false` for panels. Fullscreen views repaint on resolution change. A page's
JavaScript runs on the game's main thread, so heavy script work, or console output in a loop,
costs frame time; strip console logging from release builds.

Since 1.5.0 (Ultralight 2.0, Free edition) a page animates and repaints at most 60 times a second, whatever the
game's frame rate: its `requestAnimationFrame` callbacks, CSS animations and transitions, smooth scrolling and the
changes its script makes wait for the next of those frames, while input, a resize and opening the page paint at once.
On a 90-144 Hz display or a VR headset a page's animations step at 60 a second; before 1.5.0 they followed the frame
rate. `Magelight.log` states the cap at startup (`pages animate and repaint at most 60 times a second`), and
`animationTimerDelay` no longer changes anything.

Since 1.5.0 a hidden page runs no `requestAnimationFrame` or CSS animation and is never painted, so
a closed panel's spinner costs nothing; its timers still run at full rate, so a closed page that polls
with `setInterval` costs script time until it hibernates. A page that finishes closing on
`transitionend` or `animationend` after its view is hidden does that at its next open instead. Its
`resize` and `scroll` events, `matchMedia` `change` events and `ResizeObserver` /
`IntersectionObserver` callbacks wait for the next open too and arrive once in its first frame, so a
panel that fits its layout to the window after a debounce timer opens at its old layout after a
resolution change (or a `SetViewBounds` / `SetViewScale` while it was closed) and jumps when the timer
fires; do that work in the handler.

Loading a page costs frame time too (its parse and first script run). Since 0.31.0 the pages of
views that have not been opened start loading one per frame after a save loads, instead of all in
the first frames, which is meant to spread that cost (not yet measured in game); a page you open
loads at once, and a batch lasts at most about a second. `Magelight.log` sums each batch:
`staggered load - N views over F frames in T ms (...; worst Update X ms)`. A single page whose own
parse is long still costs one long frame. If a mod's page is not ready right after a load because
of this (a mod that waits for its page before it opens it), set `"loadStagger": false` in
`Magelight.json` (and tell the mod's author: a page that is shown loads at once).

A view with prepaint on (`SetViewPrepaint`, manifest `"prepaint"`) is painted while hidden about 1.5 s
after it loads (`view N prepainted while hidden`) and, from 1.5.0, once more when its page settles
(`view N page settled - ...`, then `view N prepainted while hidden again, its page settled`): Ultralight
2.0 decodes a page's images only once it is painted, so the second paint is the one that takes them off
the first open, and it is the longer of the two on a page with many images. Each is one frame during
play, never in VR, and the second comes at most once per load. A page settles only after it has been
painted (shown or prepainted) since its last navigation, once per navigation: a change of the URL's
fragment (`location.hash`) is one, so a page that changes it as it runs can settle many times in a load,
and one that changes it after its last paint does not settle until it is painted again.
The first 64 settles of a session are logged. A prepaint view whose page never settled while hidden keeps
only its first paint, and has no such line after its `prepainted while hidden` one: it kept changing its
layout from a timer (CSS animations and `requestAnimationFrame` are paused while it is hidden, so they do
not stop it), kept making requests, or changed its fragment after that paint (`location.hash` set from a
`visibilitychange` handler, a timer or a host call while hidden). A settle that came while another thread
held the view registry logs `a page settled while the view registry was busy - no settle repaint for it
this load` instead (the first 64 of those, counted apart from the settles), with no view number; that page
also keeps only its first paint for the load.

Ultralight 2.0 (Magelight 1.5.0) draws page shapes analytically, and no page target measured has
asked for MSAA, so `msaa` costs nothing unless one does: the memory line below says
(`MSAA 4x offered, no page target asked for it`). Pages run with Ultralight's compositor on: what it
composites (animated or transformed layers, such as a transform-scaled shell) holds tiles of its own,
the memory line's `page tiles`. Measured on SeverActions' five pages (desktop, 2560x1440 main page),
that is about 70 MB more video memory than with the compositor off (113 MB of render targets against
43 MB), for pages that paint correctly at any scale and tell Magelight exactly when they changed, which
the VR panel upload depends on.

### Memory

`Magelight.log` has a memory line about a minute after the first save loads, then every
`memoryLogMinutes` (10), about 5 s after UI mode ends (at most one a minute), and when a script or
the console asks for one (`cgf "Magelight.LogMemoryUsage"`, which also writes Ultralight's full
memory report):

```
Magelight: memory (periodic) - JS 5.1 MB in 5 documents, WebCore 16.9 MB, script cache 11.1 MB, image cache 0.0 MB,
page tiles 72.4 MB, GPU textures 16.3 MB + render targets 112.6 MB; game process private 7.8 GB; views 5 live,
0 hibernated, 2 not loaded; MSAA 4x offered, no page target asked for it; system RAM ...; read in 0.9 ms
```

`JS` is the pages' script heap and `documents` the pages loaded, `WebCore` the engine's own heap, the
two caches what it keeps of loaded scripts and decoded images, `page tiles` the compositor's layer
tiles, and `GPU textures` and `render targets` Ultralight's video memory (each page's own target and
its tiles). `game process private` is the whole game, not Magelight. A `?` is a figure this
Ultralight version no longer reports. A view that hibernates (`SetViewHibernate`) gives its script
heap, caches and process memory back within a few seconds, and its share of Ultralight's video memory
(`page tiles`, `GPU textures`, `render targets`) within about 20 s, whether or not another view is
shown: a memory line taken in between still counts that video memory, which is not a leak. Magelight never forces a
collection, because on Ultralight 2.0 that throws away the compiled script of every page.

## A page's console lines are missing from the log

Since 0.31.0 `Magelight.log` records a page's console warnings and errors only, as
`[view N console/warn]` and `[view N console/ERROR]`: pages log whatever they like, settings and
keys included, and the log is the file players post. To see `console.log`, `info` and `debug`
lines too, set `"consoleLog": "all"` (or `"devMode": true`) in `Magelight.json`, reproduce, and
set it back before posting the log anywhere. Other rules:

- A line ends with where the message was written: `(file:///.../app.js:12:34)` is line 12,
  column 34 (1.5.0; earlier hosts give the line only). An `Error` passed to `console.error`
  reads as its message, the JSON of any fields of its own (`Error: x {"code":"E_FOO"}`), then its
  stack, one frame a line (1.5.0; it was that JSON alone, `{}` for an Error with no fields). An error
  nothing caught in a timer or an event handler has no stack: Ultralight passes only its message.
- A message longer than 2 KB is cut, ending in `… [N bytes]` with its full length.
- An exception a mod's call left uncaught (`[view N InvokeJS/ERROR]`,
  `[view N InteropCall('fn')/ERROR]`, 1.5.0) is an error line: it is written unless `consoleLog`
  is `none`, shares the view's 20 lines a second, and its stack is cut to 12 frames in the log.
- A view writes at most 20 console lines a second; the rest are counted, and the view's next
  logged line, or its destruction, is preceded by `[view N console] N lines not logged (over 20 a
  second)`.
- The mod that owns the view still receives every message in full through its `ConsoleMessage`
  event, whatever the log records; a mod that forwards its console lines elsewhere is unaffected.
- The settings line (`settings loaded (... consoleLog=warnings)`) names the level in effect.

## The cursor looks different over one mod, or disappears

Since 0.31.0 a mod can bring its own cursor images for its views, and a page that draws its own
pointer can hide the host's (CSS `cursor: none`, or `"none"` in its manifest). Over such a view
you see the mod's cursor, or the page's own, instead of Magelight's; elsewhere, and as soon as
the pointer leaves that view, Magelight's cursor is back. A mod's cursor images win over your
`cursorFile` over that mod's views; a mod's cursor colours (0.31.1) never do. To keep your own cursor everywhere set `"cursorForce": true`
in `Magelight.json`; to drop mods' images but let pages that draw their own pointer keep doing so,
set `"modCursors": false` (since 0.31.1 that also drops a mod's cursor colours, `view N cursor
tint: ...` in the log). `Magelight.log` names each view's cursor (`view N cursor: ...`) and
each image it loads, or why one failed (`cursor image ... - not an image Windows can read`); a
failed image shows Magelight's cursor instead. In VR the laser keeps its dot; with
`vr.cursorDot` `false` it shows the mod's image. If another cursor or a menu draws over
Magelight's, see "Something draws over the page or its cursor".

## Something draws over the page or its cursor

In the UI-pass composite (`composite` `ui`, or `auto` behind an upscaler or a swapchain wrapper)
pages are drawn while one of Magelight's own engine menus draws, at depth 9. A menu another mod
draws higher, or one at the same depth opened later, therefore covers the pages and the cursor:
a second cursor on top of Magelight's, for one. Shortly after a page takes the mouse,
`Magelight.log` names those menus (`menus drawn over the pages: <menu> (<depth>), ...`, or
`none`; Magelight's own focus menu and the game's cursor are left out of the list), again only when
that list changes, re-checked every second while a page holds the mouse; outside the UI pass it says
the views draw after every menu instead. It also records every menu's depth and flags the first time
the menu opens (`menu '<name>' depth <n> flags ...`). Something drawn at Present, an ImGui overlay for
one, is not a menu: it can cover the pages whatever the composite, and is not listed.

For a second cursor over the page, read these lines:

- `the game's cursor while a page holds the mouse: ...` (at each UI-mode entry, when it changes): the
  Cursor Menu's depth, whether its sprite is `hidden` (Magelight hides it) or `DRAWN`, whether it draws
  over or under the pages, the movie it loads (a cursor replacer names its own file), and whether
  Windows' cursor shows over the game.
- `the game's cursor draw is being skipped ...` (once a session): `skipGameCursor` is working, so the
  game's own cursor cannot be the second one; read the menu list and the Windows line instead.
- `something showed the game's cursor again ...`: after Magelight hid the sprite, something set it
  visible again, usually a cursor replacer's own script or another mod. With `skipGameCursor` (the
  default) it is only noted, since the sprite is not drawn anyway. With `skipGameCursor` false it is a
  warning: Magelight hides it again every second, so it flickers or seems stuck; try without the cursor
  mod the line's movie names.
- `Windows' own cursor shows over the game ...` (a warning): the second cursor is Windows' arrow, not the
  game's. Alt-tabbing out and back usually hides it.
- `Windows cursor kept inside the game window ...` (the first few UI-mode entries): since 1.5.1 Windows' cursor
  is clipped to the game window while a page holds the mouse, so a freed one (a second UI host such as PrismaUI
  holding focus, a non-exclusive mouse) can no longer reach another monitor and click there as well as on the
  page. `Windows cursor released (...)` says when it was let go. `"clipCursor": false` turns it off.
- `menu 'Cursor Menu' CLOSED` then `OPENED while UI mode is on`, usually right after another menu opens: the
  game rebuilt its cursor, which starts visible. Since 1.2.0 it is not drawn at all with `skipGameCursor`,
  and hidden at once otherwise; before, it showed for up to a second each time.
- `menu 'Cursor Menu' CLOSED after UI mode ended` (for three seconds after a page closes, with the focus menu
  and `MagelightOverlay`): the cursor's menus closing. One close per exit is normal; a CLOSED then OPENED pair
  there is the game rebuilding its cursor.

`"composite": "engine"` draws the pages after every menu, from the engine's end of frame; it has
not been tried with an upscaler's frame generation, so check that the pages still show with it on.

## Text after a page's scale changes

A mod changes a loaded page's scale one of two ways. `SetViewScale` keeps the page: it lays out again at
the new scale in the next frame (`view N device scale X` in `Magelight.log`). The same call made before
the page loads (a view created with a `ViewDesc::uiScale` logs it once) or while it is hibernated only
stores the scale, and from 1.5.0 its line says so: `view N device scale X (applies at its next load)`;
before 1.5.0 the two lines read alike, so a `device scale` line from before the view's DOM ready is its
creation, not a live change. `RebuildViewAtScale` loads
the page again at the new scale (`view N released for a rebuild at device scale X`, then the page's DOM
ready), so what the page held in script is gone. Measured offline on real mod pages,
going from 1.0 to 1.25, 1.35 or 1.5 and back, on Magelight's GPU driver and on the CPU renderer, a page
rescaled with `SetViewScale` draws as one loaded at that scale, text included, on Ultralight 1.4 and 2.0.
One difference is new on 1.5.0 (Ultralight 2.0): where the view's width or height divided by the scale is
not a whole number, as at most UI scales, a page whose view was resized or rescaled after it loaded
(`SetViewScale`, `SetViewBounds` to a new size, a fullscreen view after a resolution change) lays out a
pixel wider or taller than one loaded at that size and scale (`innerWidth` 1423 against 1422 for 1920 px
at 1.35), so an edge can sit a pixel off, and can move back after the view hibernates and wakes.
Mods use `RebuildViewAtScale` because `SetViewScale` crashed the game on hosts before 0.28.5, which made
the change on the caller's thread, and it has not run in game since. If text overlaps or goes soft after a
scale change on 1.5.0, report which of the two log lines came before it, with `Magelight.log`.

## Rendering oddities (Ultralight 1.4 GPU path)

A rounded box whose borders differ per side and is not fully opaque can draw wedge artifacts.
Keep such boxes opaque or drop the radius. Known engine defect.

## Where things are

`<My Games>` is `Documents\My Games\<game>` as described at the top.

| | |
|---|---|
| Log | `<My Games>\SKSE\Magelight.log` (overwritten each launch) |
| Settings | `<My Games>\SKSE\Magelight.json`, created by hand (see "Magelight.json" below) |
| Page storage | `<My Games>\SKSE\Magelight-cache\<modId>\`: one folder per mod, shared by every save, character and mod-manager profile, not rolled back when a save loads. Delete a mod's folder (game closed) to reset it |
| Runtime | `Data\SKSE\Plugins\Magelight\` |
| Manifest mods | `Data\Magelight\<ModId>\` |

## Magelight.json

Optional; every key has a default. Create it by hand beside `Magelight.log` (a copy in
`Data\SKSE\Plugins\Magelight\` is read only when there is none there). The log names the file
it read, or says that defaults are in effect.

```json
{ "toggleKey": "PageUp", "logLevel": "info", "hotkeys": { "Magelight.Badge/panel": "F4", "Magelight.Badge/badge": 0 } }
```

| Key | Default | Meaning |
|---|---|---|
| `toggleKey` | `201` (Page Up) | The host toggle key, which always leaves UI mode: a DirectInput scancode or a key name, as in `hotkeys` (key names since 0.31.2). `0` or `"none"` turns it off; Escape and each mod's own keys still leave UI mode. A Windows key code is not a scancode: F3 is `61` or `"F3"`, not `114` (the log warns about that mistake since 0.31.2) |
| `hotkeys` | none | Rebind or disable any mod's hotkey: `"ModId/viewName": "F7"` (a key name or scancode), `0`, `"none"` or `"off"` disables |
| `devMode` | `false` | Hot reload of page files and the on-page JS error banner; also writes every page console line to the log unless `consoleLog` says otherwise |
| `consoleLog` | `"warnings"` | Which page console messages go into the log (0.31.0): `none`, `errors`, `warnings` (warnings and errors) or `all` (`console.log`, `info` and `debug` too). Unset, it is `all` with `devMode` and `warnings` otherwise. See "A page's console lines are missing from the log" |
| `logLevel` | `"info"` | `trace`, `debug`, `info`, `warn` or `error` |
| `forceCpu` | `false` | Skip the GPU driver and use Ultralight's CPU renderer |
| `presentHook` | `"auto"` | Where pages are drawn onto the frame. `auto` draws inside dxgi's own Present when another mod hooked Present first on a plain dxgi swapchain (an upscaler, for one) and could otherwise draw over the pages, or for NVIDIA Smooth Motion when engine mode is off and d3d11.dll is not a proxy such as ENB's; never on VR. `late` tries it wherever dxgi's own Present can be reached: not behind a dxgi.dll proxy such as ReShade's, where it logs why and draws in the vtable hook. `vtable` never does. Try `late` when a page opens (sound, paused game) but stays invisible, but not with ENB: behind ENB's `d3d11.dll`, `late` can crash the game at start |
| `composite` | `"auto"` | The stage pages are drawn at. `present` draws them over the finished frame at Present; `engine` draws them from the engine's end of frame, after every menu, with the engine's own device (flat only; what `auto` uses under NVIDIA Smooth Motion); `ui` draws them in the game's own UI pass, as part of the game's menus (while no interactive page is open, click-through HUD pages draw from the engine's end of frame behind a swapchain wrapper (1.5.0), else at Present). `auto` uses `ui` when Skyrim Upscaler is installed or the game's swapchain is not dxgi's own: NVIDIA Streamline's (`sl.interposer.dll`: Skyrim Upscaler, Community Shaders' and Open Shaders' upscaling), since a HUD Fix keeps the game's UI apart from the scene and frame generation drops anything drawn at Present, or another wrapper such as ENB's or ReShade's `d3d11.dll` / `dxgi.dll` proxy (0.31.8), which may present a buffer other than the one pages are drawn into, but not a frame-generation swapchain on Direct3D 12 or NVIDIA Smooth Motion without engine mode; never on VR. Try `ui` when a page opens (sound, paused game) but stays invisible, and `present` if a page misbehaves only in the UI pass |
| `freezeWorld` | `true` | Lets mods stop the game's 3D world render behind a paused fullscreen page (0.31.0; flat only). `false` turns it off for every mod and restores 0.30.6's menu flags; try it if the background behind a page goes black or a page stops answering when it opens |
| `freezeWorldSkipCapture` | `true` | In UI-pass composite (`composite` `ui`, or `auto` behind an upscaler or a swapchain wrapper), keeps pages out of the frame the game freezes as the background. `false` if a page flickers when the freeze starts. No effect with composite `present` or `engine`, or with `freezeWorld` `false` |
| `msaa` | `4` | 0.31.0: MSAA samples for the page targets Ultralight asks to antialias on the GPU path. `1` turns it off, `2`, `4` or `8`; a count the graphics card cannot do steps down, any other number rounds down to one of these. On Ultralight 2.0 (1.5.0) pages draw their shapes analytically and no page target measured has asked for it, so it costs nothing unless one does; the memory line says (see "Memory"). A target that takes it holds that many more copies of itself in video memory (about 15 MB each at 2560x1440) |
| `memoryLogMinutes` | `10` | 1.5.0: minutes between the periodic memory lines (0-1440; `0` = none). The lines after UI mode and on request stay. See "Memory" |
| `ulFiberMB` | `8` | 1.3.0: the stack pages run on, in MB (8-64; since 1.5.0 a smaller number is raised to 8, with a line in the log, because on Ultralight 2.0 a page's first uncaught error overflows a page stack under about 6 MB): a fiber on the game's main thread, so a page that recurses or nests deeply, or leaves an error uncaught, cannot run out the game's own stack. `0` runs pages on the game's stack, as Magelight 1.2 did; since 1.5.0 the engine's stack reserve is then set past the main thread's stack guarantee. See "A page error ends the UI or the game" |
| `fontHinting`, `fontGamma` | `"normal"`, unset | Text rendering: `smooth`, `normal`, `monochrome` or `none`; gamma 1.0-3.0. Since 1.5.0 `normal` and an unset gamma keep Ultralight's font profile's (gamma 2.2; 1.8 before 1.5.0) |
| `animationTimerDelay` | none | Ignored since 1.5.0, with a warning in the log: Ultralight 2.0 has no animation timer, and pages animate at most 60 times a second (see "Performance") |
| `cursorFile`, `cursorHeight` | `""`, `24` | The cursor is drawn in code (it glows over a clickable element and becomes an I-beam over text); `cursorFile` replaces it with your own image (relative to the runtime folder, or absolute, placed by `cursorHotspotX/Y`). `cursorHeight` is its height in pixels at 1080p (8-256), scaled with the resolution: 24 is 48 px at 4K |
| `cursorHotspotX`, `cursorHotspotY` | `0`, `0` | The pointer pixel of a `cursorFile` image, 0-1 across and down |
| `cursorForce`, `modCursors` | `false`, `true` | 0.31.0: mods may bring their own cursor. `cursorForce` `true` shows yours (your `cursorFile`, else the drawn cursor) everywhere, over every mod's cursor and over pages that hide the pointer. `modCursors` `false` ignores mods' cursor images and colours (0.31.1), but a page that draws its own pointer still hides yours. See "The cursor looks different over one mod, or disappears" |
| `loadStagger`, `loadBudgetMs` | `true`, `8` | 0.31.0: views not yet opened start loading their pages one per frame after a load, the next once the previous page is ready or failed (or 100 ms passed) and the last frame's page work took under `loadBudgetMs` (1-100), at most about a second per batch; a view being opened, or visible at creation, loads at once. `false` loads every page in the same frame, as before 0.31.0. See "Performance" |
| `skipGameCursor` | `true` | 1.2.0: while a page holds UI mode, the game's own cursor is not drawn (its Cursor Menu's draw is skipped), so the game's Cursor Menu cannot draw over Magelight's cursor (a mod drawing a cursor of its own still can). Flat only. `false` installs nothing |
| `clipCursor` | `true` | 1.5.1: while a page holds the mouse and the game is in front, Windows' cursor is kept inside the game window, and let go on exit and on focus loss (a clip that was there before is put back). Flat only. `false` never clips |
| `blockPolledKeys` | `true` | 0.31.10: while a page holds UI mode, other SKSE plugins that ask Windows for the key state themselves (`GetAsyncKeyState` / `GetKeyState`) read every keyboard key as up, so typing into a page cannot fire their hotkeys. The plugin whose page it is is exempt. `false` installs no filter |
| `stallWatchdog`, `stallThresholdMs` | `true`, `1500` | Log the present and main threads' stacks when no frame is presented for this long (250-60000 ms); the first time per session, every other thread's too. On flat, not while another program's window is in the foreground (alt-tab): the gap counts from the game's return, unless the game's window stops responding |
| `exceptionTelemetry` | `true` | The `Magelight VEH` lines (see "Magelight VEH" below). `false` takes Magelight's exception handler out of the game entirely, from the main menu on |
| `vr` | see below | Skyrim VR only |

The `vr` block: `enabled` (`true`), `mirror` (`true`: keep drawing pages on the desktop mirror),
`panelFollow` (`false`: a panel stays where it opened; `true` glides it back when you look away,
tuned by `followAngleDeg` 45, `followMinDeg` 80 and `followDistM` 0.5), `panelDistanceM` (1.2),
`panelWidthM` (1.6), `panelHeightOffsetM` (-0.1), `panelWidth` / `panelHeight` (1600 / 900, the
page size of fullscreen views), `keyboard` (`true`: Magelight's own laser keyboard for text
fields; since 0.31.9 in the colours of the mod whose page you type into when it gives some: `view N hostTheme` or `view N keyboard theme` in the log; since 1.5.0 laid out for the field's `inputmode`, with edit buttons in its bar), `runtimeKeyboard` (`false`), `beam` (`true`) and `beamAlpha` (0.55) for the laser,
`cursorScale` (0.012) and `cursorDot` (`true`) for its pointer, `suppressRuntimeLaser` (`true`:
hide OpenComposite's own menu laser over Magelight pages), `aimUseTip` (`true`) and
`aimPitchDeg` (-35) for the controller's aim.

## The UI stopped working partway through a session

`Magelight: SEH exception ... in frame work — overlay disabled, UI mode will drop` means something
crashed inside a frame, in Ultralight or in Magelight's own frame code. Magelight caught the crash so
the game keeps running, and turned its overlay off for the rest of the session: restart the game to
bring the UI back (loading a save does not). Post the whole log with the report. The
`Magelight VEH: exception ... (module <dll> +0x...)` line just after it (written a moment later by another
thread, with its `#NN` caller lines under it) names the module and offset; a place that faulted earlier in the session is named by its first
line, and new places stop being logged after 64: `MgWCore.dll` is Ultralight's web engine (see the VEH section below). `MgWCore.dll +0xFB7F77` before 0.31.3
was a PC where Arial could not be loaded. Since 0.31.3 the `fonts:` lines near the top of the log
name the last-resort font the session uses and, when Arial could not be used, why. An exception
`0xC00000FD` is a stack overflow: see the next section.

## A page error ends the UI or the game (stack overflow, 0xC00000FD)

`SEH exception 0xC00000FD ... in frame work — overlay disabled` in `Magelight.log`, or a crash whose
CrashLogger log names `EXCEPTION_STACK_OVERFLOW` with `MgWCore.dll+0x173F003` at the top (Magelight
1.5.0, Ultralight 2.0; `MgWCore.dll+0x18CB970` on Magelight 1.3.x and earlier, which crash logs name
`JSC::VM::throwException+0x160`, the nearest name the DLL exports, not the code that failed).

**Why.** Every page runs its scripts on a stack, and the web engine stops a runaway script some way
short of that stack's end. CrashLogger SSE AE VR 1.23.0 and later holds back the last 64 KB of the
game's main-thread stack for its own crash handler (a stack guarantee; the log line `the game's frame
leaves ... KB of its stack at Present (...; 64 KB of it held back by a stack guarantee)` says so). The
engine does not count that: when a page leaves an error uncaught (in a timer, a click handler, a
`requestAnimationFrame` callback), or a script reaches the recursion limit, the engine first claims
stack inside the held-back part, and Windows raises 0xC00000FD: Magelight turns its overlay off when it
can catch that, and otherwise the game crashes. Pages on the game's own stack meet this; pages on
Magelight's page stack do not.

**Magelight 1.5.0.** Pages run on a stack of their own by default, an 8 MB fiber on the game's main
thread (`pages run on a stack of their own (8 MB, a fiber on the game's main thread)` in the log, then
`WebKit's VM is up ...; the main thread holds its lock whenever it is off the page stack` once the first
page loads), where an uncaught error reaches the page's `error` event and a runaway script ends in
`RangeError: Maximum call stack size exceeded`, as in a browser. `ulFiberMB` below 8 is raised to 8
(`ulFiberMB 4 is below 8, ...` in the log). With `"ulFiberMB": 0` in
`Magelight.json`, pages run on the game's stack, and Magelight sizes the engine's stack reserve past the
guarantee the main thread holds when the first save loads (`JavaScript stack zones for pages on the game's stack, past a
64 KB stack guarantee ...` in the log): those errors are survived there too, but a page that lays itself
out from the bottom of a deep recursion, or one nested hundreds of elements deep, can still run the game's
stack out, and a script gets at most 384 KB of stack there (about 5 MB on the page stack), so a deep
recursion, or one call that spreads a very large array into arguments (`fn.apply(null, bigArray)`; 32768
numbers fit), ends in `RangeError` sooner. A guarantee another plugin sets after the first save loads is
not covered: the log then says
`the game's main thread now holds back ... KB of its stack (a stack guarantee), more than ...`. Magelight
1.2 and earlier always ran pages on the game's stack; 1.3.x runs them on the page stack unless
`"ulFiberMB": 0`.

**If it still happens on 1.5.0**, send: the whole `Magelight.log` (it holds the main thread's id, the
stack lines above, which view was open and the exception lines), the CrashLogger log if the game
crashed, your `Magelight.json` if you have one, your CrashLogger version, and what you clicked or opened
just before. If you can, install `MgStackTest` (`tools/stack-test` in Magelight's source: F7, then
**Run all tests**) and say how far it got. A crash in a thread other than the main thread at
`MgWCore.dll+0x2B34EA`, or an illegal instruction at `MgWCore.dll+0x1866F30`, is the page stack without
its lock (the 1.5.0 test builds before this fix crashed so at load with `"ulFiberMB"` set): report it,
with the log, and set `"ulFiberMB": 0` until a fix.

## "Magelight VEH" and "Magelight stall" lines

`Magelight VEH: exception ...` records each serious exception raised on any thread of the
game, once per place it happens (up to 64 places a session), including ones another mod raises and
handles itself: most of them are exactly that and are not crashes (the first such line says so).
The `Magelight VEH:   #01 <module>+0x...` lines under it are the calls that led there, most recent
first: they tell a fault Magelight caused from another mod's or the game's own (a stack overflow
gets the one line only). Magelight never handles these exceptions, so the mod that raised one, the
game and CrashLogger see each as if Magelight were not there, and its faults while writing a crash
report are left out. The handler only notes the fault and returns; another thread writes the lines
a moment later, so this handler can no longer hang a crash before CrashLogger writes its report. A
game that dies at once (no CrashLogger) can end before the line is written: the crash report is the
record. `"exceptionTelemetry": false` in `Magelight.json` turns the handler off. `Magelight stall: ...`
records the stacks of the present and main threads when no frame was presented for longer than
`stallThresholdMs` (a long save or a hitch can do that). On flat it does not sample while
another program's window is in the foreground, since an alt-tabbed game stops presenting; when the game
presented nothing for longer than the threshold while away, one `none sampled` line gives that time. A
game window that stops responding is sampled even then. The first sample of a session also
records every other game thread (up to 64, labelled `other`), which names the thread a hang is
waiting on: post the whole block with a hang report. They are diagnostics written only to the log, and on their
own they do not mean Magelight crashed. Nothing is sent anywhere.

## A key typed into a page triggered another mod

On flat, while UI mode is on, Magelight mutes the engine's keyboard device: its DirectInput poll reads and discards the buffered keys instead of turning them into button events, so no keyboard button event exists anywhere in the engine — not for SKSE input sinks, not for mods that hook the input dispatcher (Immersive Equipment Displays' Backspace UI key was the field case), not for Papyrus `OnKeyDown` — and nothing replays into the game when the page closes. The page keeps typing through the window's own key messages, and so do Escape and Magelight's bound hotkeys. It also raises the engine's text-entry flag for the mods that check it. Mouse and gamepad are untouched. On VR the device is not muted (the page is typed from the engine's key events); Magelight takes each key out of the event chain before the input sinks registered after its own. A mod that asks Windows for the key state itself (`GetAsyncKeyState` or `GetKeyState`, which is how some plugins read their hotkeys) reads every keyboard key as up while a page holds UI mode, since 0.31.10; the log names each such plugin once (`keys <dll> polls are hidden while a page holds UI mode`), and `"blockPolledKeys": false` turns this off. Exempt: the plugin whose page holds UI mode, the mouse buttons, and Magelight's own runtime folder (`SKSE\Plugins\Magelight\`), whose web engine reads Shift, Ctrl and Alt that way for the page's own key events. A mod that still reacts reads the keyboard some other way (`GetKeyboardState`, raw input); report it with the mod name.

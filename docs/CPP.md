# Magelight UI — the C++ tier

The C++ tier is for an SKSE plugin that needs more than a manifest or Papyrus
can do: per-frame data into a page, texture images (render an actor into the
UI), custom JS listeners, owning UI mode, VR controller bindings. If a static
HUD or a hotkey-toggled config page is all you need, use the
[manifest tier](MANIFEST.md) — no DLL required.

There are two headers, both in `api/`, both header-only (no import library —
the host is reached by `GetModuleHandle`/`GetProcAddress`):

- **`MagelightUI_API.h`** — the raw C ABI. One struct of function pointers per
  API version; you acquire it and call the vtable. This is the whole contract.
- **`MagelightUI_Mod.h`** — an *optional* convenience wrapper over it. Owns
  `RegisterMod`/`UnregisterMod`, pre-fills the `size` fields, and turns the one
  `onEvent` callback into per-view `onClose`/`onError` slots. Use it to skip
  ~50 lines of boilerplate; ignore it if you want zero abstraction.

Vendor both headers into your plugin (copy them in, or add `api/` to your
include path). They depend only on the standard library and, for the raw
header, `<d3d11.h>` forward decls. Include CommonLib (or your precompiled
header) first: the raw header includes `<windows.h>`, and CommonLibVR stops
with "Windows API detected" when that comes before it.

## Minimum viable plugin

This is a complete SKSE plugin that puts a page on screen and logs when the
user closes it. It uses the wrapper; the raw-header equivalent is in the
comments.

```cpp
#include <SKSE/SKSE.h>               // CommonLib first (see above)
#include "MagelightUI_Mod.h"        // pulls in MagelightUI_API.h
#include <filesystem>

static MAGELIGHT_API::MagelightMod g_mod;
static MAGELIGHT_API::ViewId       g_view = 0;

// Your pages live under Data\Magelight\<modId>\...  — resolve that from the
// running exe, NOT a relative path (the host loads an absolute file:/// URL).
static std::string PagePath()
{
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    auto p = std::filesystem::path(buf).parent_path()
           / L"Data" / L"Magelight" / L"MyMod" / L"views" / L"panel" / L"index.html";
    return p.string();
}

static void OnMessage(SKSE::MessagingInterface::Message* m)
{
    if (m->type != SKSE::MessagingInterface::kDataLoaded) return;

    // acquire: false = Magelight absent or older than 0.16.0 (it logs why).
    if (!g_mod.acquire("MyMod", "My Mod", 0, 16, 0)) {
        SKSE::log::warn("MyMod: Magelight UI 0.16.0+ not present — UI disabled");
        return;
    }
    const std::string page = PagePath();
    g_view = g_mod.createView("panel", page.c_str(),
                              MAGELIGHT_API::Layer::Panel,
                              { .fullscreen = true });     // a self-scaling page
    if (!g_view) {
        SKSE::log::error("MyMod: view refused — {}", g_mod.v4()->GetLastErrorMessage(g_mod.id()));
        return;
    }
    g_mod.onClose(g_view, [](MAGELIGHT_API::ViewId) {
        SKSE::log::info("MyMod: the user closed the panel");
    });

    // Open it with F13 (DirectInput scancode 0x64); a page in the Panel layer
    // takes UI mode (cursor + suspended game controls) when its key fires.
    g_mod.v4()->BindHotkey(g_view, 0x64, MAGELIGHT_API::kHotkeyActionToggleUIMode);
}

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    return true;
}
```

The page (`Data\Magelight\MyMod\views\panel\index.html`) is any HTML. To talk
to the plugin, the page uses `window.magelight.send(name, arg)` and
`window.magelight.on(name, fn)`; on the C++ side you push with
`v4()->InvokeJS(view, "window.foo(...)")` or the typed `EvalJS`, and receive
with `v4()->RegisterJSListenerEx(view, "name", cb, user)`.

## The rules that bite

- **Threads.** Only your `onEvent` (and the wrapper's `onClose`/`onError`)
  and `EvalJS` results arrive on the callback thread you pass to `acquire` —
  default `GameThread`, the only thread where `RE::` engine state is safe.
  JS listeners (`RegisterJSListenerEx`, and v1 `RegisterJSListener`) are called
  inline on the **render thread**, mid-frame, whatever thread you chose: copy
  the argument (it is valid only during the call) and post the work with
  `v4()->PostGameTask(fn, user)` (host 0.30.1+) before you touch anything
  `RE::`, never SKSE's `AddTask`: these callbacks run inside the game's Present,
  where on Skyrim VR an `AddTask` can hang the game (the threading contract in
  `MagelightUI_API.h` says why). Magelight's own functions are safe to call from
  there; every one is callable from any thread. VR button callbacks
  (`BindVRHotkeyCallback`, `SetVRButtonListener`) run on the **present thread**;
  post from them the same way. `fn` is a plain function pointer, so box the copy:

  ```cpp
  void OnClick(MAGELIGHT_API::ViewId, const char* arg, void*)
  {
      auto* copy = new std::string(arg ? arg : "");
      auto run = [](void* user) {
          std::unique_ptr<std::string> text(static_cast<std::string*>(user));
          // RE:: is safe here
      };
      if (v4()->hostVersionNumber >= 3001) {
          v4()->PostGameTask(run, copy);
      } else {   // an older host: post from a thread of your own
          std::thread([run, copy]() { SKSE::GetTaskInterface()->AddTask([run, copy]() { run(copy); }); }).detach();
      }
  }
  ```
- **Versioning.** Anything added after your `minHost` must be gated:
  `if (v4()->hostVersionNumber >= 2100) v4()->BindVRHotkeyCallback(...)`, or
  `if (v4()->QueryCapability("vr")) ...`. The packed number is
  `MAJOR*10000 + MINOR*100 + PATCH`. See [CHANGELOG.md](../CHANGELOG.md) for
  which version added which call.
- **Absolute page paths.** The host loads a `file:///` URL and serves files
  from the `Data\Magelight` tree and from the directory of every page kept
  elsewhere (a page laid out under another UI mod's own folder, e.g.
  `Data\<OtherUI>\views\...`; pages outside `Data\Magelight` load as of
  0.28.1). The file system cannot tell which page asks, so every page reads
  that whole union.
  Resolve the path from `GetModuleFileNameW`, not the working directory.
- **Network.** Since 0.30.0 every mod's pages are file-only by default
  (`NetworkPolicy::FileOnly`): they read their own files (see the previous
  point) and the host's runtime folder, and nothing over the network. Opt in with
  `SetNetworkPolicy(mod, policy)` once after `RegisterMod`:
  `NetworkPolicy::LoopbackOnly` adds `http(s)` and `ws(s)` to this machine
  (`localhost`, `127.0.0.1`), for your own local server or a companion app;
  `NetworkPolicy::Any` reaches any host with any scheme (the internet:
  catalogs, indexes, a user-set endpoint). It applies to all of that mod's
  views and each change is logged. (`FileOnly` as a value is new in 0.30.0;
  an older host reads it as `LoopbackOnly`.) Before 0.30.0 the default was
  `LoopbackOnly`, so a mod that relied on reaching a local server must now
  call `SetNetworkPolicy(mod, NetworkPolicy::LoopbackOnly)`; the call exists
  since 0.28.2 (`hostVersionNumber >= 2802`), and on 0.28.2 to 0.29.x it only
  restates the old default. Views created through the v1-v3 `CreateView`
  belong to no mod: they are file-only and cannot opt in. A page that needs
  `LoopbackOnly` or `Any` must be created with `CreateViewEx` under a
  registered mod.
- **Register early.** Call `RegisterMod` at `kDataLoaded` at the latest, before
  any script runs. A script that registers your slug first, or touches the
  manifest mod you ship under it, owns that mod for the session, and your
  `RegisterMod` then returns `InvalidMod`.
- **One UI-mode owner.** Only one view holds UI mode (cursor + input) at a
  time; `RequestUIMode` returns `Busy` (or queues with `kUIModeFlagQueue`) if
  someone else holds it. A HUD/click-through view never takes UI mode.
- **Polled keys read up under another mod's page.** From 0.31.10, while a page
  holds UI mode, `GetAsyncKeyState` and `GetKeyState` called from an SKSE plugin
  read every keyboard key as up (mouse buttons and toggle bits pass). Your DLL is
  exempt while your own view holds UI mode, provided your `RegisterMod`
  callbacks (`onEvent`, else `onLog`) live in that DLL.

## Trust model: views are addressable, not owned

The C ABI carries no caller identity. Any plugin that knows a view id can
call `ShowView`, `Navigate`, `DestroyView`, `EvalJS` and the
listener-registration functions on it — including another mod's ids. That
is deliberate: the Skyrim modding ecosystem is cooperative and cross-mod UI
coordination is a feature (see `FindView`). But it is **not** isolation.
What the host does isolate per mod: storage (per-mod sessions), network
(file-only unless the mod opts in with `SetNetworkPolicy`; reads stay inside
the page roots, but not inside the page's own mod folder), hotkeys (first registrant wins), and
page calls (since 0.30.0 a page's `window.magelight.send` reaches only the
listeners of the view it is in; it cannot call another view's). What it does
not isolate between plugins: view control and JS execution inside an
already-created view. The etiquette follows: drive only your own views, and
coordinate with other mods through the channels they document — never by
reaching into their pages.

Storage jars are shared by name. The default `sessionName` (isolated) gives
your mod a jar named after its `modId`, which only your mod uses. A custom
`sessionName` is shared with every mod that names it and with any mod whose
`modId` is that name; `"default"` is the jar all v1-v3 views use. Scripts
cannot drive a mod whose jar a plugin uses, but another mod's pages can still
run in it. If your storage must stay out of other mods' reach, keep the
isolated default.

Scripts get less reach (0.30.0). A mod registered through this API
(`RegisterMod`/`RegisterModEx`, including a manifest mod your DLL adopted) is
**plugin-owned**: Papyrus `RegisterMod`, `CreateView`, `ReleaseUIMode` and
`SetNetworkPolicy` refuse its slug, and every Papyrus call that acts on its
views (`ShowView`, `RequestUIMode`, `Call`, `Eval`, `EvalAsync`,
`RegisterListener`, `BindHotkey`, …) does nothing and returns false. Scripts
can still read: `FindView`, `IsVisible`, `IsUIModeActive`, `GetUIModeView`,
`GetLastError`. The same refusals cover a script-tier mod whose storage jar
is the default one or a plugin's session name. If scripts should open your
panel, register a Papyrus native
of your own that does it (`examples/Magelight.InspectTarget`). See
[PAPYRUS.md](PAPYRUS.md#which-mods-a-script-can-drive).

## Adopting a manifest mod from C++

If you also ship a `manifest.json` under the same `modId`, your DLL adopts it:
call `RegisterMod` with that id and you can drive its declared views. Resolve
each view's id from its name with `v4()->FindView(id(), "viewName", &out)`
(0.26.8+). This lets a designer lay out views in the manifest and the DLL fill
them with live data. Adopting makes the mod plugin-owned, so scripts can no
longer drive its views (see the trust model above).

## IME (Chinese, Japanese, Korean input)

Nothing to do. When a text field in your page takes focus while the page holds
UI mode, the host attaches the window's IME context (never for a password
field or an `<input type="number">`, where the player types plain digits with
the IME on, as in a browser); the composition text shows inline in the field, a
field in a same-origin iframe or a `contenteditable` region included, with the
`compositionstart`, `compositionupdate` and `compositionend` events a browser
fires; the committed text replaces it as the composition ends. While a
composition is open, Enter, Space, Escape, the arrow keys and the number keys
belong to the IME and your key handlers do not see them. Escape is the IME's
whole: the first one closes the candidate list, the next cancels the reading
(text already committed stays), and neither, nor the key-up or auto-repeat of
the one that ended the composition, leaves UI mode or reaches your page as a
key, captured Escape or not; only an Escape with no composition open does what
Escape does. An IME that commits on Escape and passes the key on, as the
Korean IME commits a syllable (and as IMEs pass on Enter and Space after a
commit), gives your page that Escape like any other, after the commit is in the
field: one Escape commits the syllable and closes or reaches the page. A click in the field mid-composition keeps the reading: it goes on
composing where it was, and the IME commits it there; a click in another field
keeps it as typed, and so does UI mode ending or moving to another view, and a
composition the IME ends while the game window has lost the keyboard focus
(Alt+Tab). Not
available in VR, where the VR keyboards own text entry.

The Windows IME windows do not draw over the game, so the host shows none and
the page core draws the IME's candidate list in a box at the composition: below
the reading's first character (the clause the IME converts next, once it has
converted clauses), above it when there is no room below, kept in the window.
The row the IME selected is highlighted: the one Space or Enter acts on (none
for the Japanese IME's predictions before converting, where Space converts to
its own choice). Since 1.5.0; `QueryCapability("imeCandidates")`
(`window.__MAGELIGHT__.capabilities.imeCandidates`). The box follows your theme
through CSS custom properties: `--ml-ime-bg`, `--ml-ime-fg`, `--ml-ime-border`,
`--ml-ime-font`, `--ml-ime-selected-bg`, `--ml-ime-selected-fg` and
`--ml-ime-max-width` (34em by default, never past 90% of the window; a longer
candidate is cut with an ellipsis). Without `--ml-ime-font` the box takes the
focused field's font family at the size the field's text is drawn at (in an
iframe or under another zoom too), with a line height of 1.4, so the list
matches the text being typed; with no field it is 14px sans-serif.
`--ml-ime-font` (a `font` shorthand) replaces all of that. The box's padding is
in em, so it follows the font. Each colour the page leaves unset comes from the
first of: the page's matching `--ml-tooltip-bg`, `--ml-tooltip-fg` or
`--ml-tooltip-border`; the focused field (its text colour; what is painted under
its centre: the background colours and gradients of every element there, in
front of it or behind it, `pointer-events: none` layers such as a backdrop or a
vignette included, a linear gradient read at that point and a radial or conic
one averaged, composited front to back down to an opaque ground, through an
iframe into its page, over black where the game shows through; its border or a
mix of the two; `url()` images and `backdrop-filter` are not read); the
defaults (`#1e1e20` ground, `#f0f0f0` text, `#5c5c60`
border). Unless the page sets `--ml-ime-bg` or `--ml-ime-fg`, text and ground
under 3:1 apart fall back to the defaults. The selected row, without
`--ml-ime-selected-*`, is the ground mixed toward the text with the text on it
(the text and ground swapped when that would read under 3:1), and the default
grey over the default colours. Colours are read each time the list changes. The box lives in `<body>`, so a UI scale applied as a `zoom` on
`<body>` scales it with the page, and it is placed in the page's own
coordinates at any zoom or device scale. A field in a same-origin iframe (an
editor written into `about:blank`, such as CodeMirror) gets the box in the top
page, at its composition.

Every change first fires a cancelable `magelight:ime` window event whose detail
is the whole state, before the page core draws the box for it:

```json
{ "state": "candidates", "text": "",
  "composition": "東京", "caret": 2,
  "candidates": ["東京", "とうきょう", "トウキョウ", "東京都"], "selectedIndex": 0,
  "clauses": [0, 2], "target": 0,
  "anchor": { "x": 222, "y": 104, "h": 24 } }
```

`state` is `start`, `compose`, `commit`, `candidates` (a list opened, changed
or closed) or `end` (empty: hide what you draw); `text` is the committed string
on `commit` and the composition on `compose` (empty otherwise), as before 1.5.0;
`caret` counts UTF-16 units into `composition`; `candidates` is the page the IME
shows and `selectedIndex` is within it (`-1` for none); `clauses` are the clause
boundaries in UTF-16 units (`0` first, the composition's length last; `[]` when
the IME reports none) and `target` is the index of the clause the IME converts
next (`-1`: none, as while the reading is unconverted); `anchor` is where the
IME's window would open, the top-left and height of the character it anchors
on (else of the caret), in the view's CSS px (`getBoundingClientRect`'s at no
zoom, for a field in an iframe too), or `null`. The composition is the field's
own: when the event fires the field already holds it. A page that draws its own
candidate list sets `data-ml-ime="off"` on the field, an ancestor or an iframe
on the way to it (`<html>` for the whole page), and the page core then draws no
box. Cancelling the event (`preventDefault()`) does the same for that one
change only, so a page that opts out that way cancels every event.
`QueryCapability("ime")` says the host attaches the IME to text fields at all
(flat only: 0 in VR since 1.5.0).

## Escape, loopback, and the other 0.28.0 hooks

- **Escape.** By default Escape leaves UI mode (never while an IME composition
  is open: that Escape is the IME's, see "IME"). When a modal or an editing
  control should handle it instead, call `SetEscapeCapture(view, true)` (or
  from the page: `magelight.send('__escapecapture','1')`); Escape then
  arrives as an ordinary keydown/keyup and UI mode stays. Clear it when the
  control closes. Feature-detect with `QueryCapability("escapecapture")`.
  The host's toggle key (PageUp unless `Magelight.json` sets `toggleKey`)
  always leaves UI mode, captured or not.
- **Local servers.** Once the mod has called
  `SetNetworkPolicy(mod, NetworkPolicy::LoopbackOnly)` (required since
  0.30.0, see Network above), a page may `fetch()`
  `http://127.0.0.1:<port>` (or `localhost`), or open a WebSocket there —
  your own server on this machine. Anything else is refused.
  `QueryCapability("loopback")`.
- **Inspector, z-order, scroll step.** `ShowInspector`, `SetViewOrder`,
  `SetScrollStep` — see the header (`QueryCapability("inspector")`,
  `"vieworder"`, `"scrollstep"`). Page console output reaches you as the
  `ConsoleMessage` host event; there is no separate console callback. The
  event carries every message in full; `Magelight.log` records only what the
  player's `consoleLog` allows (0.31.0: warnings and errors by default, cut at
  2 KB, 20 lines a second per view). `QueryCapability("consolelog")` answers
  that level (0 none, 1 errors, 2 warnings, 3 all); forward your own lines
  from the event when you need them and it is below 3. Keep secrets out of a
  page's console output either way. Since 1.5.0 (`QueryCapability("scripterrors")`)
  an `Error` argument arrives as its message, the JSON of its own fields when
  it has any (`Error: x {"code":"E_FOO"}`), and its stack (it was that JSON
  alone, `{}` for an Error with no fields of its own), and an exception your
  `InvokeJS` script or an `InteropCall` handler leaves uncaught arrives as a
  level-2 `ConsoleMessage` too (`y` = the line it was made at, 0 when that is
  not known, the detail its message and stack) and is logged as
  `[view N InvokeJS/ERROR]` or `[view N InteropCall('fn')/ERROR]`; `EvalJS`
  hands its exception to your callback instead. Your script runs unnamed,
  which lets a relative `import()` in it resolve against the page, and its
  stack frames carry no location. What decides the line is who is on the stack
  when the error is made, not who owns the code. With no frame that names a
  script, `y` and the log line (`line 3:31 of an unnamed script`) give the line
  of the unnamed code that made it: your script's, or the line of a page's eval
  or `new Function` code your script called directly. When a page file is on
  the stack, `y` is the line in the page file that made the error, or 0 when
  unnamed code it reached made it: eval or `new Function` code, or a function
  of yours that the page's code called (a callback you passed it), whose frame
  is a bare `@` above the page's; the first frame with a file is then where the
  page reached it, and nothing gives the line in your function. An `Error`
  subclass is made at its constructor's `super(...)` call, so one with no
  constructor of its own, yours or the page's, made from your script reports
  JSC's built-in constructor, `line 1:28`, and made from page code reports no
  line. A value thrown with no stack (a string, a plain object, a
  `DOMException` from a DOM call) has `y` 0 and no frames: only its text.

## UI sounds (0.29.0)

Ultralight has no media stack — `<audio>` and Web Audio do nothing — so the
host plays through the game's own audio, which also respects the player's UI
volume. Three ways in, all the same names:

- **Markup, no code.** `data-ml-sound="click"` on any element plays on click
  (keyboard activation too); `data-ml-sound-hover="focus"` plays on entering
  the element. The injected bridge wires both up; hover is throttled host-side
  (one per ~60 ms per view). Page-driven sounds play only while the view is
  visible.
- **Page script.** `magelight.sound('cancel')` (SDK: `host.sound`, React:
  `useSound()`).
- **C++ / Papyrus.** `PlayUISound(view, "cancel")` on a refused action, no page
  round-trip; `Magelight.PlaySound(view, name)`. Gate on `hostVersionNumber
  >= 2900` or `QueryCapability("sound")`.

Names: `ok`/`click`, `cancel`, `prevnext`, `focus`/`hover`, `open`, `close`,
`inactive` — the vanilla `UIMenu*` descriptors, resolved by FormID (EditorIDs
are unreliable at runtime on VR); **any vanilla `UI*` / `ITM*` descriptor by its
EditorID** (`UIJournalOpen`, `UISelectOn`, `ITMGoldUpSD`, `ITMBookOpenSD` — all
134, case-insensitive, from a generated FormID table); `none` for explicit
silence; or `Plugin.esp|0xFormID` for any SNDR your mod ships. An unknown name logs once and stays silent. `SetViewSounds(view,
"open", "close")` (manifest `"sounds"`) makes a menu sound like a menu on
UI-mode enter/exit with zero page code; it is off unless you ask. There is
deliberately no file playback: a loose `.wav` path would be a new attack
surface (SECURITY.md) and would bypass the audio settings.

## A quick close and reopen (1.1.0)

`RequestUIMode` and `ReleaseUIMode` only queue the entry or the exit: the host runs it on a later
game-thread task, and a busy frame (a page's first parse, a save's load) can hold it for hundreds of
milliseconds. Two orders catch a mod that closes and reopens quickly:

- **A close before the entry ran.** `ReleaseUIMode` withdraws nothing that has not entered: it returns
  `Ok` and the entry still runs, so the mod gets a `UIModeEntered` it no longer wants and has to give the
  mode back. Call `CancelUIModeRequest(mod)` instead, before any `ReleaseUIMode`: `Ok` means the entry
  (and any `kUIModeFlagQueue` request of yours) was withdrawn, and no `UIModeEntered` or `UIModeRefused`
  follows for it. `Denied` means it already ran: its event is on its way, and you release once it is
  entered, as before.
- **A reopen before the exit ran.** A `RequestUIMode` from the mod that still holds the mode retargets it
  in place and returns `Ok`, and then the release's queued exit takes the mode away (`UIModeExited`).
  Add `kUIModeFlagReclaim` to the reopen: your release that has not run is withdrawn and the mode is kept,
  retargeted to the new view and pause, with no `UIModeExited` for that release; a `UIModeEntered`
  "switched" confirms it, the same view included. A view you hid with the release needs `ShowView(view, true)`.

Both are opt-in: a mod that never calls `CancelUIModeRequest` or passes the flag sees the earlier
behaviour, events included. Gate on `hostVersionNumber >= 10100` and `QueryCapability("uimodecancel") == 1`;
an older host ignores the flag.

## Device scale and the pause retarget

- **Real DPI (0.26.9).** `ViewDesc::uiScale` is Ultralight's device scale:
  the page's `devicePixelRatio`, rasterised at that scale (0 = the host
  default). `SetViewScale(view, scale)` changes it on a live view and keeps
  the page (no reload): the render thread applies it at the start of its
  next frame, in one Ultralight call with any size a `SetViewBounds` left
  pending, the page lays out again in that frame and is painted at the new
  scale. Its `resize`, `matchMedia` `change` and `ResizeObserver` callbacks
  run once, in that frame's paint (a hidden view's in the paint of its next
  show or prepaint, whichever comes first: a `SetViewPrepaint` view is
  prepainted about 1.5 s after the change, still hidden, never in VR), never
  inside the call. A `SetViewScale` with the scale the view already has, like
  a `SetViewBounds` that ends at the size it has, makes no call from 1.5.0
  (on 2.0 such a call lays the page out again; see below). Before 0.28.5 the
  host made the change on the caller's
  thread, where it crashed the game for a consumer three times out of three;
  it has not run in game since, which is why `RebuildViewAtScale` below
  exists. Offline, on real mod pages changed live from
  1.0 to 1.25, 1.35 or 1.5 and back, on Magelight's GPU driver and the CPU
  renderer, a live change draws like a page loaded at that scale, text
  included, on Ultralight 1.4 (hosts before 1.5.0) and 2.0 (1.5.0) alike
  wherever the view's width and height divided by the scale are whole
  numbers (1.4 leaves a few small spots, 2.0 a few edge pixels a few levels
  of anti-aliasing apart). Where they are not, as at most real UI scales,
  1.4 still matches, but 2.0 lays the two out a pixel apart. On 2.0 a View
  that is resized or rescaled after it was created gets that CSS size
  rounded up, and a View created at that size and scale gets it rounded down
  (`innerWidth` 1423 against 1422 for 1920 px at 1.35, 854 against 853 for
  1280 px at 1.5; 1.4 rounds down both ways). That is not only `SetViewScale`:
  a `SetViewBounds` to a new size and a fullscreen view following a new
  resolution or VR panel size give the rounded-up size too (a view loaded at
  1600x900 at 1.35 reads 1185x666, and 1423x800 once resized to 1920x1080), while its first
  load, its wake from hibernation and `RebuildViewAtScale` create the View
  and round down. So what fills the view can sit a pixel apart between two
  opens, and the page created at the scale can leave its last pixel column or
  row undrawn. Until a live change is confirmed in
  game, prefer `RebuildViewAtScale` on a page that has loaded. Clamped 1.0..3.0 since 0.30.3
  (below 1 Ultralight clipped the page to scale squared of the view; shrink a
  page with a CSS transform instead). Cutout and
  image rects are in VIEW pixels = CSS px x scale. `GetViewInfo` reads it
  back. Gate on `hostVersionNumber >= 2609`.
- **Rebuild at a scale (0.31.7).** `RebuildViewAtScale(view, scale)`: the
  host releases the View at its next frame and loads the page again in a new
  View created at that scale (what hibernation does), instead of re-laying
  the live page out. The page reloads, so keep what it must survive in
  `localStorage` and send again on `ViewReloaded` (or your `onDomReady`)
  whatever you set only when you opened it, the cutout included: it is kept
  in view pixels, so re-send it at the new scale. Bounds, visibility, UI
  mode, listeners, cursor, sounds and VR placement stay. A hidden view
  reloads at once unless it hibernates or loads on show (then when shown);
  the scale the View already has is a no-op. Gate on
  `hostVersionNumber >= 3107` (the capability `rebuildscale` is 1 on every
  such host), e.g.
  `if (v4()->hostVersionNumber >= 3107) v4()->RebuildViewAtScale(view, 1.5f);`
  reloads the page at 150 %. It still reloads on 1.5.0, `ViewReloaded`
  included; `SetViewScale` is the call that keeps the page.
- **Pause retarget (0.26.11).** Re-requesting UI mode on the view that
  already holds it re-targets `kUIModeFlagPause` in place: the live focus
  menu's pause flag and the engine's pause counter move on the game thread,
  nothing is hidden or re-shown, so your close detection never fires. Gate
  on `hostVersionNumber >= 2611`.

## Freezing the world behind a fullscreen page (0.31.0)

`SetViewFreezeWorld(view, true)` asks the game to skip its 3D world render
while `view` holds UI mode with `kUIModeFlagPause`: the engine shows a frozen
frame instead (expected from the Journal's freeze flag; the flag alone is
untested in game as of 0.31.0, and the saving is still to be measured).
It is a per-view preference, callable from any thread and kept until you
change it or destroy the view; the host applies it on the game thread.

- **Pause is required.** A freeze over a running game hangs it, so the host
  sets the freeze only while its own menu holds the pause, and drops it
  before the pause whenever the view is unpaused in place, another view takes
  UI mode, UI mode closes or a load starts. If another mod lets the game run
  under it, the host drops it within a fraction of a second.
- **Opaque pages only.** The HUD is expected to keep drawing under the page,
  and anything translucent shows the frozen frame, not the live world.
- **Flat only.** Untested on VR, so excluded as a precaution (the frozen frame
  may never be drawn in the headset): `QueryCapability("freezeworld")` answers 0 and the call returns
  `Unsupported`. The player can turn it off for every mod with
  `"freezeWorld": false` in `Magelight.json`; then it answers 0 too.
- **Never set menu flags yourself.** `kFreezeFrameBackground` and
  `kTopmostRenderedMenu` on `MagelightFocus` (the PrismaUI recipe) would not
  outlive a pause retarget safely, and `kTopmostRenderedMenu` stops the UI
  pass that draws your page behind an upscaler or a swapchain wrapper.

```cpp
if (v4()->hostVersionNumber >= 3100 && v4()->QueryCapability("freezeworld") == 1)
    v4()->SetViewFreezeWorld(dashboard, true);   // once; takes effect while it holds UI mode paused
```

## When pages load (0.31.0)

A view's page loads some time after `CreateViewEx`, never inside it; DOM ready
(your `onDomReady`, the `ViewDomReady` event) follows the load. Since 0.31.0
the host spreads first loads over frames instead of starting every registered
view in the frame the world becomes ready (each page's parse and first script
run otherwise land in the same few frames after a save loads):

- A view loads at once when it is visible at creation (`startVisible`) or is
  shown (`ShowView(true)` or UI-mode entry) before its page has loaded, so an
  open never waits behind other views.
- The others wait in a queue in the order they were created. One starts per
  frame, and the next only once the previous page reached DOM ready or failed
  (or 100 ms passed) and the frame's Ultralight update ran under the player's
  `loadBudgetMs` (default 8 ms). A batch lasts at most about a second;
  whatever is still waiting then starts together. The log line
  `staggered load - N views over F frames in T ms` sums each batch.
- Calls made before DOM ready (`RegisterJSListenerEx`, `InteropCall`,
  `InvokeJS`, `EvalJS`) stay queued on the view and are delivered after it, as
  for any view created before the world loaded. A consumer that waits for DOM
  ready before it shows a view (or refuses to show it until then) waits for
  the queue as well, up to about a second after a load plus its page's own
  load. Show the view instead: showing it loads it at once.
- `QueryCapability("loadstagger")` is 1 while staggering is on; the player
  turns it off with `"loadStagger": false` in `Magelight.json` (every waiting
  view then loads in one frame, as before 0.31.0).

`SetViewLoadOnShow(view, true)` goes further for a panel that is rarely
opened: its page is not loaded at all until the view is first shown. Call it
right after `CreateViewEx`, before the world loads (manifest views take the
`loadOnShow` key); it decides only a load that has not started, and a loaded
page stays loaded. Such a view's DOM ready comes after its first show, so
never wait for it before showing the view.

`ReloadView` and `Navigate` load the page again: calls made from then on wait
for the new page's DOM ready, and `ViewReloaded` follows it. From 1.5.0 a
`Navigate` to the page's own URL with only its fragment added or changed
(`MyMod/index.html#settings` while `index.html` or `index.html#home` is shown)
keeps the page, as a link to `#settings` would: the page scrolls and gets a
`hashchange`, there is no DOM ready and no `ViewReloaded`, and calls keep
flowing. The page's URL with the fragment it already has, or with none, loads
the page again. On an older host such a `Navigate` leaves every later call to
the view queued, so change the fragment from the page itself there
(`location.hash`).

A `ReloadView` or `Navigate` whose page does not load (its file is missing)
keeps the page shown, as a failed link would: `ViewLoadFailed` reports the
failure, there is no `ViewReloaded`, and from 1.5.0 calls flow to the page
again and a focused text field keeps its VR keyboard and IME (an older host keeps
every later call queued until a load succeeds). A view that wakes from
hibernation or is rebuilt (`RebuildViewAtScale`) loads the URL its last
`Navigate` loaded, query and fragment included, and `GetViewInfo`'s `htmlPath`
names that page's file (no query or fragment, no percent-encoding). On an
older host, a view navigated with a fragment, or to an absolute path with a
space or `#` in it, wakes as the page-missing card.

```cpp
if (v4()->hostVersionNumber >= 3100 && v4()->QueryCapability("loadonshow") == 1)
    v4()->SetViewLoadOnShow(settingsPanel, true);   // right after CreateViewEx
```

## A painted first open (0.31.11)

Magelight draws only visible views, so a hidden page's first show paints the
whole page, decodes its images and may start Ultralight's worker threads, all
in the frame the player opens it. `SetViewPrepaint(view, true)` paints it
while it is still hidden, about 1.5 s after its page reaches DOM ready (again
after a reload or resize), so the first show composites a finished texture.
One view is painted per frame, never on a frame already over the load budget.

```cpp
if (v4()->hostVersionNumber >= 3111 && v4()->QueryCapability("prepaint") == 1)
    v4()->SetViewPrepaint(mainView, true);   // any time; the paint waits for the page
```

- The texture stays allocated while the view lives. For a 2560x1440 view that
  is about 74 MB with MSAA 4x on hosts before 1.5.0 (Ultralight 1.4), and on
  1.5.0 (Ultralight 2.0) about 15 MB for the view's own target plus the tiles
  of the layers the page composites. Opt in for the view the player opens
  often, not every popup.
- It paints what the page shows while hidden. A page that hides its own
  content then (`display: none`, an empty shell until the first open) gets
  nothing from it: keep the page laid out and pause its polling instead.
- The paint is still one long frame. It moves from the open to a moment
  about 1.5 s after the page loads, during play. Never in VR, where that frame
  would be dropped in the headset: there the option is accepted and does
  nothing.
- From 1.5.0 the view is painted once more, at most once per load, when its
  page settles: Ultralight's `OnPageSettled`, once network and layout have
  stayed quiet for a moment
  (about 120-150 ms after the first paint for a page that is already
  complete). Ultralight 2.0 decodes a page's images only once the page is
  painted, so the first paint draws none of them, and without the second one
  their decode and paint landed on the first open (measured offline on
  Magelight's GPU driver: 29 ms for a page with 5 images, 86 ms for one with
  19, against 0.3 ms with it). The second paint is not gated on a change; one
  of a page that did not change costs well under a millisecond. A page that
  keeps changing its layout from a timer, or keeps making requests, never
  settles while hidden and keeps only the first paint (CSS animations and
  `requestAnimationFrame` are paused while it is hidden, so they do not keep
  it from settling). So does a page whose URL's fragment changes after its
  first paint (`location.hash` set from a `visibilitychange` handler, a timer
  or a host call while hidden): it settles again only once it is painted
  again, at its first open. Settling waits for no timer, request or host
  call, so content the page fills in after it is painted at the first open,
  as before. `Magelight.log` notes each settle: `view N page settled`. A settle
  that comes while another thread holds Magelight's view registry (an API
  call at that moment; rare) loses its paint for that load, and the log says
  `a page settled while the view registry was busy` instead.
- A page that fills its content only when it opens (data it asks for on open)
  is painted as that empty shell, so it saves the layout and the worker
  start-up but not the paint of its content.
- A page can opt in itself: `magelight.send('__prepaint', '1')`. A manifest
  view takes `"prepaint": true`.
- From 1.5.0 the page sees each paint: its view is shown for the one frame a
  paint takes, usually twice a load (after DOM ready and when the page
  settles, about 120-150 ms apart), so one round of `requestAnimationFrame`
  callbacks runs in each such frame, and so do the `resize`, `scroll`,
  `matchMedia` and observer callbacks held while it was hidden (what they
  change, and work the page deferred to a `requestAnimationFrame` while
  hidden, is in the texture), and in the frame after each it gets two
  `visibilitychange` events that both read `'hidden'`. Code that treats its
  first such callback as a one-time signal runs twice.
  Inside such a frame's callbacks `document.visibilityState` reads
  `'visible'`: the page is painted as shown. So a `'visible'` read in a
  `requestAnimationFrame`, `resize`, `scroll` or observer callback does not
  mean the player opened the page (do not mark notifications read or send an
  "opened" from there); read the state in a `visibilitychange` handler, or
  outside those callbacks, rather than counting events ("Hidden pages").

## Hidden pages (1.5.0)

From 1.5.0 (Ultralight 2.0) a hidden view's page is paused the way a browser
pauses a background tab:

- `document.visibilityState` is `'hidden'` and `document.hidden` is true while
  the view is hidden, from the page's first script when it loads hidden.
  `visibilitychange` fires on each hide and show, within a frame of the call;
  a page that loads hidden gets none until its first show.
- `requestAnimationFrame` callbacks, CSS animations and transitions stop.
  `animationend` and `transitionend` wait for the next show, and a CSS
  animation's time jumps to the wall clock then. A page that finishes its close
  (unmounting, resetting) on one of those events now does that at its next
  open.
- The rest of what a browser runs when it paints waits too: the window's
  `resize` and `scroll` events, `matchMedia` `change` events, and
  `ResizeObserver` and `IntersectionObserver` callbacks. A hidden view that
  the host resizes (a new resolution, a VR panel size) or whose page scrolls or
  changes size gets them in the frame it is next shown or prepainted, before
  that frame is painted, coalesced: one `resize` and one `scroll` however many
  changes there were (the positions between are lost), and an observer
  callback only for a target whose state differs from the last one reported.
  What a script reads itself (`innerWidth`, a new `matchMedia(q)`'s `matches`,
  `getBoundingClientRect()`, `scrollY`) is current while hidden, but a
  `MediaQueryList` the page keeps (a `useMediaQuery` hook's) keeps its last
  reported `matches` until its `change` event, in the render of that first
  frame, so a page that re-renders while hidden from a kept list renders the
  old branch. A handler that changes the page at once is in that first frame;
  one that defers its work (a debounce timer) lands that much later, so a page
  that fits its layout to the window shows its old layout for that long after
  it opens.
- Timers, messages and calls run at full rate: `setTimeout`, `setInterval`,
  `MessageChannel` (React's scheduler), promises, `fetch`, `InteropCall`,
  `InvokeJS`, `EvalJS` and `magelight.send`. What a page changes while hidden,
  directly or in a `requestAnimationFrame` callback it asked for while hidden,
  is on screen in the first frame it is shown. A callback that asks for
  another, or hands its update to a scheduler (React state set outside an
  event), lands a frame or two later, as in a browser.
- A hidden page is never painted, so it costs no render time; its timers still
  cost script time, so pause polling on `visibilitychange`. A hidden view keeps
  its texture: hibernation (`SetViewHibernate`) is still what frees it, its
  video memory within about 20 s, whether or not another view is shown.
- `ShowView` changes `IsViewVisible` at once; the View follows on the render
  thread at two points in each frame, before the input and again between
  `Renderer::Update` and the paint. A show from the game thread (a game task,
  an event or an `EvalJS` callback a `GameThread` mod receives, which is the
  default) runs between frames and is painted in the next one; a show from
  another thread, in the first frame whose second point comes after it. A show
  made on the render thread during a frame is painted in that frame when it
  comes before the paint:
  - the input: a page's click, key or focus handler, and the VR keyboard a
    click or key focuses;
  - a call the host delivers before `Update`: an `InteropCall` or `InvokeJS`
    handler, and for a `RenderThread` mod its `EvalJS` callback and the event
    it gets from them;
  - `Update`: the view's `onDomReady` callback (and its `ViewDomReady` event
    for a `RenderThread` mod), timers, `MessageChannel` messages, promise
    callbacks;
  - a JS listener the page calls from any of these.

  It is painted a frame later when it comes from page code that runs inside
  the paint: a `requestAnimationFrame` callback, the window's `resize` and
  `scroll` events, `matchMedia` `change` events, `ResizeObserver` and
  `IntersectionObserver` callbacks, `transitionend` and `animationend` (and a
  JS listener they call, or the VR keyboard a focus made there raises). A hide
  made during a frame takes effect in it, so two views handed over (hide one,
  show the other) from one of these paint callbacks, a `transitionend` at the
  end of a close animation included, leave a frame with neither drawn: hand
  over from one of the calls in the list above (an input event such as a
  click or a key, a timer, a message, an `InteropCall`) instead.

```cpp
if (v4()->QueryCapability("pagevisibility") == 1) {
    // hidden pages pause: the page can stop its own work on visibilitychange
}
```

A shown page animates and repaints at most 60 times a second (the cap of
Ultralight 2.0's Free edition), whatever the game's frame rate:
`requestAnimationFrame` callbacks, CSS animations and transitions, smooth
scrolling and the changes its script makes wait for the next such frame, while
input, a resize and a show paint at once. Before 1.5.0 they followed the frame
rate. `Magelight.json` `animationTimerDelay` is ignored.

## Keys and the mouse (1.5.0)

From 1.5.0 (Ultralight 2.0) a page's input matches a browser's more closely
([SDK.md](SDK.md) lists what a page sees):

- Key events are built from the key's Windows message, so `KeyboardEvent.code`,
  `location` and a `key` that follows the player's keyboard layout are filled
  on flat and in VR (VR builds the message from the engine keyboard's
  scancode). `keyCode` is as before. `Magelight_DeliverVKey` keys get the
  scancode the key has on the player's layout. `metaKey` is always false.
  As in a browser, `code` names the key's place on a US keyboard (the key
  labelled A on a French AZERTY keyboard reads `KeyQ`): a page that hands
  your plugin a key to poll with `GetAsyncKeyState` must send its `keyCode`,
  which follows the layout, not a code derived from `code` or `key`.
- Alt, F10 and Alt+key reach the UI-mode page as system keys; the game still
  gets them too (Alt+F4 still closes it).
- Mouse buttons 4 and 5 reach the page as back and forward, with `auxclick`
  and never `click` or `dblclick`. A middle click no longer starts autoscroll
  (and two give no `dblclick`).
- Moves and releases follow the press: while a button is held they go to the
  view it was pressed on, wherever the pointer is, so a drag released over
  another view or the game ends there. In VR each hand is its own pointer, with
  its own press and hover. When the pointer moves to another view
  with no button held, the view it left gets a move outside itself
  (`mouseout`, `mouseleave`, `:hover` cleared). The wheel always goes to the
  view under the pointer and scrolls the element there. Leaving UI mode, or
  the game losing focus, releases what the page saw go down: each held mouse
  button and each held modifier key.

```cpp
if (v4()->QueryCapability("keyboardcode") == 1) {
    // KeyboardEvent.code / key / location are the platform's
}
if (v4()->QueryCapability("auxbuttons") == 1) {
    // mouse 4/5 arrive as back/forward + auxclick; middle clicks never autoscroll
}
```

## Your own cursor (0.31.0)

The flat cursor is the host's by default: a drawn arrow that glows over a
clickable element and turns into an I-beam over text. `SetViewCursor` gives a
view its own images, one per page state, chosen from the page's CSS cursor:

| State | Shown over | Without an image |
|---|---|---|
| `kCursorArrow` | everything not below, `cursor: url(...)` included | the host arrow |
| `kCursorPointer` | `cursor: pointer`, `grab`, `grabbing` | the view's arrow image, else the host arrow |
| `kCursorText` | text (`cursor: text`) | the host I-beam (the caret hint matters more than the look) |

```cpp
if (v4()->hostVersionNumber >= 3100 && v4()->QueryCapability("cursor") == 1) {
    MAGELIGHT_API::CursorDesc d{ sizeof(d) };
    d.state = MAGELIGHT_API::kCursorArrow;
    d.imagePath = "views/cursor/arrow.png";   // relative to the mod folder, or absolute
    d.hotspotX = 3; d.hotspotY = 1;           // image pixels, like CSS cursor: url(x) 3 1
    d.height = 28;                            // px at 1080p, scaled with the resolution; 0 = the image's own
    d.pressShrink = true;                     // shrink about the hotspot on a click, like the host arrow
    v4()->SetViewCursor(view, &d);
    d.state = MAGELIGHT_API::kCursorPointer;
    d.imagePath = "views/cursor/hand.png";
    v4()->SetViewCursor(view, &d);
}
// later: v4()->SetViewCursor(view, nullptr) clears every state
```

- One call per state, kept until changed or the view is destroyed; an empty
  `imagePath` clears that state, `nullptr` clears them all. Any thread.
- Images are PNG or DDS (anything Windows' image decoder reads), at most
  256x256. A relative path resolves against the mod folder (`Data/Magelight/
  <Mod>/` for a page there, else the page's own folder) and may not contain
  `..`; a plugin may pass an absolute path. A missing file is
  `InvalidArgument`. The image decodes on a worker thread the first time it is
  drawn, never inside a frame, and is shared by every view that names the
  same file with the same hotspot, height and press; until it is ready, and
  if it fails (one log line), the host cursor shows.
- `imagePath = "none"` (state `kCursorArrow`) says the page draws its own
  pointer: the host draws nothing over the view. A page can say the same with
  CSS `cursor: none`, on any element. Either only takes effect while the
  pointer is inside that view, and every page's cursor is forgotten when UI
  mode closes, so a page cannot leave the player without a pointer.
- A view with no cursor of its own uses its mod's default (the manifest's
  top-level `"cursor"`), then the host cursor (the player's `cursorFile`, else
  the drawn art).
- The player can override all of it: `Magelight.json` `"cursorForce": true`
  shows the host cursor everywhere, even over a page that hides the pointer;
  `"modCursors": false` ignores mod images (a page that hides the pointer
  still hides it). Design for the host cursor too.
- VR keeps its laser dot. With `vr.cursorDot` false the laser end shows the
  view's image for the page state (text falls back to the arrow image; the
  size comes from the panel, `vr.cursorScale`); `"none"` and `cursor: none`
  are ignored there.
- The hover glow belongs to the drawn art and does not carry over: give the
  pointer state its own image if you want hover feedback.

The manifest equivalent is the `cursor` key ([MANIFEST.md](MANIFEST.md)), and
Papyrus has `SetCursor` / `ClearCursor` ([PAPYRUS.md](PAPYRUS.md)).

## Tinting the host cursor (0.31.1)

To match a page's colours without image files, recolour the host's own drawn
cursor over the view. It keeps its shape, hover glow, press-shrink, I-beam and
the player's `cursorHeight`, and stays sharp at every resolution:

```cpp
if (v4()->hostVersionNumber >= 3101 && v4()->QueryCapability("cursortint") == 1) {
    MAGELIGHT_API::CursorTint t{ sizeof(t) };
    t.lit   = 0xFFDCE6F0;   // 0xAARRGGBB; alpha 0 keeps the host's colour, any other is drawn opaque
    t.shade = 0xFF6E8296;
    t.ink   = 0xFF0B1118;
    t.glow  = 0xFF7CB2CE;   // the hover glow over a clickable element
    t.ibeam = 0xFFDCE6F0;   // the I-beam over text
    v4()->SetViewCursorTint(view, &t);
}
// later: v4()->SetViewCursorTint(view, nullptr) gives the host's colours back
```

- Kept until changed or the view is destroyed. Any thread; the cursor is
  rebuilt on the render thread the next time it is drawn over the view. Set it
  when the colours change, not every frame: each new tint builds its textures,
  and the host keeps four.
- The view's own images (`SetViewCursor`, the manifest's `"cursor"`) win over
  the tint. On flat the player's `cursorFile` replaces the drawn cursor, and
  `"cursorForce": true` or `"modCursors": false` drop the tint, as they drop
  images.
- VR: the laser dot takes `lit` for its core and `ink` for its rim, at the
  panel's next redraw (`"cursorForce": true` and `"modCursors": false` drop it
  there too). With `vr.cursorDot` false the laser end shows the view's image,
  the player's `cursorFile` or the baked arrow, none of them tinted.
- No manifest key or Papyrus call. Since 0.31.9 a page can set the same slot
  itself: `magelight.hostTheme({ v: 1, cursor: { lit: '#dce6f0', ... } })`
  ([SDK.md](SDK.md)); whichever of you sets it last wins.

## Theming the VR keyboard (0.31.9)

On VR, a text field that takes focus in the UI-mode view raises Magelight's
own keyboard (`views/keyboard`, `vr.keyboard` in `Magelight.json`), laid out
for the field's `inputmode` and `enterkeyhint` ([SDK.md](SDK.md), "Text
fields"). It is one page shared by every mod, so it shows the theme of the view
that holds UI mode whenever it is raised, and the host's look for a view that
set none:

```cpp
if (v4()->hostVersionNumber >= 3109 && v4()->QueryCapability("keyboardtheme") == 1) {
    MAGELIGHT_API::KeyboardTheme k{ sizeof(k) };
    k.panel       = 0xFF101820;   // 0xAARRGGBB; alpha 0 = not given, any other alpha is drawn opaque
    k.panelBorder = 0xFF3A4E62;
    k.key         = 0xFF1A2633;
    k.keyBorder   = 0xFF2C3D4F;
    k.keyHover    = 0xFF233344;
    k.text        = 0xFFDCE6F0;
    k.muted       = 0xFF7F93A8;
    k.accent      = 0xFF7CB2CE;
    // action, danger, pressed, pressedText, barHover left at 0: the host fills them in
    v4()->SetViewKeyboardTheme(view, &k);
}
// later: v4()->SetViewKeyboardTheme(view, nullptr) gives the host's look back
```

| Token | Where | Required |
|---|---|---|
| `panel`, `panelBorder` | the keyboard's background and border (the border colour also draws the drag grip and a hovered key's or bar button's border) | yes |
| `key`, `keyBorder`, `keyHover` | a key or a bar button, its border, one under the laser | yes |
| `text`, `muted`, `accent` | key labels; the title bar's hints; the title, Shift and Caps, a pressed key's or bar button's border | yes |
| `action` | Bksp, Tab, Enter, Esc and the arrows, and the bar's Undo, Redo, Select all, Cut, Copy and Paste (1.5.0) | no: `accent` if it has 3:1 on `key` and `keyHover`, else `text` |
| `danger` | Close | no: `#c0392b` or `#e74c3c`, whichever reads better on both `key` and `keyHover`, if it has 3:1 on both; else `text` |
| `pressed` | a key or a bar button held down, Shift or Caps on, the bar while dragged | no: `accent` 20% over `key` |
| `pressedText` | the label of any key or bar button held down, and of Shift or Caps while on | no: `text`, or black or white (the one with more contrast) when `text` has under 4.5:1 on `pressed` |
| `barHover` | the title bar under the laser | no: `panel` and `key` half and half |

Contrast is WCAG 2's ratio. Labels are held to 3:1 on both `key` and
`keyHover`, because in VR the laser hovers every key before it presses it:

- **Refused** (`InvalidArgument`, the reason in `GetLastErrorMessage`, nothing
  stored): a required colour with alpha 0, or a `text` with under 3:1 on
  `key`, `keyHover` or `panel`. `size` must be at least the 0.31.9 struct's;
  the host reads only the fields it knows, so a later, longer struct still
  works.
- **Replaced by the fill-in** (the theme is still stored, with a warning in the
  log and through your `onLog`): a given `action` or `danger` with under 3:1
  on `key` or `keyHover`, a given `pressedText` with under 4.5:1 on `pressed`.
- **Kept, with a warning**: an `accent` with under 3:1 on `key`. Shift, Caps
  and the title are drawn in it, so pick one that reads; the `action` fill-in
  stops copying it.
- So the fill-in makes a light theme work as well as a dark one: give the
  eight required colours and every label the host fills in can be read.
- Without a theme, the keyboard is drawn exactly as before 0.31.9, including
  a held special key, which keeps its own colour there.
- Per view: kept until you change it or destroy the view; hibernation and
  `RebuildViewAtScale` keep it. It shows only while your view holds UI mode
  with the keyboard up. From 1.5.0 the keyboard goes down when another view
  takes UI mode (including your own other views), and shows that view's theme
  the next time a field there raises it. Any thread.
- The font is the keyboard's own; there is no font token.
- Flat has no on-screen keyboard (the player types on a real one): the theme
  is stored there and never shown, so set it unconditionally.
- A page can set the same slot itself with `magelight.hostTheme({ v: 1,
  keyboard: { ... } })` ([SDK.md](SDK.md)), with the same rules, and the
  cursor tint in the same message. Whichever of you sets it last wins. From
  1.1.0 a part with a bad value is refused alone: the other part of the
  message still applies, and a page that asks gets a reply saying which part
  was refused and why.
- The keyboard page is privileged (its keys go to the UI-mode view), so it
  never sees what you or a page passed: the host checks the colours, builds a
  fresh message from the numbers, and the page checks each value again.
- The keyboard page is kept current while it is down, and after a hide it
  stays blank until the message of its next show arrives, so a show never
  paints another view's colours or a theme that has since changed (at worst
  it is blank for one frame).

## When something doesn't work

Every failure is logged to `My Games\Skyrim Special Edition\SKSE\Magelight.log`
(`Skyrim VR` on VR, `Skyrim Special Edition GOG` on GOG) with your mod id and
the view name. `GetLastErrorMessage(mod)` returns the last
per-mod failure string. Page console warnings and errors go to the same log
(every console line with `"consoleLog": "all"` or `"devMode": true` in
`Magelight.json`, 0.31.0); devMode also adds hot reload and an on-page banner
for script errors. See [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

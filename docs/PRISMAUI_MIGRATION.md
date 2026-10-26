# Moving a mod from PrismaUI to Magelight UI

For mod authors with an existing PrismaUI dependency. End users don't need this document —
and, because the two hosts coexist, they never have to choose (see
[Running both](#running-both)).

A note on framing before anything technical: PrismaUI proved that web-stack UI belongs in
Skyrim modding and built the market this project serves. Magelight exists because
SeverActions needed things PrismaUI doesn't do — GPU rendering, lifecycle control, VR on
SteamVR itself — not because the model was wrong. If your mod works on PrismaUI today and needs nothing listed
below, staying is a legitimate choice; this page is for the day it isn't.

## Why mods move (and why some shouldn't)

**You might move for:** GPU rendering through a D3D11 driver on the game's own device, view
lifecycle beyond create/destroy (`ReloadView`, `Navigate`, hibernation of hidden views), view
bounds, anchors and named layers (PrismaUI views are fullscreen; it has a stacking order but no
bounds), a Papyrus API, hot reload in devMode, per-mod storage isolation, VR on SteamVR directly
as well as through OpenComposite (PrismaUI's VR build runs through OpenComposite Unleashed only),
and a `minHost` version gate that tells users what to install instead of rendering a blank page.
From 1.5.0 (Ultralight 2.0) also: hidden pages that pause instead of running unseen (PrismaUI
updates and paints every view each frame, shown or not), the IME composed in the field with no
page code (PrismaUI hands the page the IME's state to draw), and UI sounds through the game's
audio, which neither engine can play itself.

**You might stay for:** PrismaUI is installed by every one of your current users and its
CPU path is fine for a light page; your page does nothing PrismaUI can't do; you don't want
to touch a working integration. One design difference cuts the other way: PrismaUI runs
Ultralight on a thread of its own, so a page's work stays off the game's frame. Magelight runs
it on the game's main thread, inside the frame. GPU rendering and paused hidden pages keep a
typical page cheap there, but a page that runs heavy script every frame costs the game that
time. A dedicated thread for Magelight's page work is planned, which would take that cost off the
game's frame as well. (Project-state facts — Nexus build version, game-version
compatibility — change over time; check them fresh rather than trusting
any summary, including this one.)

## Running both

Magelight ships its Ultralight runtime under private file names precisely so it cannot
collide with PrismaUI's copy in the same process. A user can install both hosts and run
PrismaUI mods and Magelight mods side by side. One caveat: if your C++ polls PrismaUI's
`HasAnyActiveFocus` to avoid overlapping UI, query Magelight's `IsUIModeActive()` too when
both hosts are present — neither host sees the other's focused view.

## What carries over

Both hosts embed Ultralight (WebKit), so **your pages carry over almost unchanged** — this
is measured, not aspirational: SeverActions ported every view (14 views over 9 pages in 4.2)
with the frontend essentially untouched.

- **Page → host:** PrismaUI registers C++ listeners as page globals (`window.castSpell(...)`).
  Magelight's `RegisterJSListener(view, name, cb)` installs the same `window.<name>(arg)`
  global — your page calls it identically. New code can use the typed
  `window.magelight.send(channel, payload)` bridge instead; both work.
- **Host → page:** PrismaUI's `InteropCall(view, fn, arg)` invokes `window.fn(arg)`.
  Same name, same behavior on Magelight. New code can use
  `window.magelight.on(channel, fn)` with pre-mount replay instead of hand-rolled
  ready-handshakes (Magelight buffers sends that arrive before your app mounts).
- **Marshalling:** pass raw JSON as one string arg, no extra escaping — same rule on both.
- **Text input and IME:** Magelight follows the focused field itself, with no script in the
  page (from 1.5.0 an input, a textarea, a `contenteditable` region or a field in an iframe), on
  the page holding UI mode, and uses the Windows IME there, whose own windows it never shows: the
  composition shows inline in the field and the page core draws the candidate list in a box at it
  (`data-ml-ime="off"` opts a page out, from 1.5.0). PrismaUI's IME is the `prismaIME_state` page event, with
  the page drawing its own composition and candidate overlay; on Magelight delete that listener
  and overlay. For an "IME active" indicator, listen for `magelight:ime` ([CPP.md](CPP.md), "IME").
- **UI sounds (Magelight-only gain, 0.29.0):** neither host's pages can play audio (Ultralight has no
  media stack — the recurring "can I add a click sound to PrismaUI buttons?" question). On Magelight
  put `data-ml-sound="click"` on a button, or call `magelight.sound('click')`, and the host plays the
  vanilla menu sound through the game's audio. PrismaUI ignores the attribute, so markup needs no
  branch. A script call does: PrismaUI has no `window.magelight`, so an unguarded
  `magelight.sound(...)` throws there. Guard it (`window.magelight?.sound?.('click')`) or use the
  SDK's `host.sound`, which is a no-op off Magelight.
- **Network:** Magelight pages are file-only by default (0.30.0): they read their own files and
  nothing over the network. If your page fetches a local server, your DLL calls
  `SetNetworkPolicy(mod, NetworkPolicy::LoopbackOnly)` (or the manifest says
  `"network": "loopback"`, or Papyrus calls `SetNetworkPolicy`); for the internet,
  `NetworkPolicy::Any` from C++ ([CPP.md](CPP.md)).
- **localStorage:** persists on both hosts. Note: Magelight's cache is a different folder,
  and sessions are isolated per mod by default (`"default"` opts back into a shared jar: the
  `sessionName` field from C++, the `"session"` key in a manifest). User data stored under PrismaUI does **not** move — if that matters for your
  mod, migrate it through your own save data.
- **Rendering:** from 1.5.0 Magelight runs Ultralight 2.0 and PrismaUI 1.4.1, so a page can look
  slightly different. Text gamma is 2.0's default 2.2 where PrismaUI keeps 1.4's 1.8; text that
  looks lighter or heavier than on PrismaUI is that, and `Magelight.json` `"fontGamma": 1.8` is
  1.4's look. Ultralight's compositor is on, which costs more video memory per page, and 2.0
  decodes a page's images only when it paints them, so a page's first open can be slower unless
  it is prepainted (`SetViewPrepaint`, manifest `prepaint`).
- **Frontend build:** strip `crossorigin` from module tags and set `base: './'` — required
  over `file:///` on Magelight, tolerated-but-pointless on PrismaUI. If you targeted es2019
  for an older engine, es2022 works on Magelight's Ultralight (WebKit 615, in 1.4 and 2.0). Both are what
  `@magelight/vite-plugin` sets for you.

## The C++ mapping

Magelight's header is the same acquisition pattern (`Magelight_RequestApi(n)`, versioned
function-pointer struct, null = host absent). Every PrismaUI
call maps like this:

| PrismaUI | Magelight | Note |
|---|---|---|
| `CreateView(path, onDomReady)` | `CreateViewEx(mod, &desc, &out)` | fullscreen via `ViewDesc::fullscreen`, or give real bounds/anchor |
| `Show` / `Hide` | `ShowView(view, bool)` | the same call; from 1.5.0 a hidden page pauses (difference 6 below) |
| `Focus(view, pauseGame, disableFocusMenu)` | `RequestUIMode(view, flags)` | returns a `Result` — someone else may own UI mode; queue or refuse. There is no focus menu to disable: the host's own `MagelightFocus` menu is managed for you |
| `Unfocus(view)` | `ReleaseUIMode(mod)` | never hides the view — same as Unfocus |
| `IsHidden(view)` | — | no query; track what you showed |
| `IsValid(view)` | `IsViewValid(view)` | |
| `HasFocus(view)` | `GetUIModeOwner()` | answers per mod, not per view |
| `HasAnyActiveFocus()` | `IsUIModeActive()` | per-host — see [Running both](#running-both) |
| `Invoke(view, js, callback)` | `InvokeJS(view, js)` / `EvalJS` | `EvalJS` returns the result like PrismaUI's callback, plus any exception |
| `InteropCall` / `RegisterJSListener` | same names | `RegisterJSListenerEx` adds a `void* user` |
| `kFreezeFrameBackground` / `kTopmostRenderedMenu` on `PrismaUI_FocusMenu` | `SetViewFreezeWorld(view, true)` (0.31.0) | once per view; the host applies it while the view holds UI mode paused and drops it before the pause. Never set flags on `MagelightFocus` yourself |
| FocusMenu close polling | **delete it** | `onEvent` gets `UIModeExited` on *every* exit path — no Escape poller, no orphaned FocusMenu |
| `SetOrder` / `GetOrder` | `SetViewOrder` / `GetViewOrder`, `RaiseView` | order within the view's layer; the layer itself is set at creation |
| `SetScrollingPixelSize` / `GetScrollingPixelSize` | `SetScrollStep(view, px)` | pixels per wheel notch (default 40) |
| `CreateInspectorView` / `SetInspectorVisibility` / `IsInspectorVisible` | `ShowInspector` / `IsInspectorVisible` | no inspector bounds to set |
| `RegisterConsoleCallback` (V2) | the `ConsoleMessage` host event | every console message in full, whatever `Magelight.log` records |
| Destroy (unused by most) | `DestroyView(view)` | async; views can also `ReloadView`/`Navigate` |

Seven behavioral differences to design around:

1. **Threading contract is explicit.** `onEvent` and `EvalJS` results arrive on the callback
   thread you pick at `RegisterMod` (default: game thread — the only thread where `RE::` is
   safe). JS listeners (`RegisterJSListener`/`RegisterJSListenerEx`) do not: they run inline on
   the render thread (PrismaUI's run on its own UI thread: on neither host are they on the game
   thread). Marshal from them with
   `PostGameTask` (0.30.1+), not SKSE's `AddTask`: copy the argument, then post the work before
   touching `RE::`. On Skyrim VR an `AddTask` made inside the frame can hang the game
   ([CPP.md](CPP.md), "The rules that bite").
2. **UI mode is owned, not global.** One view holds UI mode at a time, across all mods;
   `Busy` is a first-class result with the owner's name in the log, and `FocusDenied` is an
   event the owner receives. HUD views never take UI mode.
3. **Failures name themselves.** `GetLastErrorMessage(mod)`, load-failure events with the
   resolved path, page console in `Magelight.log` — instead of a blank view with nothing in
   the log.
4. **A hidden page is ready a little later** (0.31.0). Pages load one per frame after a save
   loads, so a hidden view's DOM ready can come up to about a second later than before; showing
   the view (or giving it UI mode) loads it at once, and calls made before DOM ready are
   delivered after it. Never gate an open on DOM ready: a bridge that refuses to show its view
   until DOM ready waits for the whole queue. `SetViewLoadOnShow` holds a rarely used panel's load until
   its first show ([CPP.md](CPP.md), "When pages load").
5. **The cursor is drawn by the host, not Windows.** PrismaUI showed the Windows pointer for
   every CSS keyword; Magelight draws its own and tells three states apart (arrow, pointer over
   `pointer`/`grab`, I-beam over text), so `not-allowed` and the resize keywords show the arrow.
   Since 0.31.0 a mod can supply its own images per state (`SetViewCursor`, manifest `cursor`)
   and CSS `cursor: none` hides the host's for a page that draws its own ([CPP.md](CPP.md),
   "Your own cursor"). Since 0.31.1 `SetViewCursorTint` recolours the host's drawn cursor to
   match your palette, with no image files ([CPP.md](CPP.md), "Tinting the host cursor").
6. **A hidden page pauses** (1.5.0, Ultralight 2.0). On PrismaUI, and on Magelight before 1.5.0, a
   hidden page is only not drawn: its animations and events run as if it were shown. From 1.5.0
   its page reads `document.visibilityState` `'hidden'` and gets `visibilitychange` on each hide
   and show; its `requestAnimationFrame` callbacks and CSS animations and transitions stop, so
   `transitionend` and `animationend` wait for the next show (a panel that finishes closing on one
   of them does that at its next open, so it can open showing what it held at the close); and its
   `resize` and `scroll` events, `matchMedia` `change` events and `ResizeObserver` /
   `IntersectionObserver` callbacks wait for the next show or prepaint and arrive once (a panel
   that relayouts after a debounced `resize` opens at its old layout until the timer fires: do
   that work in the handler). Timers, messages, `InteropCall` and `InvokeJS` keep running.
   `capabilities.pageVisibility` in the page, or `QueryCapability("pagevisibility")` in C++, says
   the host does this ([CPP.md](CPP.md), "Hidden pages").
7. **Key events carry `code` and the layout's `key`** (1.5.0, Ultralight 2.0). On PrismaUI, and on
   Magelight before 1.5.0, `KeyboardEvent.code` was `""`. From 1.5.0, as in a browser, `code`
   names the key's place on a US keyboard (the key labelled A on a French AZERTY keyboard reads
   `KeyQ`) and `key` is the character the player's layout types (`"ö"`, `"&"` for AZERTY's
   unshifted 1). A page that turns a keydown into a Windows virtual-key
   code, such as a hotkey capture for a plugin that polls `GetAsyncKeyState`, must read `keyCode`,
   which is unchanged: one that prefers `code` binds the wrong key on a non-US keyboard
   ([SDK.md](SDK.md), "Keyboard and mouse"). Also from 1.5.0: Alt, F10 and Alt+key reach the page
   (a system key; Alt+F4 and the like still work), and mouse buttons 4 and 5 arrive as `button`
   3 and 4 with `auxclick`, never `click`, with no middle-click autoscroll.

## Distribution changes

Your FOMOD's dependency line changes from PrismaUI to Magelight; pages move from
`Data/PrismaUI/views/...` to `Data/Magelight/<modId>/views/...` (the folder is your
namespace — see [MANIFEST.md](MANIFEST.md)). Declare `minHost` and the host itself renders
the "needs Magelight a.b.c" panel for users on an older host. Bundle-versus-require and the
licensing your mod inherits are covered in [DISTRIBUTION.md](DISTRIBUTION.md). Both hosts ship
Ultralight's free tier, but not the same build: PrismaUI ships 1.4.1-dev, Magelight 2.0.0-beta.2
from 1.5.0 (1.4.0b before), whose licence is not 1.4's word for word, so read it before you bundle.

If your mod is Papyrus-only, you were never on PrismaUI — start at
[PAPYRUS.md](PAPYRUS.md) instead; there is nothing to unlearn.

## A suggested port order

1. Build your frontend against `@magelight/vite-plugin` (or set `base`/`crossorigin`/es2022
   by hand) and iterate in a plain browser with the SDK's mock host — no game needed.
2. Swap the native calls per the table; delete your Escape poller and FocusMenu plumbing.
3. Smoke-test in the desktop harness (`tools/desktop-harness`), then in game with
   `"devMode": true` for hot reload, the on-page error banner and every page console line in
   `Magelight.log` (without it the log records console warnings and errors only, 0.31.0).
4. Ship to a small test group *with both hosts installed* — coexistence is the thing to
   exercise, and it's the configuration your users will actually have during any transition.

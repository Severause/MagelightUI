# Magelight on Cyberpunk 2077 — design and engine notes

The code is `cyberpunk/` (MagelightCP, a RED4ext plugin for game 2.5); its README says how to build it and what a mod
author writes. This file is why it is built the way it is: the engine points it uses, how a page reaches the screen,
how input and scripts reach a page, and what was checked.

**Status, 2026-11-27:** manifest mods load and draw in game; redscript, CET and C++ mods drive their views and hear
what pages send; hotkeys, the mouse, clicks and typing were tried by hand in game. Not built: gamepad, hiding views
over the game's own menus, HDR conversion, the GPU path (§7). Addresses and hashes are game 2.5's.

---

## 1. What carries over from the Skyrim host

| Part | Where | On Cyberpunk |
|---|---|---|
| Ultralight 2.0.0-beta.3 | `extern/ultralight` | Unchanged |
| Page rules: bridge, hotkey names, cursor art, key events and the context-menu guard | `src/JsBridgeCore.h`, `src/HotkeyNames.h`, `src/MagelightCursorArt.h`, `src/InputCore.h` | Reused unchanged from `../src` |
| The manifest schema | `docs/MANIFEST.md` | The same file; a subset of the keys (§4.1) |
| The page contract (`window.__MAGELIGHT__`, `window.magelight`) | `docs/SDK.md` | The same core script |
| The D3D11 GPU driver | `gpu/` | Not used: the game is D3D12 (§3) |
| The host (`src/Magelight.cpp`): Present hook, input, UI mode | | Rewritten for the engine (§2-§5) |
| Papyrus | `papyrus/`, `docs/PAPYRUS.md` | Replaced by redscript, CET and a C++ export (§6) |
| VR, the Scaleform focus movie | | Dropped |

**What a page cannot do** is the same as on Skyrim: no WebGL, WASM, `<video>` or `<audio>`.

---

## 2. The engine points

Read in a disassembly of the 2.5 executable, 2026-11-27. **R** = read there; **I** = inferred. Hooks go by hash
through RED4ext's address library, never by a raw address; a hash is the Adler-32 of the mangled name.

| What | Address (2.5) / hash | How known |
|---|---|---|
| End of frame | `GpuApi::Present` `0x1405d95b8` / `2468877568`; caller `CRenderNode_Present::Execute` `0x140bf60f0` / `3554614662` | R |
| Backbuffer to PRESENT before it | `GpuApi::TransitionSwapChainPresent` `0x140bed50c` / `2023363837`, from `CRenderPostProcess::PresentCopy` `0x14229e7d0` / `3732677997` | R |
| Window procedure | `ViewportWindowPC::StaticWndProc` `0x1408f124c` / `920064814` → `ProcessMessages` `0x1408f1304` / `2440959140` → `WndProc` `0x1408f16c8` / `530451888` | R |
| Adding a message sink | `ViewportWindowPC::AttachMessageSink` `0x1408effa4` / `1219894350` (appends) | R |
| The game's input sink | `InputSystemWin32Game::MessageProc` `0x1408f1398` / `1696011280` | R |
| Raw input per frame | `CollectInput` `0x1420849a0` / `4103545742` (also calls `ClipCursor`) | R |
| Input filters, cursor | `ink::InputManager::EnableFilter` `0x140916b40`, `DisableAllFilters` `0x140917624`, `game::input::ConsumingBuffer::OnAction` `0x1408c1508`, `InputSystemWin32Base::ShowCursor` `0x14276463c` | Exist (R); not traced |
| Gamepad | `InputDeviceGamepadXInput::Update` `0x1405d5c60` / `3617596257` calls `XInputGetState` each frame | R |
| HDR setup | `ResizeBackbuffer` `0x14068813c` / `239671909`, `SetSwapchainHdrMetadata` `0x140688544` / `11283364`, `CreateSwapChainWithBackBuffer` `0x140688788` / `3077119135` | Exist (R); not decoded |
| A load starts | `gsm::State_SessionLoading::OnProcessInit` / `3416991615` | Hooked, seen in game |
| A load ends | `gsm::State_SessionActive::OnProcessInit` / `2704353053` | Hooked, seen in game |
| A world comes down | `gsm::State_Session::OnProcessShutdown` / `436804462` | Hooked, seen in game |

The RED4ext SDK already gives the D3D12 device and direct queue (`GpuApi::GetDeviceData()`) and the swapchain
(`GpuApi::SSwapChainData`), so nothing hooks `ExecuteCommandLists` to find the queue.

---

## 3. Drawing

### 3.1 Where in the frame

`CRenderNode_Present::Execute`, the render graph's last node, calls `GpuApi::Present`, which takes a mutex, presents
(through a frame-generation object if one is set, otherwise `swapChain->Present`), and signals a fence on the direct
queue. **The plugin hooks `GpuApi::Present`** and submits its own command list on the game's direct queue before
calling the original, so pages land after all of the game's GPU work, its own UI included.

The backbuffer is already in PRESENT state there (`PresentCopy` transitions it before the node runs), so the list
goes PRESENT → RENDER_TARGET, draws through a render-target view of its own, and goes back to PRESENT.

**The game presents from many threads** (nine seen in one session). So Ultralight never runs in the hook: it runs on
a page thread of its own, and the hook only takes the newest image of each page. Draws are serialised by a lock.

### 3.2 The D3D12 side (`src/Compositor.cpp`)

- three command allocators used in turn, each with its own upload buffer; one command list; a fence of our own;
- a texture per view, plus one for the cursor, in a 64-slot shader-resource heap; one pipeline per backbuffer
  format, drawing a quad with premultiplied alpha;
- a slot still in use after 100 ms skips the frame rather than stall the game, and what that frame was handed is
  kept for the next. Each wait has its own event: a shared one, left armed by a wait that timed out, would let a
  later wait return while the GPU still reads what it guards.

### 3.3 Getting a page into a texture

1. **Now: Ultralight's CPU surface.** A page that changed is copied whole through the upload buffer. Enough for menus
   and HUD widgets; too slow for a full-screen animated page at 4K. Copying only the changed rectangles would be
   cheaper: not built.
2. **Later: the SDK's D3D12 driver** (`platform/src/gpu/d3d12/`, `GPUContextD3D12`). It makes its own device and
   queue, so it needs the change Magelight made to the D3D11 driver: take the game's device. A second direct queue on
   that device is allowed, and the game's queue waits on its fence before our draw.
3. Or D3D11On12 on the game's queue, which runs Magelight's D3D11 driver nearly unchanged, at an acquire, a release
   and a flush a frame.

### 3.4 Lifetime

**Ultralight is never destroyed.** Its renderer's destructor waits for its threads under the loader lock at process
exit, and the game never exits (a hang dump showed it). The renderer, views and their globals are leaked on purpose,
and the DLL pins itself. Its DLLs are delay-loaded from the plugin's folder, so nothing that derives from an
Ultralight class may be built at static initialisation. They ship under Magelight's names (`Magelight1.dll`,
`Magelight1Core.dll`, `MgWCore.dll`, invariant 3), patched into every reference at stage time by
`cyberpunk/tools/rename_ultralight.ps1`, so another mod's stock Ultralight in the game is never ours.

### 3.5 Not yet looked at

- **CET draws over us** (I): its ImGui runs inside `swapChain->Present`, after our list.
- **Frame generation.** XeSS frame generation is in the exe; DLSS's (Streamline) sits in front of DXGI. With either
  on, a page drawn here may be treated as game picture and interpolated (I). Needs a run with each.
- **HDR.** Pages are not converted for an HDR backbuffer, and the test snapshot refuses one rather than write garbage.
- **A resize while we draw.** Our work runs before the original Present, outside the engine's present mutex. A
  `ResizeBackbuffer` on another thread at that moment could fail its `ResizeBuffers` while we hold a backbuffer
  reference (I). Not seen; a resolution change in game is the test.

---

## 4. Mods and pages

### 4.1 The manifest

A mod is `<game>/r6/magelight/<ModId>/manifest.json` (`docs/MANIFEST.md`). Supported per view: `path`, `layer`,
`anchor`, `x`/`y`/`w`/`h`, `fullscreen`, `clickThrough`, `startVisible`, `hotkey`. Positions are 1080p pixels,
scaled by screen height / 1080. Other keys are logged as unsupported and ignored. Each mod's pages share one storage
session named after the mod. A page loads only `file://` (inside the mods folder and the plugin's `resources/`),
`data:` and `blob:` URLs. A folder is read by its UTF-8 name, and nothing in the scan may throw: it runs in the
Present hook, where an exception turns every page off.

Containment is by path text (`..` and absolute paths are refused). A real-path check, which would also stop a
junction inside a mod folder from leading out, **cannot be used under MO2**: there a file's real path is in its own
mod's folder, never under the game's, and the check refused Ultralight's own ICU data and the game died at boot.

### 4.2 The page thread (`src/Views.cpp`)

It owns the renderer and every view, ticks them, and hands each changed bitmap over by swapping buffers under a lock
the hook only tries. Every call from another thread is a queued command (show, focus, send, reload). `Show` also
publishes the new visibility at once, so a second press of a HUD key or an `IsVisible` right after sees it.

A page's errors, its uncaught exceptions included, are logged, at most 50 a load; its other console lines are not,
as pages log what a player types. The page core carries Magelight's context-menu guard (invariant 29), armed again
before a right release, the Menu key and F10: without it a right click on selected text in a field crashes the
process (`view_test` showed it, C0000005).

---

## 5. Input (`src/Input.cpp`)

### 5.1 Where it is taken

`StaticWndProc` keeps the `ViewportWindowPC*` in `GWLP_USERDATA`; `ProcessMessages` walks a list of message sinks
and the first that answers true ends the walk. The game's sink, `InputSystemWin32Game::MessageProc`, takes
`WM_INPUT` through `GetRawInputData` and answers true. `AttachMessageSink` appends, so a sink added the normal way
never sees the mouse.

**The plugin hooks `InputSystemWin32Game::MessageProc`.** While a view holds UI mode, raw mouse and keys go to the
pages and the hook answers true without calling the original: the camera does not turn and V does not move. Its
first call decides whether it takes `this` (it does on 2.5).

### 5.2 The rules

- **UI mode:** one view at a time. Its hotkey opens and closes it; Escape closes it. A HUD view (layer `hud`, or
  `clickThrough`) never takes it: its hotkey shows and hides it. While a view holds UI mode, another panel's hotkey
  goes to the page, as in Magelight (invariant 15); a HUD's still acts. UI mode closes when a load starts or the
  session ends, no panel opens while a load runs (its key goes to the game), and none opens again once drawing has
  died (a fault in our Present work, or a failed D3D12 call). The hooks that learn this only set atomics; the
  game-thread tick closes UI mode. Alt+F4 still reaches the game in UI mode. Each pointer source's page gets a leave
  when the pointer goes to another view or UI mode closes, and a press holds its view until every button is up.
- **An up goes where its down went.** A key or button held before UI mode opens is released to the game, and
  Escape's release after it closed UI mode is kept from the game. Without this, V keeps walking after the panel
  closes, or the pause menu opens as it closes. The other way too: as UI mode closes, the page gets the release of
  every key and button it holds, and the real release is kept from the game. When the game loses focus nothing
  stays held on either side, as Windows sends no release for it.
- **The pointer** goes to the topmost shown view under it; a held button sticks to the view it went down on. It is
  Magelight's drawn cursor. Movement is scaled by screen height / 1080 in whole 1080ths, so a slow mouse moves below
  1080p. A tablet, VM or remote desktop sends absolute positions: the system cursor's place is used.
- **Text:** `ToUnicode` on the raw key (Shift, CapsLock, and Ctrl+Alt as AltGr; Ctrl or Alt alone types nothing),
  until a `WM_CHAR` is ever seen; from then on `WM_CHAR`, surrogate pairs joined. A key that types is typing in UI
  mode, even if a view is bound to it. `ToUnicode` is asked not to change the thread's state (it is the game's), so
  a dead key is composed here with the next letter (Unicode NFC; one that does not compose types both, as Windows
  does).
- **Keys:** raw scancodes become DirectInput codes (E0 sets 0x80; Pause is 0xC5). Each key carries the modifier
  sides and locks held when it was queued, and is built on the page thread with `Input::MakeKeyEvent` (invariant
  24): Ultralight's constructor reads them from its own thread, which never sees a key, so Ctrl+A was a plain A.

### 5.3 Not built

- **Gamepad:** the game reads the controller itself, `XInputGetState` in `InputDeviceGamepadXInput::Update` every
  frame. Hooking it, the page gets the state and the game a zeroed one.
- **The engine's own way to take all input** (`ink::InputManager` filters, `ConsumingBuffer`) and how its menus show
  the cursor: not traced. `CollectInput` calls `ClipCursor`.
- **Hiding a HUD view** over the main menu, pause menu, loading screens, photo mode and braindances, and refusing
  UI mode over the game's own menus (inventory, map, pause): only a load refuses it now.

---

## 6. Scripts

### 6.1 What a mod writes

Redscript (needs Codeware for the events):

```swift
public class MyShopUi extends ScriptableService {
    private cb func OnLoad() {
        GameInstance.GetCallbackSystem().RegisterCallback(n"Magelight/MyShop/buy", this, n"OnBuy");
    }
    private cb func OnBuy(evt: ref<MagelightEvent>) {
        // evt.view "panel", evt.channel "buy", evt.payload "{\"item\":\"Items.Pistol\"}"
        Magelight_Send("MyShop", "panel", "balance", "1200");
    }
}
```

CET:

```lua
GetMod('magelight').On('MyShop', 'buy', function(msg) Game.Magelight_Send('MyShop', 'panel', 'balance', '1200') end)
```

The page side is Magelight's: `magelight.send('buy', {...})` and `magelight.on('balance', fn)`.
`cyberpunk/examples/MagelightDemo.reds` is a working one.

### 6.2 The natives (`scripts/reds/Magelight.reds`)

A view is named by mod id (any letter case) and view name. Nothing is kept in the save, as on Skyrim.

| Native | What |
|---|---|
| `Magelight_IsReady() -> Bool` | the page thread made the views (after the game's first frame) |
| `Magelight_HasView(mod, view) -> Bool` | the manifest made it |
| `Magelight_Show(mod, view, show: Bool) -> Bool` | shows or hides; hiding the view that holds UI mode closes UI mode |
| `Magelight_IsVisible(mod, view) -> Bool` | |
| `Magelight_Send(mod, view, channel, text) -> Bool` | `window[channel](text)` if the page defined it, else `magelight.on(channel)`, buffered until a subscriber. False, and a log line, for text that is not valid UTF-8: JavaScriptCore drops such a script without an exception |
| `Magelight_OpenUIMode(mod, view) -> Bool` / `Magelight_CloseUIMode()` | as the hotkey and Escape; false for a HUD view |
| `Magelight_UIModeView() -> String` | `"mod/view"`, `""` when none |
| `Magelight_Reload(mod, view) -> Bool` | loads the page again |
| `Magelight_NextMessage() -> String` | the CET mod's queue (§6.4) |

None waits on the page thread or on Ultralight, so none can stall the game thread.

### 6.3 What scripts hear

One event class, `MagelightEvent extends CallbackSystemEvent` (`mod`, `view`, `channel`, `payload`; an object arrives
as JSON text), named `Magelight/<ModId>/<channel>`. Two channels are the host's: `__ready` (a page loaded: push its
first state) and `__uimode` (`1` / `0`, so a script knows Escape closed its panel).

The plugin calls the reds static `Magelight_Fire` (`scripts/reds/MagelightEvents.reds`), which registers the name with
Codeware and dispatches. Without Codeware only that file fails to compile; the natives are in their own file, and the
CET and C++ listeners still work.

### 6.4 CET and C++

- **CET:** a CET mod `magelight` calls `Game.Magelight_NextMessage()` each frame until it returns `""`. A message is
  `mod\x1fview\x1fchannel\x1fpayload`, so no JSON library is needed. Nothing is queued for CET until it has asked
  once. `On(mod, channel, fn)` takes `'*'` for either; a listener that throws is printed and never stops the next.
- **C++:** `MagelightCP_GetApi(1)` (`cyberpunk/include/MagelightCP_API.h`) returns the same calls plus
  `AddListener(mod, fn, user)`, called on the game thread: the versioned-struct way of `api/MagelightUI_API.h`.
- Every listener kind gets every message.

### 6.5 Threads and game states

- Messages start on the page thread (`magelight.send`), the window's thread (UI mode by key) and the game thread (UI
  mode from a script), into one outbox capped at 4096 (a drop is counted and logged).
- They are delivered only on the game thread, from the plugin's own RED4ext `Running` game state, whose callback reads
  an atomic count first and returns with no lock and no allocation when nothing waits.
- **Never during a load.** Code that runs while a world is loading or coming down may take no lock, allocate nothing
  and log nothing: the script VM and the world are being rebuilt. The three `gsm` hooks (§2) only set atomics. From a
  session end (to the main menu, or into a load from in game) or a load's start, messages wait until a load is over.
  The main menu holds them too: pages load there and their `__ready` arrives once a game is up.

### 6.6 Not built

Views made at runtime without a manifest; Magelight's ownership rules (which mod a script may drive: today any script
drives any mod); sounds, gamepad and the other reserved `__` channels.

---

## 7. Order of work

1. A page on screen — done.
2. Manifest mods, several views, UI mode, the page API — done.
3. The scripting API — done.
4. **Runs with others:** CET's overlay, XeSS and DLSS frame generation, ReShade, HDR on.
5. **Gamepad, the engine's input filters, hiding over the game's menus** (§5.3).
6. **The GPU path** (§3.3).

Open before release: does Ultralight's Free licence cover a Cyberpunk mod the way it covers Magelight?

---

## 8. How it was checked

- **Offline**, `cyberpunk/tools/view_test.cpp`, 64 checks: the real manifest loader, page thread, page API, input
  routing and script API, driven by raw key and mouse calls, against the renamed DLLs. Among them: delivery held
  through a load, then every listener kind; every native including its refusals; an up going where its down went; a
  quick double press of a HUD key; a slow mouse at 720p; Ctrl+A and Shift+Left in a field; a right click on selected text; another
  panel's hotkey while one is open; a page's thrown error in the log and its `console.log` not (each of these four
  failed, or crashed, before its fix); typing after focus was lost with Ctrl held; a Spanish dead acute then "a";
  a page's press whose release comes after UI mode closed; a key held through the close; a load closing UI mode and
  refusing it.
- **Redscript** compiles offline with redscript's `scc.exe` over the game's base scripts and Codeware's, and in the
  game's own compile.
- **In game, 2026-11-27** (2.5, 3840x2160, backbuffer format 28):
  - the hooks attach, the 10 natives register, the tick runs;
  - our work in the Present hook is 0.08-0.13 ms a frame; the page thread 0.1-0.17 ms a tick with a still page,
    0.4-5.4 ms with the demo's animated panel (off the game's frame);
  - the demo's redscript heard the panel's `__ready` and each click, and its answers showed on the panel;
  - from CET every native answered, a CET listener heard `__uimode` and `__ready`, and `Magelight_Send` changed the
    page;
  - the session-end hook fired at the boot load and at a load from in game, and the pages and listeners still worked
    after it;
  - by hand: F11 and Escape (no pause menu), clicks, typing with `@`, F10, a held W released, a slow mouse;
  - the game quits cleanly.
- **Not run:** a character outside the BMP typed; the C++ export from another plugin; leaving for the main menu while
  messages wait; a GPU stall long enough to skip a frame.

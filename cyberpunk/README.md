# cyberpunk/ — MagelightCP, Magelight on Cyberpunk 2077

A RED4ext plugin that loads Magelight manifest mods and draws their Ultralight pages over Cyberpunk 2077 2.5. It is
separate from the Skyrim host: its own CMake project. Design, engine points and what was checked:
[`../docs/CYBERPUNK.md`](../docs/CYBERPUNK.md). From `../src` it reuses, unchanged, `HotkeyNames.h`,
`JsBridgeCore.h`, `MagelightCursorArt.h` and `InputCore.h`. The version is `project(... VERSION)` in
`CMakeLists.txt` alone; changes are in [`CHANGELOG.md`](CHANGELOG.md).

## Mods

A mod is a folder `<game>/r6/magelight/<ModId>/` holding Magelight's `manifest.json` (`../docs/MANIFEST.md`) and its
pages. Supported per view: `path`, `layer`, `anchor`, `x`/`y`/`w`/`h` (1080p pixels, scaled to the screen),
`fullscreen`, `clickThrough`, `startVisible`, `hotkey`. Other Magelight keys are logged as unsupported and ignored.
`examples/MagelightDemo` is the example mod: a HUD (F10 shows and hides it) and a panel (F11), with its redscript
half beside it in `examples/MagelightDemo.reds`. It is here, not in the repo's top-level `examples/`, because
`build.ps1 -Examples` stages every folder there into the Skyrim build.

Pages get `window.__MAGELIGHT__` and `window.magelight` (`send` / `on` / `off` / `pending`), and `'__uimode'` is
dispatched `'1'` / `'0'` as a view gains and loses UI mode. A page loads only `file://` URLs inside the mods folder
and the plugin's `resources/`, and `data:` / `blob:` ones. Each mod's pages share one storage session named after the mod.

## Scripts

A mod's script drives its views and hears its pages:

- **Drive:** the natives in `scripts/reds/Magelight.reds`: `Magelight_Show`, `_Send`, `_OpenUIMode`,
  `_CloseUIMode`, `_Reload` and others. Redscript calls them by name, and CET calls them as `Game.Magelight_*`.
- **Hear, from redscript** (needs Codeware): `GameInstance.GetCallbackSystem().RegisterCallback(n"Magelight/<ModId>/<channel>", this, n"Fn")`.
  The callback gets a `MagelightEvent`, with `mod`, `view`, `channel` and `payload`. `examples/MagelightDemo.reds`
  is a working one.
- **Hear, from CET:** `GetMod('magelight').On('<ModId>', '<channel>', fn)`. `'*'` hears every mod or every channel.
- **Hear, from C++:** `MagelightCP_GetApi(1)` in `include/MagelightCP_API.h`, with `AddListener`.
- **The host's own channels:** `__ready` (a page loaded) and `__uimode` (`'1'` / `'0'`).
- **When messages arrive:** on the game thread, and never while a game is loading: they wait until the load is over.
  The plugin knows a load from the engine's own state machine, three hooks. UI mode closes as a load starts or a
  session ends, and no panel opens during a load.

## What it does

- hooks `GpuApi::Present` (hash `2468877568`) and draws every shown page there, in layer order, then the cursor, with
  its own D3D12 command list on the game's direct queue (`src/Compositor.cpp`): PRESENT -> RENDER_TARGET -> PRESENT;
- runs Ultralight's CPU renderer for every view on a thread of its own (`src/Views.cpp`): the game presents from
  many threads, so the Present hook only takes the newest images. Ultralight is never destroyed (its destructor
  deadlocks the game's exit under the loader lock), and the DLL pins itself;
- hooks `InputSystemWin32Game::MessageProc` (hash `1696011280`, `src/Input.cpp`): a HUD view's hotkey shows or hides
  it; any other view's hotkey opens UI mode on it, where the raw mouse and keys go to the pages and the game never
  sees them. The pointer goes to the topmost view under it; Escape or the hotkey again closes UI mode, and while it
  is open another panel's hotkey goes to the page;
- logs a page's errors, never its other console lines;
- logs to `MagelightCP.log` beside the DLL, including both threads' cost every 600 frames;
- test switch: with a `snapshot.on` file beside the DLL, every 600 draws the part of the backbuffer the pages cover
  is read back after our draw and written to `snapshot.bmp`. Delete the file to turn it off.

No HDR conversion, no gamepad, no game-state hiding, no GPU path yet.

`tools/view_test.cpp` (`mlcp_view_test.exe <stage> <out.bmp>`) runs the host with no game: the demo manifest, the
page thread, the page API, the input routing and the script API (delivery held through a load, every listener
kind), driven by raw key and mouse calls; it prints PASS/FAIL per check
and writes the pages on a 1920x1080 screen to a BMP.

## Build

Needs the Ultralight SDK in `../extern/ultralight` (`extern/README.md`) and a RED4ext.SDK checkout for game 2.5.
`build.ps1` fetches nlohmann/json into `extern/` (gitignored).

```powershell
powershell -ExecutionPolicy Bypass -File cyberpunk\build.ps1 -Red4extSdk <path to RED4ext.SDK>
```

Output in `C:\b\mlcp` (or `-Out` / `MLCP_BUILD_DIR`); `stage\` is laid out as the game root:
`red4ext\plugins\MagelightCP\` holds the DLL, Ultralight's three DLLs under Magelight's names (`Magelight1.dll`,
`Magelight1Core.dll`, `MgWCore.dll`: `tools/rename_ultralight.ps1`) and `resources\`; `r6\magelight\` the demo mod;
`r6\scripts\Magelight\` and `r6\scripts\MagelightDemo\` the redscript; `bin\x64\plugins\cyber_engine_tweaks\mods\magelight\`
the CET door.

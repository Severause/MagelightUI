# MagelightCP — changelog

Versions are `MAJOR.MINOR.PATCH`, from `project(... VERSION)` in `CMakeLists.txt`. `window.__MAGELIGHT__.versionNumber`
is `MAJOR*10000 + MINOR*100 + PATCH`.

## 0.1.0 - Unreleased

The first version: Magelight manifest mods on Cyberpunk 2077 2.5 (`../docs/CYBERPUNK.md`).

- Pages drawn over the game in the Present hook, on D3D12, from Ultralight's CPU renderer on a thread of its own.
- Manifest views: `path`, `layer`, `anchor`, `x`/`y`/`w`/`h`, `fullscreen`, `clickThrough`, `startVisible`, `hotkey`.
- UI mode: a panel's hotkey opens and closes it, Escape closes it; the mouse, keys and typed text go to the page and
  the game never sees them; a key or button held before UI mode is released to the game.
- Keys reach the page with their modifiers (Ctrl+A, Shift+arrows), built with `Input::MakeKeyEvent`.
- WebKit's context menu is guarded as in Magelight: a right click on selected text in a field no longer crashes.
- A page's errors are logged, at most 50 a load; its other console lines and its messages' payloads are not.
- UI mode closes at a load or a session end, is refused during a load, and stays closed once drawing has died; a
  page gets the release of what it holds as UI mode closes (a key held through the close stays from the game to its
  release), and nothing stays held when the game loses focus. Alt+F4 still closes the game in UI mode.
- A page gets a leave when the pointer moves to another view; a press holds its view until every button is up.
- A page channel with a control character is refused; the natives are bound even when the pages cannot start.
- Dead keys compose ("´" + "a" = "á"); a request's query string is dropped, as in Magelight.
- A view reloaded while it holds UI mode keeps it; redscript event names use the manifest's `modId` spelling.
- Scripts: redscript natives `Magelight_*`, Codeware events `Magelight/<ModId>/<channel>`, a CET door, and a C++
  export `MagelightCP_GetApi(1)`; messages wait through a game load.
- Ultralight's DLLs ship under Magelight's names, as in the Skyrim build.
- The DLL carries a Windows file version, the same three numbers, for other plugins to read off the loaded module.
- `IsReady` / `Magelight_IsReady` mean drawing works: false before the views are made and after the host gave up.
- Libraries from vcpkg, as the root's: RED4ext.SDK is the overlay port `ports/red4ext-sdk`, pinned to game 2.5's types.

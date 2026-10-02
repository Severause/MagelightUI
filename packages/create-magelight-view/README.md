# create-magelight-view

```bash
npm create magelight-view MyMod
cd MyMod && npm install && npm run build
```

Produces a manifest mod whose page is a React app on `@magelight/react`. `npm run build` writes the
page to `views/config/` (`index.html` and its `assets/`). To install it, copy `manifest.json` and the
`views/` folder into `Data/Magelight/MyMod/` (or a mod-manager mod holding `Magelight/MyMod/`); the
rest of the project (`src/`, `node_modules/`, the config files) stays out of the game.

F10 opens the page in game. Before you ship, set `hotkey` in `manifest.json` to a key no other mod
and no game control uses (the press still reaches the game): one key opens one mod's view, and the
first mod to bind it keeps it. PageUp is Magelight's own toggle key and is refused.

Drive the mod from Papyrus (`Magelight.RegisterMod("MyMod")`) or from a DLL
(`RegisterMod("MyMod")` adopts it; from then on Papyrus cannot act on its views).

The `@magelight/*` packages are on npm and version with the Magelight host. To scaffold against a
clone of the repository instead, build the packages at the repo root (`npm install && npm run build`)
and run `node packages/create-magelight-view/index.js MyMod --local`.

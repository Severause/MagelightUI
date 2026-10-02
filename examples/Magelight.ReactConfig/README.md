# Magelight.ReactConfig (example)

A zero-DLL config menu: a React page driven by a `manifest.json`, opened with **F8**,
closed with F8 or Escape. Demonstrates the manifest tier end to end — no C++, no ESP,
and the local-state pattern that keeps a page interactive before the host echoes data back.

## What it shows

- `manifest.json` declares the view (`panel` layer, hotkey F8) — the host creates it at data-load.
- The page uses `@magelight/react` hooks: `useChannel` (host → page state with pre-mount
  replay), `useSend` (page → host saves), `useUIMode`, `HostGate`.
- Controls write to **local state** and `send('save', ...)`; nothing in this example echoes
  `state` back, so local state is what makes the slider move (copy this pattern — binding a
  controlled input straight to `useChannel` freezes it until a host answers).
- In a plain browser the SDK's **mock host** mounts a floating panel (bottom right): dispatch
  a `state` payload to the page and watch the `save` sends arrive.

## Build

From the repo root (the example is an npm workspace; the `@magelight/*` packages build first):

```
npm install
npm run build
```

Output lands in `examples/Magelight.ReactConfig/views/` (gitignored). For iteration,
`npm run dev` in this folder serves the page to a plain browser with the mock host.

## Install

`build.ps1 -Examples` stages this mod (plus the other examples) into the install layout;
by hand, copy this folder's `manifest.json` and `views/` to `Data/Magelight/Magelight.ReactConfig/`.
Launch the game and press **F8**. Without the Examples download there is no
`Data/Magelight` folder and the hotkey does nothing.

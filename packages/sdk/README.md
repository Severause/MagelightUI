# @magelight/sdk

Typed page bridge for [Magelight UI](../../README.md), the Ultralight-based web UI host for
Skyrim SE/AE/VR. Framework-agnostic; `@magelight/react` adds hooks on top.

```ts
import { host, bridge, channel } from '@magelight/sdk';

if (host.present) console.log(`running on Magelight ${host.version} as ${host.modId}/${host.viewName}`);

// host → page: whatever the mod sends with InteropCall(view, 'state', json)
const state = channel<{ volume: number }>('state');
state.on((s) => render(s));          // replayed if it arrived before this line

// page → host: reaches the listener the mod registered as 'save'
bridge.send('save', { volume: 70 }); // objects are JSON-encoded, strings pass through

bridge.onUIMode((on) => on ? input.focus() : input.blur());
```

- **`host`** — `present`, `version`, `versionNumber`, `modId`, `viewName`, `viewId`, `dev`,
  `can('textureImage')`, `atLeast(0, 15)`, `imageUrl(name)`, `sound('click')` (host 0.29.0 — through the
  game's audio; a page cannot play sound itself).
- **`bridge.on / send / onRaw / pending / onUIMode`**, **`channel<T>(name)`**, **`ready()`**.
- **Pre-mount replay** — the host buffers any channel payload until someone subscribes, so a
  payload sent from `ViewDomReady` is never lost to a late React mount.
- **Browser mock** — with no host present the SDK installs `window.magelight` itself and a
  floating panel to dispatch payloads / watch sends, so `vite dev` works on the page alone.
  Opt out with `window.__MAGELIGHT_NO_MOCK__ = true` before the SDK loads.

Host side: any channel with no `window.<channel>` shim routes into the page core, so a mod
needs no page-side registration for host → page traffic; page → host still needs
`RegisterJSListenerEx(view, channel, …)` (or Papyrus `RegisterListener`).

# The frontend tier — `@magelight/*`

Three npm packages and a scaffolder, all in `packages/` (npm workspaces; `npm run build` at the
repo root builds them). They are the field-hardened seed of SeverActions' `skse-api.ts` and
`build-ui.ps1`, turned into something any mod can install.

| Package | What |
|---|---|
| `@magelight/sdk` | `host` (presence, version, ids, capabilities, image URLs, `host.sound`, `host.hostTheme`), `bridge.on/send`, `channel<T>()`, pre-mount replay, browser mock with a floating panel |
| `@magelight/react` | `useChannel`, `useChannelEvent`, `useUIMode`, `useCapability`, `useHost`, `useSend`, `useSound`, `<HostGate>` |
| `@magelight/vite-plugin` | file:// safe output (`base './'`, es2022, no `crossorigin`), per-entry pruned `views/<name>/` folders |
| `create-magelight-view` | `npm create magelight-view MyMod` → manifest mod + React config page |

## The page contract (host side)

At window-object-ready, before any page script, the host installs:

```js
window.__MAGELIGHT__ = { version, versionNumber, viewId, modId, viewName, capabilities, dev, runtimeUrl }
window.magelight     = { send(channel, payload), on(channel, fn) → off, off(channel, fn), pending(channel), sound(name),
                         hostTheme(theme) }
```

- **Host → page**: `InteropCall(view, channel, text)` calls `window.<channel>(text)` if the page
  defined one (the classic shim), otherwise routes into `magelight._dispatch(channel, text)`,
  which **buffers until someone subscribes**. Nothing is lost to a late mount.
- **Page → host**: `magelight.send(channel, payload)` reaches the listener the mod registered
  for that channel (`RegisterJSListenerEx` / Papyrus `RegisterListener`). Objects are JSON text
  on the C++ side.
- **`__uimode`**: the host dispatches `'1'`/`'0'` when the view gains/loses UI mode.
- **`__sound`** (host 0.29.0): `magelight.sound(name)` / `send('__sound', name)` plays a UI sound through
  the game's audio (a page cannot itself); `hover:<name>` is throttled. `data-ml-sound="click"` /
  `data-ml-sound-hover="focus"` markup does it with no script. `capabilities.sound` says the host has it.
  The SDK's `host.sound` and React's `useSound()` call `magelight.sound` and do nothing on an older host.
- **`__prepaint`** (host 0.31.11): `send('__prepaint', '1')` asks the host to paint this view once while it
  is hidden, after it loads, so its first show is not one long frame ([CPP.md](CPP.md), "A painted first
  open"); `'0'` turns it off. `capabilities.prepaint` says the host has it.
- **IME candidate list**: Chinese, Japanese and Korean input shows its composition inline in the field
  (its selection on the IME's caret or target clause) and its candidate list in a box the page core draws at
  the caret (the host passes them on the reserved
  `__ime` channel; nothing to subscribe). `data-ml-ime="off"` on a field, an ancestor or `<html>` turns it
  off there, a `magelight:ime` event (cancelable; `detail.state`, `text`, `composition`, `caret`,
  `candidates`, `selectedIndex`, `clauses`, `target`) fires before each change, and `--ml-ime-bg`, `--ml-ime-fg`,
  `--ml-ime-border`, `--ml-ime-font`, `--ml-ime-selected-bg`, `--ml-ime-selected-fg`, `--ml-ime-max-width` restyle
  the box. It scales with a `zoom` on `<body>`, and follows the caret into a same-origin iframe editor.
  `capabilities.imeCandidates` says the host has it; a page that draws its own IME opts out. The focused field also
  gets `compositionstart`/`compositionupdate`/`compositionend`, and password fields get no IME. Flat only
  ([CPP.md](CPP.md), "IME").
- **Tooltips** (host 1.2.0): an element's `title` shows as a tooltip the page core draws (the host
  passes it on the reserved `__tooltip` channel; nothing to subscribe). `data-ml-tooltip="off"` on an
  element or on `<html>` turns it off there, a `magelight:tooltip` event (cancelable; `detail.text`,
  `detail.target`) fires before each one shows, and `--ml-tooltip-bg`, `--ml-tooltip-fg`,
  `--ml-tooltip-border`, `--ml-tooltip-font` restyle it. `capabilities.tooltips` says the host has it; a
  page that draws its own tooltips opts out on those elements.
- **`__hosttheme`** (host 0.31.9): `magelight.hostTheme(theme)` gives the host this view's colours for
  what the host draws itself. `capabilities.keyboardTheme` says the host has it; the SDK's `host.hostTheme`
  calls it and does nothing on an older host. See "Host theme" below.

Plain-HTML pages use the two verbs directly; the SDK types them and adds the mock for browser
development. Neither the SDK nor React is required — the contract is the two globals.

## Host theme (0.31.9)

Two things on screen are the host's, not the page's: Magelight's own VR keyboard (one page shared by every
mod, raised when a text field in the UI-mode view takes focus) and the drawn cursor. A page with its own
theme hands its colours over so both match it:

```js
magelight.hostTheme({
  v: 1,
  keyboard: { panel: '#101820', panelBorder: '#3a4e62', key: '#1a2633', keyBorder: '#2c3d4f',
              keyHover: '#233344', text: '#dce6f0', muted: '#7f93a8', accent: '#7cb2ce' },
  cursor:   { lit: '#dce6f0', shade: '#6e8296', ink: '#0b1118', glow: '#7cb2ce', ibeam: '#dce6f0' },
});
// later: magelight.hostTheme(null) gives the host's look back for both
```

- **Each part**: an object sets it, `null` clears it, a part left out stays as it was. `v` must be `1`.
- **`keyboard`**: `panel`, `panelBorder`, `key`, `keyBorder`, `keyHover`, `text`, `muted`, `accent` are
  required; `action`, `danger`, `pressed`, `pressedText`, `barHover` are optional and the host fills them
  in from the others (the table in [CPP.md](CPP.md) "Theming the VR keyboard"), so a light theme works as
  well as a dark one. A given `action` or `danger` with under 3:1 contrast on `key` or `keyHover`, or a
  given `pressedText` with under 4.5:1 on `pressed`, is replaced by its fill-in; an `accent` with under
  3:1 on `key` is kept (Shift, Caps and the title use it), and both are warnings in `Magelight.log`. It
  shows while this view holds UI mode with the keyboard up; another view in UI mode shows its own. Flat
  has no on-screen keyboard: the theme is kept there and never shown.
- **`cursor`**: `lit`, `shade`, `ink`, `glow`, `ibeam`, all optional (one left out keeps the host's
  colour). The same slot as C++ `SetViewCursorTint`, with its rules: the view's own cursor images win,
  the player's `cursorForce` and `modCursors: false` drop it, and on VR the laser dot takes `lit` and `ink`.
- **Strict, per part**: every colour is exactly `#rrggbb`. A bad value in a part, or a `text` with under 3:1
  contrast on `key`, `keyHover` or `panel`, refuses that part and leaves it as it was; the other part still
  applies (1.1.0; before 1.1.0 it refused the whole message). A message over 2 KB, one that is not a JSON
  object, or a `v` other than `1` refuses everything. A refusal is a warning in `Magelight.log` (a view's
  first 32); the call itself never throws.
- **Reply** (1.1.0, `capabilities.hostThemeReply`): add `reply: true` and the host answers on the reserved
  `__hostthemeresult` channel with `{ keyboard, cursor, why? }`, each part `"set"`, `"cleared"`, `"kept"` or
  `"refused"`, and `why` the first refusal's reason. The payload is JSON text: `JSON.parse` it in
  `magelight.on('__hostthemeresult', fn)`, or use the SDK's `bridge.on<HostThemeResult>('__hostthemeresult', fn)`,
  which parses it.
  Without `reply` nothing is sent, so a page that never listens is sent nothing. A message refused whole
  before it could be read (over 2 KB, not JSON) gets no reply.
- **Scope**: only the calling view's own slots change, and they stay until changed, a reload of the page
  included, or until the view is destroyed. Send it again whenever your theme changes; sending the same
  theme again changes nothing. `Magelight.log` records a view's first change and then at most one a
  second, with a count of the changes between. Each new cursor tint builds the cursor's textures (the host
  keeps four), so change it when your colours change, not every frame.
- **Order with C++**: the plugin's `SetViewKeyboardTheme` / `SetViewCursorTint` and the page write the
  same slots; whichever comes last wins.

## What a page can reach

- **Files**: the `Data/Magelight/` tree, registered page folders and the host's runtime dir, nothing
  else; the host does not keep one mod's page out of another mod's folder.
- **Network**: nothing by default (file-only, since 0.30.0; before that the default also let
  a page reach this machine). A mod opts in for itself: the manifest key `"network": "loopback"`,
  or Papyrus `SetNetworkPolicy(modId, "loopback")` for a mod scripts own, adds http(s) and ws(s)
  to this machine (`localhost`, `127.0.0.1`). Only a DLL can grant internet reach (C++
  `SetNetworkPolicy(mod, Any)`). A page that fetches from a local server needs one of these. The
  policy is a Content-Security-Policy in the page (0.31.5), so a page cannot `fetch()` its own
  files: a plain Vite build sets `build.modulePreload.polyfill = false` (the plugin does) or logs
  one refused fetch per page.
- **Storage**: `localStorage`, IndexedDB and cookies are per mod by default (a mod that names
  a shared session, or `"default"`, shares that jar). They are not per save or per character,
  and they are only as private as the tier that owns the mod. Scripts cannot act on the views
  of a mod a DLL registered or adopted, nor of a mod whose jar a plugin uses. Papyrus cannot
  tell scripts apart, though, so any script can run JavaScript in, and read the storage of, any
  other mod that scripts own (a manifest mod no DLL adopted, or one a script registered). Keep
  secrets out of page storage.
- **Your own view only**: `magelight.send` reaches the listeners of the view the page is in; a
  page cannot call another view's (0.30.0).

## Versions and publishing

The packages are on npm and version with the host they match: 1.0.0 now. At each host
release that changes the contract, bump all four `packages/*/package.json` versions and every
cross-package range with them (`@magelight/react`'s sdk peer and dev ranges, the
`create-magelight-view` template, `examples/Magelight.ReactConfig`). From 1.0 a caret range
takes every later 1.x (`^1.0.0` installs 1.4.0), which the append-only page contract allows;
a new major moves every range. Before 1.0 a caret stopped at the next minor, so the 0.x
packages (`^0.30.0`) never install 1.0.0. Then run `npm install` at the
repo root (not `npm ci`) to refresh the lockfile, and `npm run clean && npm run build`. Each
package carries `prepack`, so its tarball always builds first, and the scoped packages publish
as public through `publishConfig`. Publish in dependency order: sdk, react, vite-plugin,
create-magelight-view.

## Scaffolding from a clone (`--local`)

`npm create magelight-view MyMod` installs the packages from npm. To scaffold against a clone
instead (unpublished package changes), build the workspace at the repo root
(`npm install && npm run build`) and run
`node packages/create-magelight-view/index.js MyMod --local`. The scaffold's `@magelight/*`
dependencies then point at the clone's package folders, and the template's
`resolve.dedupe` keeps Vite on one copy of React.

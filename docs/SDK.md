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
- **`__prepaint`** (host 0.31.11): `send('__prepaint', '1')` asks the host to paint this view while it
  is hidden, after it loads (and, from 1.5.0, once more when the page settles), so its first show is not one
  long frame ([CPP.md](CPP.md), "A painted first open"); `'0'` turns it off. `capabilities.prepaint` says the
  host has it.
- **Page visibility** (host 1.5.0): `document.visibilityState` and `visibilitychange` follow the host's
  `ShowView`: `'hidden'` while the view is hidden (from the first script if it loads hidden), and the
  event fires within a frame of each hide and show. While hidden, `requestAnimationFrame` and CSS
  animations stop, and the window's `resize` and `scroll` events, `matchMedia` `change` events and
  `ResizeObserver` / `IntersectionObserver` callbacks wait: they run once, coalesced, in the first frame
  shown (or prepainted), before it is painted, so a page that fits its layout to the window should do it
  in the handler rather than after a timer. What a script reads itself (`innerWidth`, `scrollY`, sizes, a
  new `matchMedia(q)`) is current while hidden; a kept `MediaQueryList`'s `matches` changes only with its
  `change` event. Timers, `MessageChannel`, promises and `magelight.on` messages keep running,
  and what the page changed while hidden is on screen in its first frame shown. A prepainted view's page is
  painted as shown for one frame per prepaint paint, usually twice a load from 1.5.0 (after it loads and when
  it settles, about 120-150 ms apart): its `requestAnimationFrame` callbacks and the held callbacks above run
  in each such frame and read `document.visibilityState` as `'visible'`, and the frame after each brings two
  `visibilitychange` events that both read `'hidden'`. So take the page as open from the state at load or in
  a `visibilitychange` handler, never from a `'visible'` read in one of those callbacks, and read the state
  rather than count events. `capabilities.pageVisibility` says the host does this ([CPP.md](CPP.md), "Hidden
  pages").
- **Frame rate** (host 1.5.0, Ultralight 2.0 Free): a shown page animates and repaints at most 60 times a second,
  whatever the game's frame rate: `requestAnimationFrame` callbacks, CSS animations and transitions, smooth
  scrolling and the changes your script makes wait for the next of those frames; input, a resize and a show paint at
  once. Before 1.5.0 `requestAnimationFrame` followed the game's frame rate.
- **IME candidate list** (host 1.5.0): Chinese, Japanese and Korean input composes in the focused field itself
  (`compositionstart` / `compositionupdate` / `compositionend`, as in a browser), and the page core draws the IME's
  candidate list in a box at the composition (the host passes it on the reserved `__ime` channel; nothing to
  subscribe). `data-ml-ime="off"` on a field, an ancestor, an iframe on the way to it or `<html>` turns the box off
  there, a `magelight:ime` window event (cancelable; `detail.state`, `text`, `composition`, `caret`, `candidates`,
  `selectedIndex`, `clauses`, `target`, `anchor`) fires before the box is drawn for each change, and `--ml-ime-bg`,
  `--ml-ime-fg`, `--ml-ime-border`, `--ml-ime-font`, `--ml-ime-selected-bg`, `--ml-ime-selected-fg`,
  `--ml-ime-max-width` restyle it (without `--ml-ime-font` it takes the focused field's font family and drawn size,
  line height 1.4; 34em wide at most; a colour the page leaves unset is its `--ml-tooltip-bg` / `-fg` / `-border`,
  else the field's text colour, the ground painted under it (every layer there, gradients and pointer-events:none
  backdrops included, see-through grounds over black) and its border, else the grey defaults, kept 3:1 readable). It scales with a `zoom` on `<body>` and follows the composition into a same-origin
  iframe editor. `capabilities.imeCandidates` says the host has it; password fields get no IME. Flat only
  ([CPP.md](CPP.md), "IME").
- **Tooltips** (host 1.2.0): an element's `title` shows as a tooltip the page core draws (the host
  passes it on the reserved `__tooltip` channel; nothing to subscribe). `data-ml-tooltip="off"` on an
  element or on `<html>` turns it off there, a `magelight:tooltip` event (cancelable; `detail.text`,
  `detail.target`) fires before each one shows, and `--ml-tooltip-bg`, `--ml-tooltip-fg`,
  `--ml-tooltip-border`, `--ml-tooltip-font` restyle it. `capabilities.tooltips` says the host has it; a
  page that draws its own tooltips opts out on those elements.
- **Keyboard and mouse** (host 1.5.0): key events carry `KeyboardEvent.code` (`KeyA`, `Digit1`, `Numpad5`,
  `ArrowLeft`, `ShiftRight`, ...), a `key` that follows the player's keyboard layout (`"@"` for AltGr+Q on a
  Spanish layout, `"ñ"`, `"AltGraph"`, `"Dead"` for a dead key) and `location` (1 left, 2 right, 3 keypad), on
  flat and in VR; `keyCode` is unchanged, a key-up's `repeat` is false and `metaKey` is always false. As in a
  browser, `code` names the key's place on a US keyboard, not its label (the key labelled A on a French AZERTY
  keyboard reads `KeyQ`), and `key` is the layout's character (`"ö"`, `"&"` for AZERTY's unshifted 1): to turn a
  keydown into a Windows virtual-key code (a hotkey your plugin polls with `GetAsyncKeyState`) read `keyCode`, which
  follows the layout, never `code` or `key`. Alt, F10 and
  Alt+key keydowns reach the page (Alt+x types nothing), so `altKey` is right on clicks too. The mouse's side
  buttons arrive as `button` 3 and 4 (`buttons` 8 and 16) on `mousedown` / `mouseup`, then `auxclick`, never
  `click` or `dblclick`, so they activate no button or link; a middle click gives `mousedown`, `mouseup` and
  `auxclick`, and no longer starts autoscroll, eats the next click or follows a link (`dblclick` is the left
  button's only). The wheel scrolls the element under the pointer even before the pointer has moved over a page.
  While a button is held, moves and the release go to the page it
  was pressed on, wherever the pointer is, so a drag released over another page or the game still ends with
  `mouseup` where it began; the pointer moving onto another page sends the one it left `mouseout` and
  `mouseleave`, clearing its `:hover`. In VR each hand is its own pointer: a press holds its page for that hand
  only, and one hand moving to another page never clears the `:hover` where the other ray points. Leaving UI
  mode, or the game losing focus, mid-drag or with Shift, Ctrl or Alt held sends the page that `mouseup` and
  those `keyup`s. `capabilities.keyboardCode` and `capabilities.auxButtons` say the host does this.
- **Text fields** (host 1.5.0): the host follows the focused field itself (an input, a textarea, a
  `contenteditable` region, a field in an iframe); the page needs no script for it. A page has window focus
  (`document.hasFocus()`, the window's `focus` and `blur` events, `:focus` matching) only while it holds UI mode,
  so a page that never held UI mode has none from its load. On VR the host's keyboard rises when a field of the
  page holding UI mode takes the focus (a click, Tab, the page's own `focus()`), and again when the player clicks
  the field that has it after closing the keyboard; it does not rise when UI mode opens on a page whose field
  already has the focus. The field's `inputmode` picks the layout: `numeric`, `decimal` and `tel` a keypad, `email`
  adds `@` and `.com`, `url` adds `/` and `.com`, and `none` keeps the keyboard down (a page with its own keypad).
  `type="number"` with no `inputmode` gets the decimal keypad (digits, `-` and `.`). `enterkeyhint` labels
  the Enter key, and a password field or a textarea puts SteamVR's own keyboard in password or multi-line mode.
  Text typed on any VR keyboard (Magelight's, SteamVR's, OpenComposite's, `Magelight_DeliverChar`) reaches a field as
  typing does: `beforeinput` and `input` with `inputType` `insertText` (a key such as `.com` as one edit, an emoji
  whole), `maxlength` and a cancelled `beforeinput` respected, React's `onChange` with it, and no `keydown`,
  `keypress` or `textInput`; with no field focused, the characters arrive as `keypress` as before. Backspace,
  Enter, Tab, the arrows and Escape stay key events (`keydown` and `keyup`, and Enter's implicit form submit), from
  SteamVR's and OpenComposite's keyboards too. The keyboard's Undo, Redo, Select all, Cut, Copy and Paste buttons
  run on the focused field; Paste reads the clipboard on the player's press, which a page script still cannot.
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

## Gamepad (1.6.0)

A view its mod opted in (C++ `SetViewGamepad`, manifest `"gamepad"`) gets the gamepad while it holds UI mode, on
flat; `capabilities.gamepad` is 1 where the layer can run. In the `keys` style the host types the pad: A is Enter,
B is Escape, the d-pad and left stick are the arrow keys (with key repeat), so the page's own keyboard handling
answers it and it needs nothing below. In the `navigation` style the page gets actions through
`window.magelight.gamepad` (typed as `GamepadHelper` in `@magelight/sdk`; the host passes it the pad on the reserved
`__gamepad` channel, nothing to subscribe), which moves focus for it:

```js
const pad = window.magelight?.gamepad;
if (pad) {
  magelight.send('__escapecapture', '1');          // B reaches the page (see below)
  const close = () => { dispose(); magelight.send('close'); };
  let dispose = pad.attachNavigation({
    root: document.querySelector('#settings'),
    initialFocus: 'first-setting',                  // an id or data-ml-id, an element, or a function
    onBack: close,
  });
  document.addEventListener('keydown', e => { if (e.key === 'Escape') close(); });
  pad.onAction(e => {                              // before the scope's defaults; return true to take it
    if (e.action === 'secondary' && e.phase === 'press') { magelight.send('favourite'); return true; }
  });
  footer.textContent = `${pad.getPrompt('accept').label} Select  ${pad.getPrompt('cancel').label} Back`;
}
```

- **Scopes.** `attachNavigation({ root, initialFocus, onBack, onAction, wrap, getCandidates })` returns a
  dispose. The newest scope moves focus; a modal attaches its own and disposes it when it closes, which gives the
  page under it back its selection. Candidates are the visible, enabled buttons, inputs, selects, textareas,
  links, `[tabindex]` elements and `[data-ml-focusable]` elements inside `root` (a marked element with no `tabindex`
  gets `tabindex="-1"` when it is focused, so it stays out of the Tab order; `getCandidates` for a custom or
  virtualised list). Directions move by geometry, as drawn (CSS `zoom` included); `data-ml-up`, `-down`, `-left`
  and `-right` on a control name the control (by id or `data-ml-id`) that direction goes to. A control removed or
  re-rendered keeps the selection when its id or `data-ml-id` is the same. The helper moves DOM focus only while the
  pad is in use; otherwise it only tracks and marks its selection, so keyboard and mouse use is as without it.
- **What the defaults do.** Accept clicks; on a range, a select or a text field it enters edit mode, where
  directions step the range or the select (`input` and `change` fire) and leave a text field alone (free text needs
  a keyboard). Cancel leaves edit mode first, then calls `onBack`. The shoulders click the `role="tab"` button
  before or after the one with `aria-selected="true"`, stopping at the ends, so the page keeps `aria-selected` up to
  date. The right stick scrolls the nearest scrollable container.
- **B and Escape.** B follows Escape's rule: it closes the view unless the page holds Escape
  (`magelight.send('__escapecapture', '1')`, or the plugin's `SetEscapeCapture`), and only then does the page get
  `cancel`. A page that takes Escape for B handles keyboard Escape too, as above. The helper never takes Escape
  itself.
- **Styling and prompts.** The focused control carries `data-ml-focused` (one at a time); style it yourself
  (`[data-ml-focused] { outline: 3px solid var(--accent); }`). `<html>` carries `data-ml-input` (`gamepad` or
  `keyboardMouse`, which follows the last device used) and `data-ml-gamepad-mode`. `getPrompt(action)` gives the
  label of the control bound to an action in the player's glyph family (`A`, `Cross`, `South`), `onStateChange`
  says when it changes. Both work in the keys style too.
- **Events.** `onAction` sees every input first: `{ control, action, phase, value, x, y, dt }`, phase `press`,
  `release`, `repeat` (a held direction), `change` (a stick, every batch while off centre, with `x` and `y`) or
  `cancel` (the pad changed hands, the view lost it, or the host dropped a flooded batch: stop anything a held button
  was doing). Controls are
  positional (`south`, `east`, `leftShoulder`, `rightStick`); triggers, start and back arrive with action `none`.
- **Cursor mode.** Where the view allows it, a right-stick click (or `setMode('cursor')`) switches to a cursor the
  left stick moves; A clicks there. A direction press, or `setMode('navigation')`, goes back.

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
takes every later 1.x (`^1.0.0` installs 1.5.0), which the append-only page contract allows;
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

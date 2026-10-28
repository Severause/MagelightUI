/** What the host installs as `window.__MAGELIGHT__` at window-object-ready. */
export interface HostInfo {
  /** "0.15.0" */
  version: string;
  /** MAJOR*10000 + MINOR*100 + PATCH — compare against this, never parse the string */
  versionNumber: number;
  viewId: number;
  /** "" for views created through the v1-v3 API */
  modId: string;
  viewName: string;
  /** 1 = supported, 0 = not (`consoleLog` is a level, 0-3); names: `QueryCapability` in the host, or the SDK mock */
  capabilities: Record<string, number>;
  /** Magelight.json devMode: hot reload of page files, JS error banner on the page */
  dev: boolean;
  /** file:///.../Data/SKSE/Plugins/Magelight/ — images live under images/<name>.imgsrc */
  runtimeUrl: string;
}

/** The two verbs the host installs as `window.magelight` (the page core). */
export interface PageCore {
  send(channel: string, payload?: unknown): void;
  on(channel: string, fn: (arg: string) => void): () => void;
  off(channel: string, fn: (arg: string) => void): void;
  _dispatch(channel: string, arg: string): void;
  pending(channel: string): number;
  /** 0.29.0: play a UI sound through the game's audio (absent on older hosts, where `host.sound` does nothing) */
  sound?(name: string): void;
  /** 0.31.9: this view's colours for the host's VR keyboard and drawn cursor; `null` clears both (see `HostTheme`) */
  hostTheme?(theme: HostTheme | null): void;
  /** 1.6.0: the gamepad helper (see `GamepadHelper`); idle unless the view opted in to the gamepad layer */
  gamepad?: GamepadHelper;
}

/** Positional pad controls: `south` is A on an Xbox pad, Cross on a PlayStation one. */
export type GamepadControl =
  | 'none' | 'dpadUp' | 'dpadDown' | 'dpadLeft' | 'dpadRight' | 'start' | 'back' | 'leftThumb' | 'rightThumb'
  | 'leftShoulder' | 'rightShoulder' | 'south' | 'east' | 'west' | 'north' | 'leftTrigger' | 'rightTrigger'
  | 'leftStick' | 'rightStick';

export type GamepadAction =
  | 'none' | 'up' | 'down' | 'left' | 'right' | 'accept' | 'cancel' | 'previousTab' | 'nextTab' | 'secondary'
  | 'tertiary' | 'toggleCursor';

/** The layer as the host last described it to this page (frozen). */
export interface GamepadState {
  enabled: boolean;
  /** this view holds the pad now (it holds UI mode, and nothing above it takes the pad) */
  active: boolean;
  connected: boolean;
  /** `keys`: the host types the pad as Enter, Escape and the arrows; `navigation`: actions reach `onAction` and scopes */
  style: 'keys' | 'navigation';
  mode: 'navigation' | 'cursor';
  /** the device in use, also on `<html data-ml-input>` */
  device: 'gamepad' | 'keyboardMouse';
  glyphFamily: 'xbox' | 'playstation' | 'generic';
  /** empty until the host's first packet (a view that never opted in keeps it empty) */
  bindings: Readonly<Partial<Record<Exclude<GamepadAction, 'none'>, GamepadControl>>>;
  generation: number;
}

/** One input, navigation style. Sticks send `change` with x and y every batch while off centre; a reset sends `cancel`. */
export interface GamepadEvent {
  control: GamepadControl;
  action: GamepadAction;
  phase: 'press' | 'release' | 'repeat' | 'change' | 'cancel';
  value: number;
  x: number;
  y: number;
  /** seconds since the last batch, at most 0.05 */
  dt: number;
  generation: number;
  sequence: number;
}

/** A focus scope: a screen or a modal. The newest scope moves focus; disposing it restores the one below. */
export interface GamepadScope {
  root: Element;
  /** an id or `data-ml-id`, an element, or a function returning one; default the first candidate */
  initialFocus?: string | Element | (() => Element | null | undefined);
  /** cancel (B) when no control is being edited. B reaches the page only while it holds Escape (`__escapecapture`). */
  onBack?(event: GamepadEvent): void;
  /** return true to handle an input before the scope's defaults */
  onAction?(event: GamepadEvent): boolean | void;
  /** focus wraps from the last control to the first */
  wrap?: boolean;
  /** the scope's controls, for a custom or virtualised widget (default: buttons, inputs, selects, textareas, links, [tabindex], [data-ml-focusable]) */
  getCandidates?(): ArrayLike<Element>;
}

/** `window.magelight.gamepad` (host 1.6.0, `capabilities.gamepad`). The focused control carries `data-ml-focused`. */
export interface GamepadHelper {
  version: 1;
  getState(): GamepadState;
  /** every input before the scopes; return true to handle it. Returns an unsubscribe. */
  onAction(fn: (event: GamepadEvent) => boolean | void): () => void;
  onStateChange(fn: (state: GamepadState) => void): () => void;
  /** ask for cursor mode (the view must allow it) or navigation mode */
  setMode(mode: 'navigation' | 'cursor'): void;
  /** the label for an action's control in the player's glyph family, e.g. `A` or `Cross` */
  getPrompt(action: GamepadAction): Readonly<{ action: GamepadAction; control: GamepadControl; family: string; label: string }>;
  /** push a scope; returns its dispose */
  attachNavigation(scope: GamepadScope): () => void;
}

/** `#rrggbb`, exactly: opaque, no shorthand, no alpha. Anything else refuses its part (host 1.1.0+; the whole theme before). */
export type HexColor = `#${string}`;

/**
 * Magelight's own VR keyboard while this view holds UI mode (host 0.31.9). The first eight are required;
 * the host fills in the rest: action = accent when it has 3:1 contrast on key and keyHover (else text),
 * danger = a red with 3:1 on key and keyHover (else text), pressed = accent 20% over key, pressedText =
 * text (or black or white under 4.5:1 on pressed), barHover = panel and key half and half. A given
 * action/danger under 3:1 on key or keyHover, or pressedText under 4.5:1 on pressed, is replaced by its
 * fill-in. Refused when text has under 3:1 contrast on key, keyHover or panel.
 */
export interface KeyboardThemeColors {
  panel: HexColor;
  panelBorder: HexColor;
  key: HexColor;
  keyBorder: HexColor;
  keyHover: HexColor;
  text: HexColor;
  muted: HexColor;
  accent: HexColor;
  action?: HexColor;
  danger?: HexColor;
  pressed?: HexColor;
  /** the label of any key held down, and of Shift or Caps while on */
  pressedText?: HexColor;
  barHover?: HexColor;
}

/** The host's drawn cursor over this view (the same slot as C++ `SetViewCursorTint`); a colour left out keeps the host's. */
export interface CursorThemeColors {
  lit?: HexColor;
  shade?: HexColor;
  ink?: HexColor;
  glow?: HexColor;
  ibeam?: HexColor;
}

/**
 * What `magelight.hostTheme` sends. Each part: an object sets it, `null` clears it, left out keeps it.
 * From host 1.1.0 a bad value refuses only its part (before, the whole message); the reason is in
 * Magelight.log. At most 2 KB as JSON.
 */
export interface HostTheme {
  v: 1;
  keyboard?: KeyboardThemeColors | null;
  cursor?: CursorThemeColors | null;
  /** Host 1.1.0 (`capabilities.hostThemeReply`): answer on the `__hostthemeresult` channel (`HostThemeResult`). */
  reply?: boolean;
}

/** What a host 1.1.0+ sends on `__hostthemeresult` for a `hostTheme` message with `reply: true`. */
export interface HostThemeResult {
  keyboard: 'set' | 'cleared' | 'kept' | 'refused';
  cursor: 'set' | 'cleared' | 'kept' | 'refused';
  /** Why the first refused part was refused. */
  why?: string;
}

declare global {
  interface Window {
    __MAGELIGHT__?: HostInfo;
    magelight?: PageCore;
    __mlNative?: (viewId: string, channel: string, arg: string) => void;
  }
}

export type Handler<T> = (value: T) => void;
export type Unsubscribe = () => void;

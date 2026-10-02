import { host } from './host.js';
import { installMock } from './mock.js';
import type { Handler, PageCore, Unsubscribe } from './types.js';

let core: PageCore | undefined;

/**
 * The page core: the host's `window.magelight` in the game, a mock in a
 * plain browser (set `window.__MAGELIGHT_NO_MOCK__ = true` before the SDK
 * loads to opt out, e.g. under another host that has its own bridge).
 */
export function ensureCore(): PageCore {
  if (core) return core;
  if (typeof window === 'undefined') throw new Error('@magelight/sdk needs a window');
  if (window.magelight) return (core = window.magelight);
  if ((window as unknown as { __MAGELIGHT_NO_MOCK__?: boolean }).__MAGELIGHT_NO_MOCK__) {
    throw new Error('@magelight/sdk: no host and the mock is disabled');
  }
  return (core = installMock());
}

/**
 * Parse what the host sent on a channel. Objects arrive as JSON text
 * (`InteropCall(view, channel, json)`); plain strings pass through when
 * they are not JSON. Mirrors what SeverActions' bridge learned in the
 * field: never throw on a payload, log and return the raw text.
 */
export function parsePayload<T = unknown>(raw: string): T {
  if (raw === '') return '' as unknown as T;
  const c = raw[0];
  if (c === '{' || c === '[' || c === '"' || c === '-' || (c >= '0' && c <= '9') || raw === 'true' || raw === 'false' || raw === 'null') {
    try {
      return JSON.parse(raw) as T;
    } catch {
      /* not JSON after all */
    }
  }
  return raw as unknown as T;
}

export const bridge = {
  /** page → host. Objects are JSON-encoded; strings pass through. */
  send(channel: string, payload?: unknown): void {
    ensureCore().send(channel, payload);
  },
  /** host → page. Payloads that arrived before this call are replayed at once. */
  on<T = unknown>(channel: string, fn: Handler<T>): Unsubscribe {
    return ensureCore().on(channel, (raw) => fn(parsePayload<T>(raw)));
  },
  /** raw text variant of `on` */
  onRaw(channel: string, fn: Handler<string>): Unsubscribe {
    return ensureCore().on(channel, fn);
  },
  /** how many payloads wait on a channel nobody subscribed to yet */
  pending(channel: string): number {
    return ensureCore().pending(channel);
  },
  /** true while this view holds UI mode (cursor up, keyboard to the page) */
  onUIMode(fn: Handler<boolean>): Unsubscribe {
    return ensureCore().on('__uimode', (raw) => fn(raw === '1'));
  },
};

/** A typed channel handle: `const state = channel<State>('state'); state.on(s => …); state.send(...)` */
export function channel<T = unknown>(name: string) {
  return {
    name,
    on(fn: Handler<T>): Unsubscribe {
      return bridge.on<T>(name, fn);
    },
    send(payload?: unknown): void {
      bridge.send(name, payload);
    },
    get pending(): number {
      return bridge.pending(name);
    },
  };
}

/** Tell the host-side mod the page is mounted (a plain send on `ready`; register a listener for it). */
export function ready(detail?: unknown): void {
  bridge.send('ready', detail);
}

export { host };

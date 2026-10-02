import { useEffect, useState, type ReactNode } from 'react';
import { bridge, ensureCore, host } from '@magelight/sdk';

/**
 * The latest payload on a channel (parsed), `initial` until one arrives.
 * A payload that landed before mount is replayed on subscribe, so the
 * first render after mount already has it.
 */
export function useChannel<T>(channel: string, initial: T): T {
  const [value, setValue] = useState<T>(initial);
  useEffect(() => bridge.on<T>(channel, setValue), [channel]);
  return value;
}

/** `useHostData` is `useChannel` under the name SeverActions' frontend used. */
export const useHostData = useChannel;

/** Subscribe with a callback instead of state (for events, not state). */
export function useChannelEvent<T>(channel: string, fn: (value: T) => void): void {
  useEffect(() => bridge.on<T>(channel, fn), [channel, fn]);
}

/** true while this view holds UI mode (cursor up, keyboard to the page). */
export function useUIMode(): boolean {
  const [on, setOn] = useState(false);
  useEffect(() => bridge.onUIMode(setOn), []);
  return on;
}

/** `useCapability('textureImage')` — false with no host. */
export function useCapability(name: string): boolean {
  return host.can(name);
}

/**
 * Play a host UI sound: `const play = useSound(); <button onClick={() => play('click')}>`.
 * Silent with no host and on hosts before 0.29.0; `useCapability('sound')` to feature-detect. Plain
 * markup works without a hook: `<button data-ml-sound="click">`.
 */
export function useSound(): (name: string) => void {
  return host.sound;
}

/** Static host facts for render-time branching (`host.present`, version, ids). */
export function useHost() {
  return host;
}

/** Page → host, stable identity. */
export function useSend(): (channel: string, payload?: unknown) => void {
  return bridge.send;
}

/**
 * Renders children only on Magelight (or the mock in a browser); `fallback`
 * elsewhere. `minVersion` = [major, minor, patch] gates on the host version
 * and renders `outdated` (or `fallback`) when too old.
 */
export function HostGate(props: {
  children: ReactNode;
  fallback?: ReactNode;
  outdated?: ReactNode;
  minVersion?: [number, number, number?];
}): ReactNode {
  // In a plain browser the mock is what makes a host present; install it before the first check.
  if (!host.present && typeof window !== 'undefined' && !window.magelight) {
    try {
      ensureCore();
    } catch {
      /* mock disabled: stay on the fallback */
    }
  }
  if (!host.present) return props.fallback ?? null;
  if (props.minVersion && !host.atLeast(props.minVersion[0], props.minVersion[1], props.minVersion[2] ?? 0)) {
    return props.outdated ?? props.fallback ?? null;
  }
  return props.children;
}

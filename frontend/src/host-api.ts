// Magelight host bridge — the same shape as SeverActions' SKSE_API
// (its skse-api.ts), so the real frontend ports over
// with its data layer unchanged.
//
// C++ -> JS: the host InteropCalls window[channel](payload) — we register the
//            channels below and fan out to subscribers, buffering anything
//            that arrives before React subscribes.
// JS -> C++: sendToHost(fn, arg) calls window[fn](arg) — the shim the host
//            installed at window-object-ready, before this module ran.

type Callback = (...args: unknown[]) => void

const subscribers: { channel: string; callback: Callback }[] = []
const pendingCalls: { channel: string; args: unknown[] }[] = []

function deliver(channel: string, args: unknown[]) {
  const subs = subscribers.filter(s => s.channel === channel)
  if (subs.length === 0) {
    pendingCalls.push({ channel, args })
    return
  }
  for (const sub of subs) {
    try {
      sub.callback(...args)
    } catch (e) {
      console.error(`[host-api] subscriber error on '${channel}':`, e)
    }
  }
}

export const HostApi = {
  subscribe(channel: string, callback: Callback) {
    subscribers.push({ channel, callback })
    // Replay anything the host sent before React mounted.
    for (let i = pendingCalls.length - 1; i >= 0; i--) {
      if (pendingCalls[i].channel === channel) {
        const [p] = pendingCalls.splice(i, 1)
        try {
          callback(...p.args)
        } catch (e) {
          console.error(`[host-api] replay error on '${channel}':`, e)
        }
      }
    }
  },

  unsubscribe(channel: string, callback: Callback) {
    const idx = subscribers.findIndex(s => s.channel === channel && s.callback === callback)
    if (idx !== -1) subscribers.splice(idx, 1)
  },

  /** Call a host listener (the window shim Magelight installed). */
  sendToHost(fn: string, arg?: string): boolean {
    const f = (window as Record<string, unknown>)[fn]
    if (typeof f !== 'function' && f == null) return false
    try {
      ;(f as (a: string) => void)(arg ?? '')
      return true
    } catch (e) {
      console.error(`[host-api] host call '${fn}' threw:`, e)
      return false
    }
  },

  /** True when the host's shims were installed before this bundle ran. */
  bridgePresent(): boolean {
    return (window as Record<string, unknown>)['requestPageData'] != null
  },
}

// Channels the host InteropCalls into this page.
for (const channel of ['receivePageData', 'showNotification']) {
  ;(window as Record<string, unknown>)[channel] = (...args: unknown[]) =>
    deliver(channel, args)
}

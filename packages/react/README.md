# @magelight/react

Hooks over `@magelight/sdk` for pages hosted by Magelight UI.

```tsx
import { useChannel, useUIMode, useSend, HostGate } from '@magelight/react';

function Config() {
  const state = useChannel<{ volume: number }>('state', { volume: 50 });
  const focused = useUIMode();
  const send = useSend();
  return (
    <HostGate fallback={<p>Open this page inside Skyrim.</p>} minVersion={[0, 15]}>
      <input type="range" value={state.volume} onChange={(e) => send('save', { volume: +e.target.value })} />
      {focused ? 'keyboard is yours' : 'HUD mode'}
    </HostGate>
  );
}
```

`useChannel(name, initial)` · `useHostData` (alias) · `useChannelEvent(name, fn)` · `useUIMode()` ·
`useCapability(name)` · `useHost()` · `useSend()` · `useSound()` (host 0.29.0: `play('click')` through
the game's audio) · `<HostGate fallback outdated minVersion>`.

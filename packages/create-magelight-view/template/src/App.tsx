import { useEffect, useState } from 'react';
import { useChannel, useHost, useSend, useUIMode, HostGate } from '@magelight/react';

// Host → page: your DLL/Papyrus sends InteropCall(view, 'state', json).
// Page → host: send('save', {...}) reaches the listener you registered as 'save'.
interface State { volume: number; subtitles: boolean }

const panel: React.CSSProperties = {
  boxSizing: 'border-box', width: 560, height: 380, padding: '18px 22px',
  background: '#1e1913', border: '1px solid #5a4a2e', borderRadius: 6,
  color: '#e8dcc4', font: '14px Georgia, serif',
};

export function App() {
  const host = useHost();
  const hostState = useChannel<State>('state', { volume: 50, subtitles: true });
  const focused = useUIMode();
  const send = useSend();
  // Local state so the page is usable as a manifest-only mod (nothing echoes
  // 'state' back yet): the control moves immediately, and whatever the host
  // sends on the 'state' channel still wins when it arrives.
  const [state, setState] = useState<State>(hostState);
  useEffect(() => { setState(hostState); }, [hostState]);
  const update = (next: State) => { setState(next); send('save', next); };

  return (
    <HostGate fallback={<div style={panel}>Open this page inside Skyrim (or use the mock panel, bottom right).</div>}>
      <div style={panel}>
        <h1 style={{ margin: '0 0 8px', fontSize: 18, color: '#cba560', fontWeight: 'normal' }}>__MODID__</h1>
        <p style={{ color: '#8a7a5a', fontSize: 12, margin: '0 0 14px' }}>
          Magelight {host.version} · view {host.viewName} · {focused ? 'keyboard is yours' : 'HUD mode'}
        </p>
        <label style={{ display: 'block', margin: '10px 0' }}>
          Volume {state.volume}
          <input type="range" min={0} max={100} value={state.volume} style={{ width: '100%' }}
            onChange={(e) => update({ ...state, volume: +e.target.value })} />
        </label>
        <label style={{ display: 'block', margin: '10px 0' }}>
          <input type="checkbox" checked={state.subtitles}
            onChange={(e) => update({ ...state, subtitles: e.target.checked })} /> Subtitles
        </label>
        <p style={{ color: '#8a7a5a', fontSize: 12 }}>F10 or Esc closes this page.</p>
      </div>
    </HostGate>
  );
}

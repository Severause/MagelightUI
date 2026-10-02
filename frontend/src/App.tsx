import { useEffect, useRef, useState } from 'react'
import { HostApi } from './host-api'

interface PlayerData {
  name: string
  level: number
  gold: number
  health: number
  healthMax: number
  location: string
}

interface LogLine {
  time: string
  text: string
}

function now() {
  return new Date().toLocaleTimeString()
}

export default function App() {
  const [player, setPlayer] = useState<PlayerData | null>(null)
  const [rtt, setRtt] = useState<number | null>(null)
  const [notify, setNotify] = useState('')
  const [log, setLog] = useState<LogLine[]>([])
  const [count, setCount] = useState(0)
  const [text, setText] = useState('')
  const [clock, setClock] = useState(now())
  const reqStart = useRef(0)
  const settingN = useRef(0)

  const pushLog = (text: string) =>
    setLog(prev => [...prev.slice(-49), { time: now(), text }])

  useEffect(() => {
    const onPageData = (...args: unknown[]) => {
      const raw = args[0]
      if (reqStart.current) setRtt(performance.now() - reqStart.current)
      try {
        const data = typeof raw === 'string' ? JSON.parse(raw) : raw
        setPlayer(data as PlayerData)
        pushLog(`receivePageData: ${String(raw).length} chars`)
      } catch (e) {
        pushLog(`receivePageData: bad JSON (${e})`)
      }
    }
    const onNotify = (...args: unknown[]) => {
      setNotify(String(args[0] ?? ''))
      pushLog(`showNotification: ${String(args[0] ?? '')}`)
    }
    HostApi.subscribe('receivePageData', onPageData)
    HostApi.subscribe('showNotification', onNotify)
    pushLog(
      HostApi.bridgePresent()
        ? 'bridge shims present at mount'
        : 'bridge shims MISSING at mount'
    )
    const t = setInterval(() => setClock(now()), 1000)
    return () => {
      HostApi.unsubscribe('receivePageData', onPageData)
      HostApi.unsubscribe('showNotification', onNotify)
      clearInterval(t)
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  const requestPlayer = () => {
    reqStart.current = performance.now()
    pushLog('-> requestPageData("react")')
    HostApi.sendToHost('requestPageData', 'react')
  }

  const sendSetting = () => {
    settingN.current += 1
    const payload = JSON.stringify({
      key: 'demoToggle',
      value: settingN.current % 2 === 0,
      n: settingN.current,
    })
    pushLog(`-> settingChanged(${payload})`)
    HostApi.sendToHost('settingChanged', payload)
  }

  return (
    <div className="panel">
      <h1>&#10022; Magelight UI &mdash; React</h1>
      <p className="sub">
        Vite-built React bundle loaded from disk (module scripts + hashed
        assets over file:///). ESC or Page Up closes.
      </p>

      <div className="row">
        <button onClick={requestPlayer}>Request player data</button>
        <button onClick={sendSetting}>Send setting change</button>
        {rtt !== null && <span className="rtt">round-trip: {rtt.toFixed(1)} ms</span>}
      </div>

      <div className="player">
        {player ? (
          <>
            <div>
              <b>{player.name}</b> &mdash; level {player.level}
            </div>
            <div>
              gold {player.gold} &nbsp; health {player.health}/{player.healthMax}
            </div>
            <div>{player.location || '(unknown location)'}</div>
          </>
        ) : (
          <span className="dim">(player data appears here)</span>
        )}
      </div>
      {notify && <div className="notify">{notify}</div>}

      <div className="row">
        <button onClick={() => setCount(c => c + 1)}>React state: {count}</button>
        <input
          value={text}
          placeholder="type here&hellip;"
          onChange={e => setText(e.target.value)}
        />
        <span className="echo">{text && `echo: ${text}`}</span>
      </div>

      <div className="log">
        {log.map((l, i) => (
          <div key={i}>
            {l.time}&nbsp;&nbsp;{l.text}
          </div>
        ))}
      </div>

      <div className="foot">
        <span>
          Bridge:{' '}
          <b className={HostApi.bridgePresent() ? 'ok' : 'bad'}>
            {HostApi.bridgePresent() ? 'connected' : 'NOT INSTALLED'}
          </b>
        </span>
        <span>{clock}</span>
      </div>
    </div>
  )
}

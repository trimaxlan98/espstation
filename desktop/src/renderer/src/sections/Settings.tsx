import { useEffect, useState } from 'react'
import { useSettingsStore } from '../store/settingsStore'
import { Badge, Button, Card, Field, Input, Select, ThemeToggle, useToast } from '../components/ui'
import type { GatewayLogLine, GatewayStatus } from '@shared/types'
import '../styles/sections.css'

const LOG_LIMIT = 200

export function Settings(): React.JSX.Element {
  const { settings, save, saving } = useSettingsStore()
  const toast = useToast()

  const [gatewayUrl, setGatewayUrl] = useState(settings.gatewayUrl)
  const [gatewayToken, setGatewayToken] = useState(settings.gatewayToken)
  const [gatewayPythonPath, setGatewayPythonPath] = useState(settings.gatewayPythonPath)
  const [status, setStatus] = useState<GatewayStatus | null>(null)
  const [logLines, setLogLines] = useState<GatewayLogLine[]>([])

  useEffect(() => {
    setGatewayUrl(settings.gatewayUrl)
    setGatewayToken(settings.gatewayToken)
    setGatewayPythonPath(settings.gatewayPythonPath)
  }, [settings])

  useEffect(() => {
    void window.espstation.gateway.status().then(setStatus)
    const unsub = window.espstation.gateway.onLog((line) => {
      setLogLines((prev) => (prev.length >= LOG_LIMIT ? [...prev.slice(1), line] : [...prev, line]))
    })
    const poll = setInterval(() => {
      void window.espstation.gateway.status().then(setStatus)
    }, 2000)
    return () => {
      unsub()
      clearInterval(poll)
    }
  }, [])

  async function handleSaveConnection(): Promise<void> {
    await save({ gatewayUrl, gatewayToken })
    toast.show({ tone: 'ok', title: 'Connection settings saved' })
  }

  async function handleSaveGateway(): Promise<void> {
    await save({ gatewayPythonPath })
    toast.show({ tone: 'ok', title: 'Gateway settings saved' })
  }

  async function handleModeChange(mode: 'managed' | 'attach'): Promise<void> {
    await save({ gatewayMode: mode })
  }

  async function handleGatewayAction(action: 'start' | 'stop' | 'restart'): Promise<void> {
    const next = await window.espstation.gateway[action]()
    setStatus(next)
  }

  return (
    <div className="section">
      <div className="section__header">
        <h1 className="section__title">Settings</h1>
      </div>

      <div className="grid" style={{ gridTemplateColumns: 'repeat(auto-fit, minmax(360px, 1fr))', alignItems: 'start' }}>
        <Card title="Connection">
          <div className="settings-form">
            <Field label="Gateway URL" htmlFor="gateway-url" hint="Base REST/WS URL of espstation-gateway.">
              <Input id="gateway-url" value={gatewayUrl} onChange={(e) => setGatewayUrl(e.target.value)} />
            </Field>
            <Field label="Bearer token" htmlFor="gateway-token">
              <Input
                id="gateway-token"
                type="password"
                value={gatewayToken}
                onChange={(e) => setGatewayToken(e.target.value)}
              />
            </Field>
            <Button variant="primary" onClick={() => void handleSaveConnection()} loading={saving}>
              Save connection
            </Button>
          </div>
        </Card>

        <Card title="Appearance">
          <div className="settings-row">
            <span>Theme</span>
            <ThemeToggle value={settings.theme} onChange={(theme) => void save({ theme })} />
          </div>
        </Card>

        <Card
          title="Gateway supervisor"
          status={status?.state === 'running' ? 'ok' : status?.state === 'error' ? 'crit' : 'neutral'}
          actions={
            <Badge tone={status?.mode === 'managed' ? 'info' : 'neutral'} variant="outline">
              {status?.mode ?? 'managed'}
            </Badge>
          }
        >
          <div className="settings-form">
            <Field label="Mode" htmlFor="gateway-mode" hint="Managed: this app spawns and supervises the gateway process. Attach: you run it yourself.">
              <Select
                id="gateway-mode"
                value={settings.gatewayMode}
                onChange={(e) => void handleModeChange(e.target.value as 'managed' | 'attach')}
                options={[
                  { value: 'managed', label: 'Managed (spawn locally)' },
                  { value: 'attach', label: 'Attach to a running gateway' }
                ]}
              />
            </Field>
            <Field
              label="Python interpreter override"
              htmlFor="gateway-python"
              hint="Leave blank to use gateway/.venv/bin/python next to the repo."
            >
              <Input
                id="gateway-python"
                value={gatewayPythonPath}
                onChange={(e) => setGatewayPythonPath(e.target.value)}
                placeholder="/path/to/python"
              />
            </Field>
            <Button variant="subtle" onClick={() => void handleSaveGateway()}>
              Save gateway settings
            </Button>

            <div className="settings-row">
              <span>
                Status: <strong>{status?.state ?? 'unknown'}</strong>
                {status?.pid ? ` (pid ${status.pid})` : ''}
              </span>
            </div>
            {status?.error ? <p style={{ color: 'var(--crit-fg)', fontSize: 'var(--text-xs-size)' }}>{status.error}</p> : null}

            <div style={{ display: 'flex', gap: 'var(--space-2)' }}>
              <Button variant="default" onClick={() => void handleGatewayAction('start')} disabled={settings.gatewayMode === 'attach'}>
                Start
              </Button>
              <Button variant="default" onClick={() => void handleGatewayAction('stop')} disabled={settings.gatewayMode === 'attach'}>
                Stop
              </Button>
              <Button variant="subtle" onClick={() => void handleGatewayAction('restart')} disabled={settings.gatewayMode === 'attach'}>
                Restart
              </Button>
            </div>
          </div>
        </Card>

        <Card title="Gateway log" bodyPadding="dense">
          <div className="log-pane" style={{ maxHeight: 260 }}>
            {logLines.length === 0
              ? null
              : logLines
                  .slice()
                  .reverse()
                  .map((l, i) => (
                    <div className="log-pane__line" key={i}>
                      <span className="log-pane__ts">{new Date(l.ts * 1000).toLocaleTimeString()}</span>
                      <span className={`log-pane__level log-pane__level--${l.level}`}>{l.level}</span>
                      <span className="log-pane__tag">{l.stream}</span>
                      <span className="log-pane__msg">{l.message}</span>
                    </div>
                  ))}
          </div>
        </Card>
      </div>
    </div>
  )
}

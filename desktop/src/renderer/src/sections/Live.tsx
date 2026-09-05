import { useEffect, useMemo, useState } from 'react'
import { useNodesStore } from '../store/nodesStore'
import { useStreamStore } from '../store/streamStore'
import { TelemetryChart } from '../components/TelemetryChart'
import { Card, EmptyState, Spinner } from '../components/ui'
import '../styles/sections.css'

const MAX_CHARTED_CHANNELS = 4

export function Live(): React.JSX.Element {
  const activeNodeId = useNodesStore((s) => s.activeNodeId)
  const detail = useNodesStore((s) => (activeNodeId !== null ? s.detailByNode.get(activeNodeId) : undefined))
  const loadDetail = useNodesStore((s) => s.loadDetail)
  const version = useStreamStore((s) => s.version)
  const getSeries = useStreamStore((s) => s.getSeries)
  const logs = useStreamStore((s) => s.logs)
  const events = useStreamStore((s) => s.events)

  const [selected, setSelected] = useState<string[]>([])

  useEffect(() => {
    if (activeNodeId === null) return
    void loadDetail(activeNodeId)
  }, [activeNodeId, loadDetail])

  // Default to the highest-rate channel once the NDB is known — a sensible
  // starting point that stays purely NDB-driven (no hard-coded channel key).
  useEffect(() => {
    if (!detail || selected.length > 0) return
    const byRate = [...detail.ndb].sort((a, b) => b.rate_hz - a.rate_hz)
    const first = byRate[0]
    if (first) setSelected([first.key])
  }, [detail, selected.length])

  function toggleChannel(key: string): void {
    setSelected((prev) => {
      if (prev.includes(key)) return prev.filter((k) => k !== key)
      if (prev.length >= MAX_CHARTED_CHANNELS) return prev
      return [...prev, key]
    })
  }

  // `version` is a dependency purely to re-run this memo on every new
  // sample — getSeries() itself always returns the current snapshot, it
  // just wouldn't be re-read without something in the deps array changing.
  const chartSeries = useMemo(() => {
    if (activeNodeId === null || !detail) return []
    return selected.flatMap((key) => {
      const channel = detail.ndb.find((c) => c.key === key)
      if (!channel) return []
      return [{ name: channel.name, unit: channel.unit, data: getSeries(activeNodeId, key) }]
    })
  }, [activeNodeId, detail, selected, getSeries, version])

  const nodeLogs = useMemo(() => (activeNodeId === null ? logs : logs.filter((l) => l.node_id === activeNodeId)), [logs, activeNodeId])
  const nodeEvents = useMemo(
    () => (activeNodeId === null ? events : events.filter((e) => e.node_id === activeNodeId)),
    [events, activeNodeId]
  )

  if (activeNodeId === null) {
    return (
      <div className="section">
        <div className="section__header">
          <h1 className="section__title">Live</h1>
        </div>
        <EmptyState title="No active node" description="Select a node in the Nodes section or the top bar's node switcher." />
      </div>
    )
  }

  return (
    <div className="section">
      <div className="section__header">
        <div>
          <h1 className="section__title">Live</h1>
          <p className="section__description">Telemetry dashboard driven by the NDB — node #{activeNodeId}.</p>
        </div>
      </div>

      <div className="live-layout">
        <div className="live-layout__main">
          <Card title="Channels">
            {!detail ? (
              <Spinner size="sm" />
            ) : (
              <div className="channel-picker">
                {detail.ndb.map((ch) => (
                  <button
                    key={ch.key}
                    type="button"
                    className={`channel-chip ${selected.includes(ch.key) ? 'channel-chip--active' : ''}`}
                    aria-pressed={selected.includes(ch.key)}
                    onClick={() => toggleChannel(ch.key)}
                  >
                    {ch.name}
                  </button>
                ))}
              </div>
            )}
          </Card>
          <Card title="Chart" bodyPadding="dense">
            {chartSeries.length === 0 ? (
              <EmptyState title="No channel selected" description="Pick a channel above to start charting its live values." />
            ) : (
              <TelemetryChart series={chartSeries} />
            )}
          </Card>
          <Card title="Log stream" bodyPadding="dense">
            <div className="log-pane" style={{ maxHeight: 220 }}>
              {nodeLogs.length === 0 ? (
                <EmptyState title="No log lines yet" />
              ) : (
                nodeLogs
                  .slice()
                  .reverse()
                  .map((l, i) => (
                    <div className="log-pane__line" key={i}>
                      <span className="log-pane__ts">{new Date(l.ts * 1000).toLocaleTimeString()}</span>
                      <span className={`log-pane__level log-pane__level--${l.level}`}>{l.level}</span>
                      <span className="log-pane__tag">{l.tag}</span>
                      <span className="log-pane__msg">{l.message}</span>
                    </div>
                  ))
              )}
            </div>
          </Card>
        </div>
        <div className="live-layout__side">
          <Card title="Events" bodyPadding="dense">
            <div className="event-rail">
              {nodeEvents.length === 0 ? (
                <EmptyState title="No events yet" />
              ) : (
                nodeEvents
                  .slice()
                  .reverse()
                  .map((e, i) => (
                    <div className={`event-rail__item event-rail__item--${e.severity}`} key={i}>
                      <span className="event-rail__code">{e.code}</span>
                      <span>{new Date(e.ts * 1000).toLocaleTimeString()}</span>
                    </div>
                  ))
              )}
            </div>
          </Card>
        </div>
      </div>
    </div>
  )
}

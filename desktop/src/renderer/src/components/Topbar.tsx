import { useMemo } from 'react'
import { useConnectionStore } from '../store/connectionStore'
import { useNodesStore } from '../store/nodesStore'
import { StatusDot } from './ui/StatusDot'
import { Select } from './ui/Field'
import type { StatusTone } from './ui/types'
import '../styles/layout.css'

const WS_LABEL: Record<string, string> = {
  idle: 'Not connected',
  connecting: 'Connecting…',
  open: 'Live',
  closed: 'Reconnecting…',
  error: 'Connection error'
}

const WS_TONE: Record<string, StatusTone> = {
  idle: 'neutral',
  connecting: 'info',
  open: 'ok',
  closed: 'warn',
  error: 'crit'
}

export function Topbar(): React.JSX.Element {
  const wsState = useConnectionStore((s) => s.wsState)
  const pingError = useConnectionStore((s) => s.pingError)
  const nodes = useNodesStore((s) => s.nodes)
  const activeNodeId = useNodesStore((s) => s.activeNodeId)
  const selectNode = useNodesStore((s) => s.selectNode)

  const tone = WS_TONE[wsState] ?? 'neutral'
  const label = pingError ? 'Gateway unreachable' : (WS_LABEL[wsState] ?? wsState)

  const options = useMemo(
    () => nodes.map((n) => ({ value: String(n.node_id), label: `${n.label} (#${n.node_id})` })),
    [nodes]
  )

  return (
    <header className="topbar">
      <div className="topbar__status">
        <StatusDot tone={tone} label={label} />
        <span>{label}</span>
      </div>
      <div className="topbar__spacer" />
      {nodes.length > 0 ? (
        <Select
          aria-label="Active node"
          className="topbar__node-select"
          options={options}
          value={activeNodeId !== null ? String(activeNodeId) : ''}
          onChange={(e) => selectNode(e.target.value ? Number(e.target.value) : null)}
        />
      ) : null}
    </header>
  )
}

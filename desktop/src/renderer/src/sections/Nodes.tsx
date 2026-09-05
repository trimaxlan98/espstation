import { useCallback, useEffect, useState } from 'react'
import { gatewayClient } from '../lib/gatewaySingleton'
import { useNodesStore } from '../store/nodesStore'
import { formatBytes, formatRelativeTime, formatUptime } from '../lib/format'
import { groupChannels } from '../lib/channelFormat'
import {
  Badge,
  Button,
  Card,
  ConfirmDialog,
  EmptyState,
  Field,
  Input,
  Select,
  Spinner,
  TBody,
  TD,
  TH,
  THead,
  TR,
  Table,
  Toolbar,
  useToast
} from '../components/ui'
import type { CreateLinkRequest, Link, LinkKind, NodeSummary, SerialPort } from '../lib/apiTypes'
import '../styles/sections.css'

const POLL_INTERVAL_MS = 4000

function stateTone(online: boolean, state: NodeSummary['state']): 'ok' | 'warn' | 'crit' | 'neutral' {
  if (!online) return 'neutral'
  if (state === 'degraded') return 'warn'
  if (state === 'safe') return 'crit'
  return 'ok'
}

/** Which mutating action a ConfirmDialog is currently open for — null means closed. */
type PendingAction =
  | { kind: 'reboot'; nodeId: number }
  | { kind: 'erase'; nodeId: number }
  | { kind: 'detach'; linkId: string }
  | null

function NodesTable({
  nodes,
  activeNodeId,
  onSelect
}: {
  nodes: NodeSummary[]
  activeNodeId: number | null
  onSelect: (id: number) => void
}): React.JSX.Element {
  if (nodes.length === 0) {
    return (
      <EmptyState
        title="No nodes yet"
        description="Attach a serial/TCP link, or spawn a simulated node, to see it appear here."
      />
    )
  }

  return (
    <div style={{ overflow: 'auto' }}>
      <Table zebra>
        <THead>
          <TR>
            <TH>Label</TH>
            <TH>State</TH>
            <TH align="right">Heap</TH>
            <TH align="right">RSSI</TH>
            <TH align="right">Uptime</TH>
            <TH>Firmware</TH>
            <TH>Last seen</TH>
          </TR>
        </THead>
        <TBody>
          {nodes.map((n) => (
            <TR
              key={n.node_id}
              onClick={() => onSelect(n.node_id)}
              style={{ cursor: 'pointer', background: n.node_id === activeNodeId ? 'var(--surface-2)' : undefined }}
            >
              <TD>
                <span className={`node-row__label ${n.online ? '' : 'node-row__label--offline'}`}>
                  <Badge tone={stateTone(n.online, n.state)}>{n.online ? n.state : 'offline'}</Badge>
                  {n.label}
                </span>
              </TD>
              <TD>{n.state}</TD>
              <TD numeric>{formatBytes(n.heap_free)}</TD>
              <TD numeric>{n.rssi} dBm</TD>
              <TD numeric>{formatUptime(n.uptime_ms)}</TD>
              <TD>{n.fw}</TD>
              <TD>{formatRelativeTime(n.last_seen)}</TD>
            </TR>
          ))}
        </TBody>
      </Table>
    </div>
  )
}

function NodeDetailPanel({
  nodeId,
  onReboot,
  onErase
}: {
  nodeId: number
  onReboot: (id: number) => void
  onErase: (id: number) => void
}): React.JSX.Element {
  const detail = useNodesStore((s) => s.detailByNode.get(nodeId))
  const loadDetail = useNodesStore((s) => s.loadDetail)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    setError(null)
    loadDetail(nodeId).catch((err: unknown) => setError(err instanceof Error ? err.message : String(err)))
  }, [nodeId, loadDetail])

  if (error) {
    return <EmptyState title="Could not load node detail" description={error} />
  }
  if (!detail) {
    return (
      <div style={{ display: 'flex', justifyContent: 'center', padding: 'var(--space-6)' }}>
        <Spinner />
      </div>
    )
  }

  const groups = groupChannels(detail.ndb)

  return (
    <>
      <div style={{ display: 'flex', flexDirection: 'column', gap: 'var(--space-2)', marginBottom: 'var(--space-3)' }}>
        <div style={{ display: 'flex', gap: 'var(--space-2)', flexWrap: 'wrap' }}>
          {detail.caps.map((cap) => (
            <Badge key={cap} tone="info" variant="outline">
              {cap}
            </Badge>
          ))}
        </div>
        <div className="ndb-row">
          <span className="ndb-row__name">MAC</span>
          <span className="ndb-row__meta">{detail.mac}</span>
        </div>
        <div className="ndb-row">
          <span className="ndb-row__name">Target</span>
          <span className="ndb-row__meta">{detail.target}</span>
        </div>
        <div className="ndb-row">
          <span className="ndb-row__name">Boot</span>
          <span className="ndb-row__meta">
            #{detail.boot.count} · {detail.boot.reason}
          </span>
        </div>
      </div>

      <div className="ndb-list">
        {[...groups.entries()].map(([group, channels]) => (
          <div className="ndb-group" key={group}>
            <div className="ndb-group__title">{group}</div>
            {channels.map((ch) => (
              <div className="ndb-row" key={ch.id}>
                <span className="ndb-row__name">{ch.name}</span>
                <span className="ndb-row__meta">
                  {ch.key} · {ch.unit || '—'} · {ch.rate_hz} Hz
                </span>
              </div>
            ))}
          </div>
        ))}
      </div>

      <Toolbar style={{ borderBottom: 'none', paddingLeft: 0, paddingRight: 0 }}>
        <Button variant="subtle" onClick={() => onReboot(nodeId)}>
          Reboot
        </Button>
        <Button variant="danger" onClick={() => onErase(nodeId)}>
          Erase store
        </Button>
      </Toolbar>
    </>
  )
}

function LinkManager({
  links,
  ports,
  onCreate,
  onDetach,
  onSpawnSim,
  busy
}: {
  links: Link[]
  ports: SerialPort[]
  onCreate: (req: CreateLinkRequest) => void
  onDetach: (id: string) => void
  onSpawnSim: () => void
  busy: boolean
}): React.JSX.Element {
  const [kind, setKind] = useState<LinkKind>('serial')
  const [path, setPath] = useState('')
  const [host, setHost] = useState('')
  const [port, setPort] = useState('')

  function submit(): void {
    if (kind === 'serial') {
      if (!path) return
      onCreate({ kind, path })
    } else if (kind === 'tcp') {
      if (!host || !port) return
      onCreate({ kind, host, port: Number(port) })
    } else {
      onCreate({ kind: 'sim' })
    }
  }

  return (
    <>
      <div className="link-form">
        <Field label="Kind" htmlFor="link-kind">
          <Select
            id="link-kind"
            value={kind}
            onChange={(e) => setKind(e.target.value as LinkKind)}
            options={[
              { value: 'serial', label: 'Serial' },
              { value: 'tcp', label: 'TCP' },
              { value: 'sim', label: 'Simulated' }
            ]}
          />
        </Field>
        {kind === 'serial' ? (
          <Field label="Port" htmlFor="link-path">
            <Select
              id="link-path"
              value={path}
              onChange={(e) => setPath(e.target.value)}
              placeholder="Select a port"
              options={ports.map((p) => ({ value: p.path, label: `${p.path}${p.in_use ? ' (in use)' : ''}` }))}
            />
          </Field>
        ) : null}
        {kind === 'tcp' ? (
          <>
            <Field label="Host" htmlFor="link-host">
              <Input id="link-host" value={host} onChange={(e) => setHost(e.target.value)} placeholder="192.168.1.42" />
            </Field>
            <Field label="Port" htmlFor="link-port">
              <Input id="link-port" value={port} onChange={(e) => setPort(e.target.value)} placeholder="3333" inputMode="numeric" />
            </Field>
          </>
        ) : null}
        <Button variant="primary" onClick={submit} loading={busy}>
          Attach
        </Button>
      </div>

      <div style={{ marginTop: 'var(--space-3)' }}>
        {links.length === 0 ? (
          <p style={{ color: 'var(--text-muted)', fontSize: 'var(--text-sm-size)' }}>No links attached.</p>
        ) : (
          <Table dense>
            <THead>
              <TR>
                <TH>Kind</TH>
                <TH>Target</TH>
                <TH>Status</TH>
                <TH />
              </TR>
            </THead>
            <TBody>
              {links.map((l) => (
                <TR key={l.id}>
                  <TD>{l.kind}</TD>
                  <TD>{l.path ?? (l.host ? `${l.host}:${l.port}` : l.scenario ?? '—')}</TD>
                  <TD>
                    <Badge tone={l.connected ? 'ok' : 'neutral'}>{l.connected ? 'connected' : 'idle'}</Badge>
                  </TD>
                  <TD align="right">
                    <Button variant="ghost" size="sm" onClick={() => onDetach(l.id)}>
                      Detach
                    </Button>
                  </TD>
                </TR>
              ))}
            </TBody>
          </Table>
        )}
      </div>

      <Toolbar style={{ borderBottom: 'none', paddingLeft: 0, paddingRight: 0, marginTop: 'var(--space-2)' }}>
        <Button variant="subtle" onClick={onSpawnSim}>
          Spawn simulated node
        </Button>
      </Toolbar>
    </>
  )
}

export function Nodes(): React.JSX.Element {
  const nodes = useNodesStore((s) => s.nodes)
  const loading = useNodesStore((s) => s.loading)
  const error = useNodesStore((s) => s.error)
  const load = useNodesStore((s) => s.load)
  const activeNodeId = useNodesStore((s) => s.activeNodeId)
  const selectNode = useNodesStore((s) => s.selectNode)
  const toast = useToast()

  const [links, setLinks] = useState<Link[]>([])
  const [ports, setPorts] = useState<SerialPort[]>([])
  const [linkBusy, setLinkBusy] = useState(false)
  const [pending, setPending] = useState<PendingAction>(null)

  const refreshLinks = useCallback(async () => {
    try {
      const [linkList, portList] = await Promise.all([gatewayClient.listLinks(), gatewayClient.listPorts()])
      setLinks(linkList)
      setPorts(portList)
    } catch {
      // Non-fatal — the link manager just shows what it last knew; the
      // top-level connection banner already reports gateway reachability.
    }
  }, [])

  useEffect(() => {
    void load()
    void refreshLinks()
    const timer = setInterval(() => {
      void load()
      void refreshLinks()
    }, POLL_INTERVAL_MS)
    return () => clearInterval(timer)
  }, [load, refreshLinks])

  async function handleCreateLink(req: CreateLinkRequest): Promise<void> {
    setLinkBusy(true)
    try {
      await gatewayClient.createLink(req)
      await refreshLinks()
      toast.show({ tone: 'ok', title: 'Link attached' })
    } catch (err) {
      toast.show({ tone: 'crit', title: 'Could not attach link', description: err instanceof Error ? err.message : String(err) })
    } finally {
      setLinkBusy(false)
    }
  }

  async function confirmDetach(linkId: string): Promise<void> {
    await gatewayClient.deleteLink(linkId)
    await refreshLinks()
    toast.show({ tone: 'ok', title: 'Link detached' })
  }

  async function confirmReboot(nodeId: number): Promise<void> {
    const res = await gatewayClient.sendCommand(nodeId, { op: 'node.reboot' })
    if (!res.ok) throw new Error(res.err?.message ?? 'reboot rejected')
    toast.show({ tone: 'ok', title: `Node ${nodeId} rebooting` })
  }

  async function confirmErase(nodeId: number): Promise<void> {
    const res = await gatewayClient.sendCommand(nodeId, { op: 'store.erase' })
    if (!res.ok) throw new Error(res.err?.message ?? 'erase rejected')
    toast.show({ tone: 'ok', title: `Node ${nodeId} store erased` })
  }

  async function handleSpawnSim(): Promise<void> {
    try {
      await gatewayClient.spawnSim({})
      await load()
      toast.show({ tone: 'ok', title: 'Simulated node spawned' })
    } catch (err) {
      toast.show({ tone: 'crit', title: 'Could not spawn node', description: err instanceof Error ? err.message : String(err) })
    }
  }

  return (
    <div className="section">
      <div className="section__header">
        <div>
          <h1 className="section__title">Nodes</h1>
          <p className="section__description">The fleet: discovered ports, identity, health, and per-node state.</p>
        </div>
        {loading ? <Spinner size="sm" /> : null}
      </div>

      {error ? <EmptyState title="Could not reach the gateway" description={error} /> : null}

      <div className="nodes-layout">
        <div className="nodes-layout__main">
          <Card title="Fleet" bodyPadding="flush">
            <NodesTable nodes={nodes} activeNodeId={activeNodeId} onSelect={selectNode} />
          </Card>
          <Card title="Links">
            <LinkManager
              links={links}
              ports={ports}
              busy={linkBusy}
              onCreate={(req) => void handleCreateLink(req)}
              onDetach={(id) => setPending({ kind: 'detach', linkId: id })}
              onSpawnSim={() => void handleSpawnSim()}
            />
          </Card>
        </div>
        <Card title="Node detail" subtitle={activeNodeId !== null ? `#${activeNodeId}` : undefined}>
          {activeNodeId === null ? (
            <EmptyState title="Select a node" description="Choose a node from the fleet table to see its NDB and details." />
          ) : (
            <NodeDetailPanel
              nodeId={activeNodeId}
              onReboot={(id) => setPending({ kind: 'reboot', nodeId: id })}
              onErase={(id) => setPending({ kind: 'erase', nodeId: id })}
            />
          )}
        </Card>
      </div>

      <ConfirmDialog
        open={pending !== null}
        onClose={() => setPending(null)}
        title={
          pending?.kind === 'reboot'
            ? 'Reboot node?'
            : pending?.kind === 'erase'
              ? 'Erase stored telemetry?'
              : 'Detach link?'
        }
        description={
          pending?.kind === 'reboot'
            ? 'The node will restart immediately. Any unsent buffered telemetry survives the reboot (store & forward), but the current session ends.'
            : pending?.kind === 'erase'
              ? 'This permanently deletes all telemetry buffered on the node (RAM ring and LittleFS). This cannot be undone.'
              : 'The station stops talking to this link. The node keeps running autonomously if its experiment is standalone.'
        }
        confirmLabel={pending?.kind === 'detach' ? 'Detach' : pending?.kind === 'reboot' ? 'Reboot' : 'Erase'}
        onConfirm={async () => {
          if (!pending) return
          if (pending.kind === 'reboot') await confirmReboot(pending.nodeId)
          else if (pending.kind === 'erase') await confirmErase(pending.nodeId)
          else await confirmDetach(pending.linkId)
        }}
      />
    </div>
  )
}

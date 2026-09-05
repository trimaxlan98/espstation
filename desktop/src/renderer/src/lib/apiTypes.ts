/**
 * Wire types for the espstation-gateway REST + WS API (given directly by the
 * orchestrator — the gateway sibling agent's docs/API.md is not read here,
 * see the desktop builder's brief). Base URL http://127.0.0.1:8787, bearer
 * token auth, WS at /ws/stream?token=…. All timestamps crossing this
 * boundary are float Unix epoch seconds (protocol/PROTOCOL.md §4: the
 * gateway is the one conversion point from the node's monotonic ms clock).
 */

// ---------------------------------------------------------------------------
// GET /api/ping
// ---------------------------------------------------------------------------

export interface PingResponse {
  name: string
  version: string
  api: string
  uptime: number
}

// ---------------------------------------------------------------------------
// GET /api/ports
// ---------------------------------------------------------------------------

export interface SerialPort {
  path: string
  vid: string | null
  pid: string | null
  description: string | null
  in_use: boolean
}

// ---------------------------------------------------------------------------
// GET/POST/DELETE /api/links
// ---------------------------------------------------------------------------

export type LinkKind = 'serial' | 'tcp' | 'sim'

export interface Link {
  id: string
  kind: LinkKind
  /** serial */
  path?: string
  baud?: number
  /** tcp */
  host?: string
  port?: number
  /** sim */
  scenario?: string
  connected: boolean
  node_id?: number | null
  created_at: number
}

export interface CreateLinkRequest {
  kind: LinkKind
  path?: string
  baud?: number
  host?: string
  port?: number
  scenario?: string
}

// ---------------------------------------------------------------------------
// GET /api/nodes, GET /api/nodes/{node_id}
// ---------------------------------------------------------------------------

/** `state` mirrors the firmware state machine (protocol/PROTOCOL.md §4.3 HEARTBEAT). */
export type NodeState = 'boot' | 'idle' | 'running' | 'degraded' | 'safe'

export interface NodeSummary {
  node_id: number
  label: string
  mac: string
  state: NodeState
  online: boolean
  last_seen: number
  uptime_ms: number
  heap_free: number
  rssi: number
  fw: string
  target: string
  link_id: string | null
}

/** NDB channel encodings, per protocol/PROTOCOL.md §4.4. */
export type ChannelType = 'u8' | 'i8' | 'u16' | 'i16' | 'u32' | 'i32' | 'f32' | 'bool'

export interface NdbChannel {
  id: number
  key: string
  name: string
  unit: string
  type: ChannelType
  rate_hz: number
  group: string
  min?: number
  max?: number
}

export interface NodeCaps {
  telemetry?: boolean
  experiment?: boolean
  espnow?: boolean
  store_forward?: boolean
  ota?: boolean
  [cap: string]: boolean | undefined
}

export interface NodeBoot {
  count: number
  reason: string
  uptime_ms: number
}

export interface NodeDetail extends NodeSummary {
  ndb: NdbChannel[]
  caps: string[]
  boot: NodeBoot
}

// ---------------------------------------------------------------------------
// POST /api/nodes/{node_id}/command
// ---------------------------------------------------------------------------

export interface CommandRequest {
  op: string
  args?: Record<string, unknown>
}

export interface CommandError {
  code: string
  message: string
}

export interface CommandResult {
  id: number
  ok: boolean
  data?: Record<string, unknown>
  err?: CommandError
}

// ---------------------------------------------------------------------------
// GET /api/nodes/{node_id}/telemetry
// ---------------------------------------------------------------------------

/** [timestamp (epoch seconds), value] */
export type TelemetryPoint = [number, number]

export interface TelemetryQuery {
  since?: number
  until?: number
  channels?: string[]
  max_points?: number
}

export interface TelemetryResponse {
  channels: Record<string, TelemetryPoint[]>
}

// ---------------------------------------------------------------------------
// Experiments (docs/EXPERIMENTS.md)
// ---------------------------------------------------------------------------

export type ExperimentStartMode = 'manual' | 'on_boot' | 'at_ms' | 'on_trigger'

export interface ExperimentChannelSpec {
  key: string
  rate_hz: number
  enc?: ChannelType
  scale?: number
}

export interface ExperimentTriggerCondition {
  channel: string
  op: '>' | '<' | '>=' | '<=' | '==' | '!='
  value: number
  for_ms?: number
}

export interface ExperimentTriggerAction {
  action: 'set_state' | 'set_rate' | 'set_gpio' | 'stop' | 'reboot' | 'mark' | 'burst'
  [param: string]: unknown
}

export interface ExperimentTrigger {
  when: ExperimentTriggerCondition
  emit: string
  do: ExperimentTriggerAction[]
}

export interface ExperimentNetworkSpec {
  mode: 'espnow' | 'wifi_sta' | 'wifi_ap' | 'mesh'
  role: string
  channel: number
  peers: 'auto' | string[]
  test?: {
    kind: 'loss_latency' | 'throughput' | 'range_sweep' | 'flood' | 'custom'
    rate_hz: number
    payload_bytes: number
    duration_ms: number
  }
}

export interface ExperimentSpec {
  schema: number
  id: string
  name: string
  standalone: boolean
  persist: boolean
  duration_ms: number
  start: { mode: ExperimentStartMode; at_ms?: number }
  channels: ExperimentChannelSpec[]
  triggers: ExperimentTrigger[]
  network: ExperimentNetworkSpec | null
  meta: Record<string, unknown>
}

export interface ExperimentValidationResult {
  valid: boolean
  errors: string[]
}

// ---------------------------------------------------------------------------
// GET /api/runs, GET /api/runs/{id}, GET /api/runs/{id}/samples
// ---------------------------------------------------------------------------

export type RunState = 'idle' | 'armed' | 'running' | 'paused' | 'done' | 'aborted'

export interface RunSummary {
  id: string
  node_id: number
  spec_id: string
  state: RunState
  started_at: number
  ended_at: number | null
  samples: number
}

export interface RunSample {
  ts: number
  channel: string
  value: number
}

// ---------------------------------------------------------------------------
// GET /api/network/topology
// ---------------------------------------------------------------------------

export interface TopologyNode {
  node_id: number
  label: string
  role?: string
}

export interface TopologyEdge {
  a: number
  b: number
  rssi: number
  loss: number
  rtt_ms: number
}

export interface NetworkTopology {
  nodes: TopologyNode[]
  edges: TopologyEdge[]
}

// ---------------------------------------------------------------------------
// GET /api/sim/scenarios, POST /api/sim/spawn, POST /api/sim/fault
// ---------------------------------------------------------------------------

export interface SimScenario {
  id: string
  name: string
  description: string
}

export interface SimSpawnRequest {
  scenario?: string
  count?: number
}

export interface SimFaultRequest {
  node_id: number
  kind: string
  args?: Record<string, unknown>
}

// ---------------------------------------------------------------------------
// WS /ws/stream
// ---------------------------------------------------------------------------

export type StreamMessageKind = 'heartbeat' | 'telemetry' | 'log' | 'event' | 'node' | 'link' | 'raw'

export interface StreamMessage<TData = unknown> {
  kind: StreamMessageKind
  node_id: number | null
  ts: number
  data: TData
}

export interface TelemetryStreamData {
  channel: string
  value: number
  replay?: boolean
}

export interface LogStreamData {
  level: 'error' | 'warn' | 'info' | 'debug' | 'verbose'
  tag: string
  message: string
}

export interface EventStreamData {
  code: string
  severity: 'debug' | 'info' | 'warning' | 'error' | 'critical'
  data?: Record<string, unknown>
}

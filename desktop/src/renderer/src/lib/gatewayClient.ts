/**
 * Typed HTTP client + reconnecting WS client for the espstation-gateway.
 * Both talk directly to the gateway from the renderer (it's localhost) — no
 * IPC hop, per docs/ARCHITECTURE.md's "typed preload, no proxying HTTP/WS
 * through main" invariant. `fetch`/`WebSocket` are injectable so tests never
 * touch the network.
 */
import type {
  CommandRequest,
  CommandResult,
  CreateLinkRequest,
  ExperimentSpec,
  ExperimentValidationResult,
  Link,
  NetworkTopology,
  NodeDetail,
  NodeSummary,
  PingResponse,
  RunSample,
  RunSummary,
  SerialPort,
  SimFaultRequest,
  SimScenario,
  SimSpawnRequest,
  StreamMessage,
  TelemetryQuery,
  TelemetryResponse
} from './apiTypes'

export interface GatewayConfig {
  baseUrl: string
  token: string
}

/** Thrown for every non-2xx response and for network-level failures alike, distinguished by `kind`. */
export class GatewayError extends Error {
  readonly kind: 'network' | 'http'
  readonly status: number | null
  readonly body: unknown

  constructor(message: string, opts: { kind: 'network' | 'http'; status?: number; body?: unknown }) {
    super(message)
    this.name = 'GatewayError'
    this.kind = opts.kind
    this.status = opts.status ?? null
    this.body = opts.body
  }
}

/** Strips a trailing slash so `baseUrl + path` never double-slashes. */
function normalizeBaseUrl(baseUrl: string): string {
  return baseUrl.replace(/\/+$/, '')
}

/** Pulls a `message` string out of an error body of unknown shape, if present. */
function extractMessage(body: unknown): string | null {
  if (body && typeof body === 'object' && 'message' in body) {
    const message = (body as Record<string, unknown>)['message']
    if (typeof message === 'string') return message
  }
  return null
}

function buildQuery(params: Record<string, string | number | string[] | undefined>): string {
  const search = new URLSearchParams()
  for (const [key, value] of Object.entries(params)) {
    if (value === undefined) continue
    if (Array.isArray(value)) {
      if (value.length > 0) search.set(key, value.join(','))
    } else {
      search.set(key, String(value))
    }
  }
  const qs = search.toString()
  return qs ? `?${qs}` : ''
}

export type FetchFn = typeof fetch

export class GatewayClient {
  private config: GatewayConfig
  private readonly fetchFn: FetchFn

  constructor(config: GatewayConfig, fetchFn: FetchFn = fetch) {
    this.config = { ...config, baseUrl: normalizeBaseUrl(config.baseUrl) }
    this.fetchFn = fetchFn
  }

  configure(config: Partial<GatewayConfig>): void {
    this.config = {
      baseUrl: config.baseUrl !== undefined ? normalizeBaseUrl(config.baseUrl) : this.config.baseUrl,
      token: config.token !== undefined ? config.token : this.config.token
    }
  }

  getConfig(): GatewayConfig {
    return { ...this.config }
  }

  private async request<T>(path: string, init?: RequestInit): Promise<T> {
    const url = `${this.config.baseUrl}${path}`
    let res: Response
    try {
      res = await this.fetchFn(url, {
        ...init,
        headers: {
          Authorization: `Bearer ${this.config.token}`,
          ...(init?.body ? { 'Content-Type': 'application/json' } : {}),
          ...init?.headers
        }
      })
    } catch (err) {
      throw new GatewayError(err instanceof Error ? err.message : 'network request failed', { kind: 'network' })
    }

    if (!res.ok) {
      let body: unknown
      try {
        body = await res.json()
      } catch {
        body = undefined
      }
      const message = extractMessage(body) ?? `${res.status} ${res.statusText}`
      throw new GatewayError(message, { kind: 'http', status: res.status, body })
    }

    if (res.status === 204) return undefined as T
    return (await res.json()) as T
  }

  ping(): Promise<PingResponse> {
    return this.request('/api/ping')
  }

  listPorts(): Promise<SerialPort[]> {
    return this.request('/api/ports')
  }

  listLinks(): Promise<Link[]> {
    return this.request('/api/links')
  }

  createLink(req: CreateLinkRequest): Promise<Link> {
    return this.request('/api/links', { method: 'POST', body: JSON.stringify(req) })
  }

  deleteLink(id: string): Promise<void> {
    return this.request(`/api/links/${encodeURIComponent(id)}`, { method: 'DELETE' })
  }

  listNodes(): Promise<NodeSummary[]> {
    return this.request('/api/nodes')
  }

  getNode(nodeId: number): Promise<NodeDetail> {
    return this.request(`/api/nodes/${nodeId}`)
  }

  sendCommand(nodeId: number, req: CommandRequest): Promise<CommandResult> {
    return this.request(`/api/nodes/${nodeId}/command`, { method: 'POST', body: JSON.stringify(req) })
  }

  getTelemetry(nodeId: number, query: TelemetryQuery = {}): Promise<TelemetryResponse> {
    const qs = buildQuery({
      since: query.since,
      until: query.until,
      channels: query.channels,
      max_points: query.max_points
    })
    return this.request(`/api/nodes/${nodeId}/telemetry${qs}`)
  }

  pushExperiment(nodeId: number, spec: ExperimentSpec): Promise<CommandResult> {
    return this.request(`/api/nodes/${nodeId}/experiment`, { method: 'POST', body: JSON.stringify(spec) })
  }

  getExperiments(): Promise<ExperimentSpec[]> {
    return this.request('/api/experiments')
  }

  putExperiments(specs: ExperimentSpec[]): Promise<ExperimentSpec[]> {
    return this.request('/api/experiments', { method: 'PUT', body: JSON.stringify(specs) })
  }

  validateExperiment(spec: ExperimentSpec): Promise<ExperimentValidationResult> {
    return this.request('/api/experiments/validate', { method: 'POST', body: JSON.stringify(spec) })
  }

  listRuns(): Promise<RunSummary[]> {
    return this.request('/api/runs')
  }

  getRun(id: string): Promise<RunSummary> {
    return this.request(`/api/runs/${encodeURIComponent(id)}`)
  }

  getRunSamples(id: string): Promise<RunSample[]> {
    return this.request(`/api/runs/${encodeURIComponent(id)}/samples`)
  }

  getTopology(): Promise<NetworkTopology> {
    return this.request('/api/network/topology')
  }

  listSimScenarios(): Promise<SimScenario[]> {
    return this.request('/api/sim/scenarios')
  }

  spawnSim(req: SimSpawnRequest = {}): Promise<Link[]> {
    return this.request('/api/sim/spawn', { method: 'POST', body: JSON.stringify(req) })
  }

  injectFault(req: SimFaultRequest): Promise<void> {
    return this.request('/api/sim/fault', { method: 'POST', body: JSON.stringify(req) })
  }

  /** WS URL for /ws/stream, token as a query param (never a header — browsers can't set WS headers). */
  streamUrl(): string {
    const wsBase = this.config.baseUrl.replace(/^http/, 'ws')
    return `${wsBase}/ws/stream?token=${encodeURIComponent(this.config.token)}`
  }
}

// ---------------------------------------------------------------------------
// Reconnecting WS client
// ---------------------------------------------------------------------------

export type ConnectionState = 'idle' | 'connecting' | 'open' | 'closed' | 'error'

/** Minimal WebSocket surface this module depends on — lets tests inject a fake. */
export interface WebSocketLike {
  onopen: ((this: WebSocketLike, ev: unknown) => void) | null
  onclose: ((this: WebSocketLike, ev: unknown) => void) | null
  onerror: ((this: WebSocketLike, ev: unknown) => void) | null
  onmessage: ((this: WebSocketLike, ev: { data: string }) => void) | null
  close: () => void
}

export type WebSocketFactory = (url: string) => WebSocketLike

const DEFAULT_BACKOFF_BASE_MS = 500
const DEFAULT_BACKOFF_MAX_MS = 15000

export interface GatewayStreamOptions {
  wsFactory?: WebSocketFactory
  setTimeoutFn?: (cb: () => void, ms: number) => ReturnType<typeof setTimeout>
  clearTimeoutFn?: (handle: ReturnType<typeof setTimeout>) => void
  backoffBaseMs?: number
  backoffMaxMs?: number
}

/**
 * Reconnecting client for /ws/stream. Backoff is a plain doubling sequence
 * (base, 2×base, 4×base, ... capped at max) with no jitter, chosen so
 * reconnect timing is deterministic and unit-testable (see
 * gatewayClient.test.ts) — jitter would be worth adding once many desktop
 * instances hammer one gateway simultaneously, not a concern at bench scale.
 */
export class GatewayStream {
  private readonly wsFactory: WebSocketFactory
  private readonly setTimeoutFn: (cb: () => void, ms: number) => ReturnType<typeof setTimeout>
  private readonly clearTimeoutFn: (handle: ReturnType<typeof setTimeout>) => void
  private readonly backoffBaseMs: number
  private readonly backoffMaxMs: number

  private socket: WebSocketLike | null = null
  private reconnectAttempt = 0
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null
  private state: ConnectionState = 'idle'
  private stoppedByUser = false

  private readonly messageListeners = new Set<(msg: StreamMessage) => void>()
  private readonly stateListeners = new Set<(state: ConnectionState) => void>()

  constructor(
    private readonly urlFn: () => string,
    opts: GatewayStreamOptions = {}
  ) {
    this.wsFactory =
      opts.wsFactory ??
      ((url: string) => new WebSocket(url) as unknown as WebSocketLike)
    this.setTimeoutFn = opts.setTimeoutFn ?? setTimeout
    this.clearTimeoutFn = opts.clearTimeoutFn ?? clearTimeout
    this.backoffBaseMs = opts.backoffBaseMs ?? DEFAULT_BACKOFF_BASE_MS
    this.backoffMaxMs = opts.backoffMaxMs ?? DEFAULT_BACKOFF_MAX_MS
  }

  getState(): ConnectionState {
    return this.state
  }

  onMessage(cb: (msg: StreamMessage) => void): () => void {
    this.messageListeners.add(cb)
    return () => this.messageListeners.delete(cb)
  }

  onStateChange(cb: (state: ConnectionState) => void): () => void {
    this.stateListeners.add(cb)
    return () => this.stateListeners.delete(cb)
  }

  connect(): void {
    this.stoppedByUser = false
    this.openSocket()
  }

  /** Backoff delay for the given attempt number (0-indexed), before any cap. */
  private delayForAttempt(attempt: number): number {
    const raw = this.backoffBaseMs * 2 ** attempt
    return Math.min(raw, this.backoffMaxMs)
  }

  private setState(state: ConnectionState): void {
    this.state = state
    for (const cb of this.stateListeners) cb(state)
  }

  private openSocket(): void {
    this.setState('connecting')
    let socket: WebSocketLike
    try {
      socket = this.wsFactory(this.urlFn())
    } catch {
      this.setState('error')
      this.scheduleReconnect()
      return
    }
    this.socket = socket

    socket.onopen = () => {
      this.reconnectAttempt = 0
      this.setState('open')
    }
    socket.onmessage = (ev) => {
      let parsed: StreamMessage
      try {
        parsed = JSON.parse(ev.data) as StreamMessage
      } catch {
        return // malformed frame — drop, don't crash the stream
      }
      for (const cb of this.messageListeners) cb(parsed)
    }
    socket.onerror = () => {
      this.setState('error')
    }
    socket.onclose = () => {
      this.socket = null
      if (this.stoppedByUser) {
        this.setState('closed')
        return
      }
      this.setState('closed')
      this.scheduleReconnect()
    }
  }

  private scheduleReconnect(): void {
    if (this.stoppedByUser) return
    const delay = this.delayForAttempt(this.reconnectAttempt)
    this.reconnectAttempt += 1
    this.reconnectTimer = this.setTimeoutFn(() => {
      this.reconnectTimer = null
      if (!this.stoppedByUser) this.openSocket()
    }, delay)
  }

  disconnect(): void {
    this.stoppedByUser = true
    if (this.reconnectTimer !== null) {
      this.clearTimeoutFn(this.reconnectTimer)
      this.reconnectTimer = null
    }
    this.socket?.close()
    this.socket = null
    this.setState('idle')
  }
}

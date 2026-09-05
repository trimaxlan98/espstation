/**
 * Types shared between the main and preload processes (and, transitively,
 * the renderer's typed view of the preload bridge — see
 * `renderer/src/lib/bridgeTypes.ts`). Nothing here describes the gateway's
 * HTTP/WS wire format: that contract lives entirely in
 * `renderer/src/lib/apiTypes.ts` because the renderer talks to the gateway
 * directly (fetch/WebSocket to localhost), never through IPC.
 */

export type ThemeSetting = 'light' | 'dark' | 'system'

/** How the desktop obtains a running gateway. */
export type GatewayMode = 'managed' | 'attach'

export interface Settings {
  /** Base URL of the espstation-gateway REST/WS API, e.g. http://127.0.0.1:8787 */
  gatewayUrl: string
  /** Bearer token sent as `?token=` on the WS and `Authorization: Bearer` on REST. */
  gatewayToken: string
  theme: ThemeSetting
  /** 'managed': the desktop spawns/supervises the gateway child process.
   *  'attach': the operator runs the gateway themselves; the desktop only connects. */
  gatewayMode: GatewayMode
  /** Overrides the interpreter used to launch the gateway in 'managed' mode.
   *  Empty string means "use the repo-relative gateway/.venv/bin/python default". */
  gatewayPythonPath: string
}

export type SettingsPatch = Partial<Settings>

export const DEFAULT_SETTINGS: Settings = {
  gatewayUrl: 'http://127.0.0.1:8787',
  gatewayToken: 'espstation-dev',
  theme: 'system',
  gatewayMode: 'managed',
  gatewayPythonPath: ''
}

/** Lifecycle of the main-process-supervised gateway child process. */
export type GatewayProcessState = 'stopped' | 'starting' | 'running' | 'stopping' | 'error'

export interface GatewayStatus {
  mode: GatewayMode
  state: GatewayProcessState
  pid: number | null
  startedAt: number | null
  /** Populated when state === 'error'. */
  error: string | null
}

export type GatewayLogLevel = 'info' | 'warn' | 'error'

export interface GatewayLogLine {
  ts: number
  /** 'main' is the supervisor's own commentary (spawn/exit/health); the rest is the child's own output. */
  stream: 'stdout' | 'stderr' | 'main'
  level: GatewayLogLevel
  message: string
}

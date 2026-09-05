/**
 * Gateway process supervisor (docs/ARCHITECTURE.md: "the station is an
 * instrument"). In `managed` mode this owns the espstation-gateway child
 * process — `gateway/.venv/bin/python -m espstation_gateway`, argv array,
 * never a shell string — relays its stdout/stderr as structured log lines,
 * and reports its lifecycle to the renderer. In `attach` mode the operator
 * runs the gateway themselves (or it's already running from a previous
 * session); this class does not spawn or own anything and simply reports
 * `state: 'stopped'` with `mode: 'attach'` — reachability is then the
 * renderer's job (it pings `/api/ping` directly, see lib/gatewayClient.ts).
 *
 * All I/O boundaries (child_process.spawn, filesystem existence checks) are
 * constructor-injectable so unit tests never spawn a real process — see
 * gatewaySupervisor.test.ts.
 */
import { existsSync } from 'node:fs'
import { spawn as nodeSpawn, type ChildProcess } from 'node:child_process'
import { join } from 'node:path'
import { app } from 'electron'
import type { GatewayLogLine, GatewayMode, GatewayStatus } from '@shared/types'

/** SIGKILL fallback grace period after SIGTERM. */
const KILL_GRACE_MS = 5000

export interface GatewayLaunchResolution {
  available: boolean
  pythonBin: string
  cwd: string
  /** Set only when `available` is false. */
  reason?: string
}

/**
 * Resolves the gateway's interpreter/cwd relative to the desktop app root:
 * dev repo layout is `<repoRoot>/gateway/.venv/bin/python`, repoRoot being
 * the parent of `desktop/`. `pythonPathOverride` (from Settings, or the
 * `ESPSTATION_GATEWAY_PYTHON` env var) takes precedence unconditionally —
 * same escape hatch PiStation's DSP/agent launchers use.
 */
export function resolveGatewayLaunch(pythonPathOverride?: string): GatewayLaunchResolution {
  const gatewayDir = join(app.getAppPath(), '..', 'gateway')
  const override = pythonPathOverride || process.env['ESPSTATION_GATEWAY_PYTHON']
  const pythonBin = override || join(gatewayDir, '.venv', 'bin', 'python')
  if (!existsSync(pythonBin)) {
    return {
      available: false,
      pythonBin,
      cwd: gatewayDir,
      reason:
        `gateway python interpreter not found at ${pythonBin}. Run, inside gateway/: ` +
        `'~/.local/bin/virtualenv .venv && .venv/bin/pip install -e ".[dev]"'.`
    }
  }
  return { available: true, pythonBin, cwd: gatewayDir }
}

export interface GatewaySupervisorDeps {
  emitLog: (line: GatewayLogLine) => void
  spawnFn?: typeof nodeSpawn
  resolveLaunchFn?: (pythonPathOverride?: string) => GatewayLaunchResolution
}

function nowTs(): number {
  return Date.now() / 1000
}

export class GatewaySupervisor {
  private readonly emitLog: (line: GatewayLogLine) => void
  private readonly spawnFn: typeof nodeSpawn
  private readonly resolveLaunchFn: (pythonPathOverride?: string) => GatewayLaunchResolution

  private child: ChildProcess | null = null
  private mode: GatewayMode = 'managed'
  private state: GatewayStatus['state'] = 'stopped'
  private startedAt: number | null = null
  private error: string | null = null
  /** Set while stop()/restart() owns the termination, so the exit handler doesn't report it as a crash. */
  private stopping = false

  constructor(deps: GatewaySupervisorDeps) {
    this.emitLog = deps.emitLog
    this.spawnFn = deps.spawnFn ?? nodeSpawn
    this.resolveLaunchFn = deps.resolveLaunchFn ?? resolveGatewayLaunch
  }

  status(): GatewayStatus {
    return {
      mode: this.mode,
      state: this.state,
      pid: this.child?.pid ?? null,
      startedAt: this.startedAt,
      error: this.error
    }
  }

  private log(level: GatewayLogLine['level'], message: string): void {
    this.emitLog({ ts: nowTs(), stream: 'main', level, message })
  }

  /** Starts (or, in `attach` mode, records) the gateway. Idempotent: a second start() while managed+running is a no-op. */
  start(mode: GatewayMode, pythonPathOverride: string): GatewayStatus {
    this.mode = mode

    if (mode === 'attach') {
      // Nothing to spawn — any previously-managed child is stopped first so
      // we never end up supervising a process while also claiming 'attach'.
      if (this.child) this.stop()
      this.state = 'stopped'
      this.error = null
      this.log('info', 'attach mode: expecting an already-running gateway, not spawning one')
      return this.status()
    }

    if (this.child && this.state === 'running') {
      return this.status() // already running under our supervision
    }

    const launch = this.resolveLaunchFn(pythonPathOverride)
    if (!launch.available) {
      this.state = 'error'
      this.error = launch.reason ?? 'gateway interpreter unavailable'
      this.log('error', this.error)
      return this.status()
    }

    this.state = 'starting'
    this.error = null
    this.log('info', `spawning ${launch.pythonBin} -m espstation_gateway (cwd=${launch.cwd})`)

    let child: ChildProcess
    try {
      child = this.spawnFn(launch.pythonBin, ['-m', 'espstation_gateway'], {
        cwd: launch.cwd,
        stdio: ['ignore', 'pipe', 'pipe']
      })
    } catch (err) {
      this.state = 'error'
      this.error = err instanceof Error ? err.message : String(err)
      this.log('error', `spawn failed: ${this.error}`)
      return this.status()
    }

    this.child = child
    this.stopping = false
    this.state = 'running'
    this.startedAt = nowTs()

    child.stdout?.on('data', (chunk: Buffer) => this.relay('stdout', chunk))
    child.stderr?.on('data', (chunk: Buffer) => this.relay('stderr', chunk))
    child.on('error', (err) => {
      this.log('error', `child process error: ${err.message}`)
      this.handleExit(null)
    })
    child.on('exit', (code) => {
      if (!this.stopping) {
        this.log(code === 0 ? 'warn' : 'error', `gateway exited unexpectedly (code ${code ?? 'unknown'})`)
      }
      this.handleExit(code)
    })

    this.log('info', `gateway running (pid ${child.pid ?? 'unknown'})`)
    return this.status()
  }

  private relay(stream: 'stdout' | 'stderr', chunk: Buffer): void {
    for (const rawLine of chunk.toString('utf8').split('\n')) {
      const line = rawLine.trim()
      if (!line) continue
      this.emitLog({ ts: nowTs(), stream, level: stream === 'stderr' ? 'warn' : 'info', message: line })
    }
  }

  private handleExit(code: number | null): void {
    this.child = null
    if (this.stopping) {
      this.state = 'stopped'
      this.error = null
    } else {
      this.state = code === 0 ? 'stopped' : 'error'
      this.error = code === 0 ? null : `gateway process exited with code ${code ?? 'unknown'}`
    }
    this.startedAt = null
  }

  /** SIGTERM (SIGKILL after `KILL_GRACE_MS` if still alive). No-op in `attach` mode or when nothing is running. */
  stop(): GatewayStatus {
    if (!this.child) {
      this.state = 'stopped'
      return this.status()
    }
    this.stopping = true
    this.state = 'stopping'
    this.log('info', 'stopping gateway')
    const child = this.child
    child.kill('SIGTERM')
    setTimeout(() => {
      if (this.child === child) child.kill('SIGKILL')
    }, KILL_GRACE_MS)
    return this.status()
  }

  restart(pythonPathOverride: string): GatewayStatus {
    this.stop()
    return this.start(this.mode, pythonPathOverride)
  }

  /** Called on app quit — waits for the child to actually exit before resolving. */
  async shutdown(): Promise<void> {
    if (!this.child) return
    await new Promise<void>((resolve) => {
      const child = this.child
      if (!child) {
        resolve()
        return
      }
      child.once('exit', () => resolve())
      this.stop()
    })
  }
}

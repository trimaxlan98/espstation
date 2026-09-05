import { EventEmitter } from 'node:events'
import { describe, expect, it, vi } from 'vitest'

// gatewaySupervisor.ts imports `app` from 'electron' (resolveGatewayLaunch's
// app.getAppPath()) purely to compute a default path — every test here
// injects its own resolveLaunchFn, so this mock only needs to make the
// module graph loadable, never gets exercised for real.
vi.mock('electron', () => ({ app: { getAppPath: () => '/fake/desktop' } }))

import { GatewaySupervisor } from './gatewaySupervisor'
import type { GatewayLogLine } from '@shared/types'

/** Minimal fake ChildProcess: an EventEmitter with stdout/stderr sub-emitters and a spy'd kill(). */
class FakeChild extends EventEmitter {
  pid = 4242
  stdout = new EventEmitter()
  stderr = new EventEmitter()
  kill = vi.fn((signal?: string) => {
    // Simulate the OS actually terminating the process on SIGTERM so tests
    // don't need a real timer to observe the exit transition.
    if (signal === 'SIGTERM' || signal === 'SIGKILL') {
      queueMicrotask(() => this.emit('exit', 0))
    }
    return true
  })
}

function collectLogs(): { logs: GatewayLogLine[]; emit: (line: GatewayLogLine) => void } {
  const logs: GatewayLogLine[] = []
  return { logs, emit: (line) => logs.push(line) }
}

describe('GatewaySupervisor', () => {
  it('reports an error status when the interpreter is unavailable', () => {
    const { logs, emit } = collectLogs()
    const sup = new GatewaySupervisor({
      emitLog: emit,
      resolveLaunchFn: () => ({ available: false, pythonBin: '/nope/python', cwd: '/nope', reason: 'missing venv' })
    })

    const status = sup.start('managed', '')

    expect(status.state).toBe('error')
    expect(status.error).toBe('missing venv')
    expect(logs.some((l) => l.level === 'error' && l.message.includes('missing venv'))).toBe(true)
  })

  it('spawns the gateway module as an argv array and reports running', () => {
    const { emit } = collectLogs()
    const fake = new FakeChild()
    const spawnFn = vi.fn(() => fake as unknown as ReturnType<typeof import('node:child_process').spawn>)
    const sup = new GatewaySupervisor({
      emitLog: emit,
      spawnFn: spawnFn as never,
      resolveLaunchFn: () => ({ available: true, pythonBin: '/repo/gateway/.venv/bin/python', cwd: '/repo/gateway' })
    })

    const status = sup.start('managed', '')

    expect(spawnFn).toHaveBeenCalledWith(
      '/repo/gateway/.venv/bin/python',
      ['-m', 'espstation_gateway'],
      expect.objectContaining({ cwd: '/repo/gateway' })
    )
    expect(status.state).toBe('running')
    expect(status.pid).toBe(4242)
    expect(status.mode).toBe('managed')
  })

  it('relays stdout/stderr chunks as structured log lines, splitting on newlines', () => {
    const { logs, emit } = collectLogs()
    const fake = new FakeChild()
    const sup = new GatewaySupervisor({
      emitLog: emit,
      spawnFn: (() => fake) as never,
      resolveLaunchFn: () => ({ available: true, pythonBin: 'python', cwd: '/repo/gateway' })
    })
    sup.start('managed', '')

    fake.stdout.emit('data', Buffer.from('line one\nline two\n'))
    fake.stderr.emit('data', Buffer.from('a warning\n'))

    const stdoutLines = logs.filter((l) => l.stream === 'stdout').map((l) => l.message)
    expect(stdoutLines).toEqual(['line one', 'line two'])
    expect(logs.some((l) => l.stream === 'stderr' && l.message === 'a warning')).toBe(true)
  })

  it('attach mode never spawns and reports stopped/attach', () => {
    const { emit } = collectLogs()
    const spawnFn = vi.fn()
    const sup = new GatewaySupervisor({
      emitLog: emit,
      spawnFn: spawnFn as never,
      resolveLaunchFn: () => ({ available: true, pythonBin: 'python', cwd: '/repo/gateway' })
    })

    const status = sup.start('attach', '')

    expect(spawnFn).not.toHaveBeenCalled()
    expect(status).toEqual({ mode: 'attach', state: 'stopped', pid: null, startedAt: null, error: null })
  })

  it('stop() sends SIGTERM and transitions back to stopped', async () => {
    const { emit } = collectLogs()
    const fake = new FakeChild()
    const sup = new GatewaySupervisor({
      emitLog: emit,
      spawnFn: (() => fake) as never,
      resolveLaunchFn: () => ({ available: true, pythonBin: 'python', cwd: '/repo/gateway' })
    })
    sup.start('managed', '')

    sup.stop()
    expect(fake.kill).toHaveBeenCalledWith('SIGTERM')
    // Let the queued microtask (fake OS exit) run.
    await Promise.resolve()
    await Promise.resolve()
    expect(sup.status().state).toBe('stopped')
  })

  it('an unexpected nonzero exit while running is reported as an error, not a clean stop', () => {
    const { logs, emit } = collectLogs()
    const fake = new FakeChild()
    const sup = new GatewaySupervisor({
      emitLog: emit,
      spawnFn: (() => fake) as never,
      resolveLaunchFn: () => ({ available: true, pythonBin: 'python', cwd: '/repo/gateway' })
    })
    sup.start('managed', '')

    fake.emit('exit', 1)

    expect(sup.status().state).toBe('error')
    expect(sup.status().error).toContain('code 1')
    expect(logs.some((l) => l.stream === 'main' && l.level === 'error')).toBe(true)
  })
})

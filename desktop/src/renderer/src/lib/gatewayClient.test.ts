import { describe, expect, it, vi } from 'vitest'
import { GatewayClient, GatewayError, GatewayStream } from './gatewayClient'
import type { WebSocketLike } from './gatewayClient'

function fakeFetch(response: { ok: boolean; status?: number; statusText?: string; json?: () => Promise<unknown> }) {
  return vi.fn().mockResolvedValue({
    ok: response.ok,
    status: response.status ?? (response.ok ? 200 : 500),
    statusText: response.statusText ?? '',
    json: response.json ?? (() => Promise.resolve({}))
  })
}

describe('GatewayClient — URL building and auth', () => {
  it('invokes the default browser fetch with its required global receiver', async () => {
    const fetchFn = vi.fn(function (this: unknown) {
      expect(this).toBe(globalThis)
      return Promise.resolve({
        ok: true,
        status: 200,
        statusText: '',
        json: () => Promise.resolve({ name: 'gw' })
      } as Response)
    })
    vi.stubGlobal('fetch', fetchFn)

    try {
      const client = new GatewayClient({ baseUrl: 'http://127.0.0.1:8787', token: 't' })
      await expect(client.ping()).resolves.toEqual({ name: 'gw' })
    } finally {
      vi.unstubAllGlobals()
    }
  })

  it('strips a trailing slash from baseUrl and joins the path cleanly', async () => {
    const fetchFn = fakeFetch({ ok: true, json: () => Promise.resolve({ name: 'gw' }) })
    const client = new GatewayClient({ baseUrl: 'http://127.0.0.1:8787/', token: 't' }, fetchFn as never)
    await client.ping()
    expect(fetchFn).toHaveBeenCalledWith('http://127.0.0.1:8787/api/ping', expect.anything())
  })

  it('sends the bearer token as an Authorization header', async () => {
    const fetchFn = fakeFetch({ ok: true, json: () => Promise.resolve([]) })
    const client = new GatewayClient({ baseUrl: 'http://127.0.0.1:8787', token: 'espstation-dev' }, fetchFn as never)
    await client.listNodes()
    const [, init] = fetchFn.mock.calls[0] as [string, RequestInit]
    expect((init.headers as Record<string, string>).Authorization).toBe('Bearer espstation-dev')
  })

  it('builds query strings for telemetry (since/until/channels/max_points), omitting undefined fields', async () => {
    const fetchFn = fakeFetch({ ok: true, json: () => Promise.resolve({ channels: {} }) })
    const client = new GatewayClient({ baseUrl: 'http://127.0.0.1:8787', token: 't' }, fetchFn as never)
    await client.getTelemetry(7, { since: 100, channels: ['sys.heap_free', 'adc.a0'] })
    const [url] = fetchFn.mock.calls[0] as [string]
    expect(url).toBe('http://127.0.0.1:8787/api/nodes/7/telemetry?since=100&channels=sys.heap_free%2Cadc.a0')
  })

  it('reconfigure() updates baseUrl/token used by subsequent requests', async () => {
    const fetchFn = fakeFetch({ ok: true, json: () => Promise.resolve({}) })
    const client = new GatewayClient({ baseUrl: 'http://127.0.0.1:8787', token: 'old' }, fetchFn as never)
    client.configure({ baseUrl: 'http://192.168.1.5:8787', token: 'new' })
    await client.ping()
    const [url, init] = fetchFn.mock.calls[0] as [string, RequestInit]
    expect(url).toBe('http://192.168.1.5:8787/api/ping')
    expect((init.headers as Record<string, string>).Authorization).toBe('Bearer new')
  })

  it('sets Content-Type: application/json only when a body is present', async () => {
    const fetchFn = fakeFetch({ ok: true, json: () => Promise.resolve([]) })
    const client = new GatewayClient({ baseUrl: 'http://127.0.0.1:8787', token: 't' }, fetchFn as never)
    await client.listNodes()
    const [, getInit] = fetchFn.mock.calls[0] as [string, RequestInit]
    expect((getInit.headers as Record<string, string>)['Content-Type']).toBeUndefined()

    await client.createLink({ kind: 'sim' })
    const [, postInit] = fetchFn.mock.calls[1] as [string, RequestInit]
    expect((postInit.headers as Record<string, string>)['Content-Type']).toBe('application/json')
  })
})

describe('GatewayClient — error mapping', () => {
  it('maps a network-level failure to a GatewayError with kind "network"', async () => {
    const fetchFn = vi.fn().mockRejectedValue(new TypeError('fetch failed'))
    const client = new GatewayClient({ baseUrl: 'http://127.0.0.1:8787', token: 't' }, fetchFn as never)
    await expect(client.ping()).rejects.toMatchObject({ kind: 'network' })
    await expect(client.ping()).rejects.toBeInstanceOf(GatewayError)
  })

  it('maps a non-2xx response to a GatewayError carrying status and body', async () => {
    const fetchFn = fakeFetch({ ok: false, status: 404, statusText: 'Not Found', json: () => Promise.resolve({ message: 'no such node' }) })
    const client = new GatewayClient({ baseUrl: 'http://127.0.0.1:8787', token: 't' }, fetchFn as never)
    await expect(client.getNode(999)).rejects.toMatchObject({
      kind: 'http',
      status: 404,
      message: 'no such node'
    })
    await expect(client.getNode(999)).rejects.toBeInstanceOf(GatewayError)
  })

  it('falls back to a generic status message when the error body has no message field', async () => {
    const fetchFn = fakeFetch({ ok: false, status: 500, statusText: 'Internal Server Error', json: () => Promise.reject(new Error('no body')) })
    const client = new GatewayClient({ baseUrl: 'http://127.0.0.1:8787', token: 't' }, fetchFn as never)
    await expect(client.ping()).rejects.toMatchObject({ message: '500 Internal Server Error' })
  })
})

describe('GatewayStream — reconnect/backoff', () => {
  class FakeSocket implements WebSocketLike {
    onopen: WebSocketLike['onopen'] = null
    onclose: WebSocketLike['onclose'] = null
    onerror: WebSocketLike['onerror'] = null
    onmessage: WebSocketLike['onmessage'] = null
    closed = false
    close(): void {
      this.closed = true
      this.onclose?.call(this, {})
    }
  }

  function harness(): {
    sockets: FakeSocket[]
    scheduled: Array<{ cb: () => void; ms: number }>
    stream: GatewayStream
    runNextTimer: () => void
  } {
    const sockets: FakeSocket[] = []
    const scheduled: Array<{ cb: () => void; ms: number }> = []
    const stream = new GatewayStream(() => 'ws://127.0.0.1:8787/ws/stream?token=t', {
      wsFactory: () => {
        const s = new FakeSocket()
        sockets.push(s)
        return s
      },
      setTimeoutFn: (cb, ms) => {
        const handle = { cb, ms } as unknown as ReturnType<typeof setTimeout>
        scheduled.push({ cb, ms })
        return handle
      },
      clearTimeoutFn: () => {
        // tests that cancel don't inspect `scheduled` afterward
      },
      backoffBaseMs: 100,
      backoffMaxMs: 800
    })
    return {
      sockets,
      scheduled,
      stream,
      runNextTimer: () => {
        const next = scheduled.shift()
        next?.cb()
      }
    }
  }

  it('starts in "connecting" then moves to "open" once the socket opens', () => {
    const { stream, sockets } = harness()
    const states: string[] = []
    stream.onStateChange((s) => states.push(s))
    stream.connect()
    expect(states).toEqual(['connecting'])
    sockets[0]?.onopen?.call(sockets[0], {})
    expect(states).toEqual(['connecting', 'open'])
  })

  it('reconnects with doubling backoff after an unexpected close, capped at backoffMaxMs', () => {
    const { stream, sockets, scheduled } = harness()
    stream.connect()
    sockets[0]?.onclose?.call(sockets[0], {}) // never opened — closes immediately
    expect(scheduled.map((s) => s.ms)).toEqual([100])

    scheduled[0]?.cb()
    sockets[1]?.onclose?.call(sockets[1], {})
    expect(scheduled.map((s) => s.ms)).toEqual([100, 200])

    scheduled[1]?.cb()
    sockets[2]?.onclose?.call(sockets[2], {})
    expect(scheduled.map((s) => s.ms)).toEqual([100, 200, 400])

    scheduled[2]?.cb()
    sockets[3]?.onclose?.call(sockets[3], {})
    expect(scheduled.map((s) => s.ms)).toEqual([100, 200, 400, 800])

    scheduled[3]?.cb()
    sockets[4]?.onclose?.call(sockets[4], {})
    // Capped: stays at 800, does not keep doubling to 1600.
    expect(scheduled.map((s) => s.ms)).toEqual([100, 200, 400, 800, 800])
  })

  it('resets the backoff attempt counter after a successful open', () => {
    const { stream, sockets, scheduled } = harness()
    stream.connect()
    sockets[0]?.onclose?.call(sockets[0], {}) // fail #1
    expect(scheduled.map((s) => s.ms)).toEqual([100])
    scheduled[0]?.cb()
    sockets[1]?.onopen?.call(sockets[1], {}) // recovers
    sockets[1]?.onclose?.call(sockets[1], {}) // fails again, should restart from base
    expect(scheduled.map((s) => s.ms)).toEqual([100, 100])
  })

  it('disconnect() suppresses further reconnect attempts and sets state to idle', () => {
    const { stream, sockets, scheduled } = harness()
    stream.connect()
    sockets[0]?.onopen?.call(sockets[0], {})
    stream.disconnect()
    expect(stream.getState()).toBe('idle')
    expect(scheduled.length).toBe(0)
  })

  it('dispatches parsed telemetry messages to listeners and ignores malformed frames', () => {
    const { stream, sockets } = harness()
    const received: unknown[] = []
    stream.onMessage((m) => received.push(m))
    stream.connect()
    sockets[0]?.onmessage?.call(sockets[0], { data: JSON.stringify({ kind: 'telemetry', node_id: 1, ts: 1.5, data: {} }) })
    sockets[0]?.onmessage?.call(sockets[0], { data: 'not json' })
    expect(received).toEqual([{ kind: 'telemetry', node_id: 1, ts: 1.5, data: {} }])
  })
})

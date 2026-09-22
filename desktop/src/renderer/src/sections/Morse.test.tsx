// @vitest-environment jsdom

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { cleanup, render, screen } from '@testing-library/react'
import { useNodesStore } from '../store/nodesStore'
import { useStreamStore } from '../store/streamStore'
import type { NodeDetail, NodeSummary } from '../lib/apiTypes'

// Same treatment as Nodes.test.tsx: the section's mount effect calls load(),
// which goes through this singleton. Nothing here touches a real gateway.
vi.mock('../lib/gatewaySingleton', () => ({
  gatewayClient: {
    listNodes: vi.fn().mockResolvedValue([]),
    getNode: vi.fn().mockResolvedValue(null)
  },
  gatewayStream: {
    onStateChange: vi.fn(() => () => undefined),
    onMessage: vi.fn(() => () => undefined),
    connect: vi.fn(),
    disconnect: vi.fn(),
    getState: vi.fn(() => 'idle')
  }
}))

import { gatewayClient } from '../lib/gatewaySingleton'
import { Morse, isMorseNode } from './Morse'

function node(id: number, label: string, fw = 'bench-morse-duplex'): NodeSummary {
  return {
    node_id: id,
    label,
    mac: `02:00:00:00:00:${id.toString(16).padStart(2, '0')}`,
    state: 'running',
    online: true,
    last_seen: Date.now() / 1000,
    uptime_ms: 1000,
    heap_free: 0,
    rssi: 0,
    fw,
    target: 'esp32',
    link_id: `morse-${id}`
  }
}

/** A node detail whose NDB carries the morse.* channels, like esps_morse. */
function morseDetail(n: NodeSummary): NodeDetail {
  return {
    ...n,
    caps: ['telemetry'],
    boot: { count: 1, reason: 'power_on', uptime_ms: 1000 },
    ndb: [
      { id: 3, key: 'sys.uptime', name: 'Uptime', unit: 's', type: 'u32', rate_hz: 0.2, group: 'system' },
      { id: 22, key: 'morse.tx', name: 'Morse TX', unit: '', type: 'u8', rate_hz: 0, group: 'morse' }
    ]
  }
}

/**
 * The decoded-text headline of one card.
 *
 * Scoped, not a bare getByText: a decoded letter now appears TWICE on a card
 * — once in this headline and once under the pulse group that produced it, in
 * the wave. Both are wanted; only this one is the running transcript.
 */
function decoded(label: string): string {
  return screen.getByLabelText(`Text received by ${label}`).textContent ?? ''
}

/**
 * Every SVG <title> in the tree. getByTitle only looks at `[title]` and at a
 * <title> that is a direct child of <svg>, and these hang off the pulse they
 * describe, which is the whole point of them.
 */
function titles(container: HTMLElement): string[] {
  return [...container.querySelectorAll('title')].map((t) => t.textContent ?? '')
}

function plainDetail(n: NodeSummary): NodeDetail {
  const d = morseDetail(n)
  return { ...d, ndb: d.ndb.filter((c) => !c.key.startsWith('morse.')) }
}

beforeEach(() => {
  useNodesStore.setState({ nodes: [], activeNodeId: null, detailByNode: new Map() })
  useStreamStore.getState().reset()
  // clearAllMocks() keeps implementations, so restore the default explicitly:
  // otherwise one test's detail leaks into the next one's fleet.
  vi.mocked(gatewayClient.getNode).mockResolvedValue(null as unknown as NodeDetail)
  vi.mocked(gatewayClient.listNodes).mockResolvedValue([])
})

afterEach(() => {
  cleanup()
  vi.clearAllMocks()
})

describe('isMorseNode', () => {
  it('recognises a board by the firmware string the adapter announces', () => {
    expect(isMorseNode(node(1, 'p1'))).toBe(true)
  })

  it('does not claim a real espstation-fw node', () => {
    expect(isMorseNode(node(2, 'real', '0.1.0'))).toBe(false)
  })

  it('recognises an esps_morse board by its NDB, which is all it has', () => {
    const n = node(3, 'real-morse', '0.1.0')
    expect(isMorseNode(n, morseDetail(n))).toBe(true)
  })

  it('is not fooled by a detail that is missing, null or has no morse channels', () => {
    const n = node(4, 'plain', '0.1.0')
    expect(isMorseNode(n, null)).toBe(false)
    expect(isMorseNode(n, undefined)).toBe(false)
    expect(isMorseNode(n, plainDetail(n))).toBe(false)
  })
})

describe('Morse section', () => {
  it('explains how to attach one when no station is present', () => {
    render(<Morse />)
    expect(screen.getByText('No Morse station attached')).toBeTruthy()
    // the empty state must name the zero-hardware route, not just the board one
    expect(screen.getByText(/morse-replay/)).toBeTruthy()
  })

  it('shows what each station received, from its RX letter events only', () => {
    useNodesStore.setState({ nodes: [node(1, 'P1'), node(2, 'P2')] })
    const ingest = useStreamStore.getState().ingestEvent
    const ev = (nodeId: number, code: string, data: Record<string, unknown>) =>
      ingest({ node_id: nodeId, ts: 1, code, severity: 'info', data })
    ev(1, 'morse.letter', { dir: 'RX', letter: 'S' })
    ev(1, 'morse.letter', { dir: 'RX', letter: 'O' })
    ev(1, 'morse.letter', { dir: 'TX', letter: 'X' }) // own echo: must NOT appear
    ev(2, 'morse.letter', { dir: 'RX', letter: 'E' })

    render(<Morse />)
    expect(decoded('P1')).toBe('SO')
    expect(decoded('P2')).toBe('E')
  })

  it('ignores letters with no direction at all rather than guessing RX', () => {
    // The sketch adapter stamps dir "?" when the line carried no sentido, and
    // an event can reach the rail with no data at all.
    useNodesStore.setState({ nodes: [node(1, 'P1')] })
    const ingest = useStreamStore.getState().ingestEvent
    ingest({ node_id: 1, ts: 1, code: 'morse.letter', severity: 'info', data: { dir: 'RX', letter: 'K' } })
    ingest({ node_id: 1, ts: 2, code: 'morse.letter', severity: 'info', data: { dir: '?', letter: 'Z' } })
    ingest({ node_id: 1, ts: 3, code: 'morse.letter', severity: 'info' })
    render(<Morse />)
    expect(decoded('P1')).toBe('K')
  })

  it('never renders a letter that is not a letter', () => {
    useNodesStore.setState({ nodes: [node(1, 'P1')] })
    const ingest = useStreamStore.getState().ingestEvent
    ingest({ node_id: 1, ts: 1, code: 'morse.letter', severity: 'info', data: { dir: 'RX', letter: { a: 1 } } })
    ingest({ node_id: 1, ts: 2, code: 'morse.letter', severity: 'info', data: { dir: 'RX', letter: 'R' } })
    render(<Morse />)
    expect(screen.queryByText(/object Object/)).toBeNull()
    expect(decoded('P1')).toBe('R')
  })

  it('keeps word gaps between letters and drops the leading and repeated ones', () => {
    useNodesStore.setState({ nodes: [node(1, 'P1')] })
    const ingest = useStreamStore.getState().ingestEvent
    const ev = (ts: number, code: string, data: Record<string, unknown>) =>
      ingest({ node_id: 1, ts, code, severity: 'info', data })
    ev(1, 'morse.word', { dir: 'RX' })
    ev(2, 'morse.letter', { dir: 'RX', letter: 'O' })
    ev(3, 'morse.word', { dir: 'RX' })
    ev(4, 'morse.word', { dir: 'RX' })
    ev(5, 'morse.letter', { dir: 'RX', letter: 'K' })
    render(<Morse />)
    expect(decoded('P1')).toBe('O K')
  })

  it('marks an unknown code rather than dropping it', () => {
    useNodesStore.setState({ nodes: [node(1, 'P1')] })
    useStreamStore.getState().ingestEvent({
      node_id: 1, ts: 1, code: 'morse.unknown', severity: 'warning', data: { dir: 'RX', code: '......' }
    })
    render(<Morse />)
    expect(screen.getByText('¿')).toBeTruthy()
  })

  it('reports the cross-check as matching when both directions carried the same count', () => {
    useNodesStore.setState({ nodes: [node(1, 'P1'), node(2, 'P2')] })
    const t = useStreamStore.getState().ingestTelemetry
    t(1, 'morse.symbols', [1, 9])
    t(2, 'morse.symbols', [1, 9])
    render(<Morse />)
    expect(screen.getByText(/Both directions carried the same count/)).toBeTruthy()
    // the verdict must survive being printed in black and white
    expect(screen.getByText('Match.')).toBeTruthy()
  })

  it('reports a mismatch instead of hiding it', () => {
    useNodesStore.setState({ nodes: [node(1, 'P1'), node(2, 'P2')] })
    const t = useStreamStore.getState().ingestTelemetry
    t(1, 'morse.symbols', [1, 9])
    t(2, 'morse.symbols', [1, 7])
    render(<Morse />)
    expect(screen.getByText(/They differ/)).toBeTruthy()
    expect(screen.getByText('Mismatch.')).toBeTruthy()
  })

  it('waits for both counters before comparing anything', () => {
    useNodesStore.setState({ nodes: [node(1, 'P1'), node(2, 'P2')] })
    useStreamStore.getState().ingestTelemetry(1, 'morse.symbols', [1, 9])
    render(<Morse />)
    expect(screen.getByText(/have reported their counters/)).toBeTruthy()
    expect(screen.queryByText('Match.')).toBeNull()
    expect(screen.queryByText('Mismatch.')).toBeNull()
  })

  it('refuses to cross-check three boards instead of comparing the first two', () => {
    useNodesStore.setState({ nodes: [node(1, 'P1'), node(2, 'P2'), node(3, 'P3')] })
    const t = useStreamStore.getState().ingestTelemetry
    t(1, 'morse.symbols', [1, 9])
    t(2, 'morse.symbols', [1, 9])
    t(3, 'morse.symbols', [1, 4])
    render(<Morse />)
    expect(screen.getByText(/3 Morse boards are here/)).toBeTruthy()
    expect(screen.queryByText('Match.')).toBeNull()
    // all three still get a card; it is only the pair verdict that is refused
    expect(screen.getByText('P3 — receiving')).toBeTruthy()
  })

  it('picks the pair from the boards that are online, not from the registry', () => {
    // A gateway registry keeps every node it ever saw. Three dead ones from
    // earlier sessions must not stop the two on the bench being compared.
    useNodesStore.setState({
      nodes: [
        { ...node(1, 'OLD-A'), online: false },
        { ...node(2, 'OLD-B'), online: false },
        { ...node(3, 'OLD-C'), online: false },
        node(4, 'LIVE-1'),
        node(5, 'LIVE-2')
      ]
    })
    const t = useStreamStore.getState().ingestTelemetry
    t(4, 'morse.symbols', [1, 9])
    t(5, 'morse.symbols', [1, 9])
    render(<Morse />)
    expect(screen.getByText('Match.')).toBeTruthy()
    expect(screen.getByText(/LIVE-1 received/)).toBeTruthy()
  })

  it('puts the online boards above the ones that are only remembered', () => {
    useNodesStore.setState({ nodes: [{ ...node(1, 'OLD'), online: false }, node(2, 'LIVE')] })
    render(<Morse />)
    const cards = screen.getAllByText(/— receiving$/).map((el) => el.textContent)
    expect(cards).toEqual(['LIVE — receiving', 'OLD — receiving'])
  })

  it('still gives a verdict for a session where everything has gone offline', () => {
    useNodesStore.setState({
      nodes: [{ ...node(1, 'P1'), online: false }, { ...node(2, 'P2'), online: false }]
    })
    const t = useStreamStore.getState().ingestTelemetry
    t(1, 'morse.symbols', [1, 9])
    t(2, 'morse.symbols', [1, 7])
    render(<Morse />)
    expect(screen.getByText('Mismatch.')).toBeTruthy()
  })

  it('draws a station with no telemetry at all without inventing numbers', () => {
    useNodesStore.setState({ nodes: [node(1, 'P1')] })
    render(<Morse />)
    expect(screen.getAllByText('—').length).toBe(6)
    expect(screen.getByLabelText(/P1: no symbols keyed yet/)).toBeTruthy()
  })

  it('draws the wave from symbol events, not from the 1 Hz level channels', () => {
    useNodesStore.setState({ nodes: [node(1, 'P1')] })
    const ingest = useStreamStore.getState().ingestEvent
    // A level channel sampled once a second cannot see a 120 ms dot; these
    // samples exist precisely so the assertion below proves they are not
    // what the drawing is made of.
    const t = useStreamStore.getState().ingestTelemetry
    t(1, 'morse.tx', [1, 1])
    t(1, 'morse.rx', [1, 1])
    const { container } = render(<Morse />)
    expect(container.querySelectorAll('.morse-wave__fill').length).toBe(0)

    cleanup()
    ingest({ node_id: 1, ts: 10, code: 'morse.symbol', severity: 'debug', data: { dir: 'RX', symbol: '.', ms: 120 } })
    ingest({ node_id: 1, ts: 11, code: 'morse.symbol', severity: 'debug', data: { dir: 'RX', symbol: '-', ms: 380 } })
    const second = render(<Morse />)
    expect(second.container.querySelectorAll('.morse-wave__fill--dot').length).toBe(1)
    expect(second.container.querySelectorAll('.morse-wave__fill--dash').length).toBe(1)
  })

  it('says dot, dash and the duration in words, not only in colour', () => {
    useNodesStore.setState({ nodes: [node(1, 'P1')] })
    useStreamStore.getState().ingestEvent({
      node_id: 1, ts: 10, code: 'morse.symbol', severity: 'debug', data: { dir: 'TX', symbol: '-', ms: 380 }
    })
    // <title> on the pulse, which is what a browser shows on hover and what
    // a screen reader reads: the fill colour is never the only statement.
    const { container } = render(<Morse />)
    expect(titles(container)).toContain('P1 TX: dash, 380 ms')
  })

  it('keeps the two hands apart: the echo lands in TX and the far end in RX', () => {
    useNodesStore.setState({ nodes: [node(1, 'P1')] })
    const ingest = useStreamStore.getState().ingestEvent
    ingest({ node_id: 1, ts: 10, code: 'morse.symbol', severity: 'debug', data: { dir: 'TX', symbol: '.', ms: 90 } })
    ingest({ node_id: 1, ts: 11, code: 'morse.symbol', severity: 'debug', data: { dir: 'RX', symbol: '.', ms: 110 } })
    const { container } = render(<Morse />)
    expect(titles(container)).toContain('P1 TX: dot, 90 ms')
    expect(titles(container)).toContain('P1 RX: dot, 110 ms')
  })

  it('measures cadence from the operator’s own key, never from the incoming line', () => {
    useNodesStore.setState({ nodes: [node(1, 'P1')] })
    const ingest = useStreamStore.getState().ingestEvent
    // A 100 ms dot on TX is 12 wpm. The RX pulses are deliberately a wildly
    // different rhythm: if the panel ever read the wrong lane, the number
    // below would move.
    ingest({ node_id: 1, ts: 10, code: 'morse.symbol', severity: 'debug', data: { dir: 'TX', symbol: '.', ms: 100 } })
    ingest({ node_id: 1, ts: 11, code: 'morse.symbol', severity: 'debug', data: { dir: 'TX', symbol: '-', ms: 320 } })
    ingest({ node_id: 1, ts: 12, code: 'morse.symbol', severity: 'debug', data: { dir: 'RX', symbol: '.', ms: 400 } })
    ingest({ node_id: 1, ts: 13, code: 'morse.symbol', severity: 'debug', data: { dir: 'RX', symbol: '-', ms: 900 } })
    render(<Morse />)
    expect(screen.getByText('12 wpm')).toBeTruthy()
    expect(screen.getByText(/Separation 220 ms — comfortable/)).toBeTruthy()
  })

  it('says the dot and dash bands overlap instead of suggesting a threshold', () => {
    useNodesStore.setState({ nodes: [node(1, 'P1')] })
    const ingest = useStreamStore.getState().ingestEvent
    ingest({ node_id: 1, ts: 10, code: 'morse.symbol', severity: 'debug', data: { dir: 'TX', symbol: '.', ms: 260 } })
    ingest({ node_id: 1, ts: 11, code: 'morse.symbol', severity: 'debug', data: { dir: 'TX', symbol: '-', ms: 240 } })
    render(<Morse />)
    expect(screen.getByText(/Dots and dashes overlap/)).toBeTruthy()
    expect(screen.queryByText(/has the most room either side/)).toBeNull()
  })

  it('waits for a hand of its own before claiming a cadence', () => {
    useNodesStore.setState({ nodes: [node(1, 'P1')] })
    useStreamStore.getState().ingestEvent({
      node_id: 1, ts: 10, code: 'morse.symbol', severity: 'debug', data: { dir: 'RX', symbol: '.', ms: 100 }
    })
    render(<Morse />)
    expect(screen.getByText(/Cadence appears once P1 has keyed a few symbols/)).toBeTruthy()
  })

  it('does not paint NaNs when the firmware reported a pulse with no duration', () => {
    // `ms` is omitted when it is zero, so this shape is real.
    useNodesStore.setState({ nodes: [node(1, 'P1')] })
    const ingest = useStreamStore.getState().ingestEvent
    ingest({ node_id: 1, ts: 10, code: 'morse.symbol', severity: 'debug', data: { dir: 'TX', symbol: '.' } })
    ingest({ node_id: 1, ts: 11, code: 'morse.symbol', severity: 'debug', data: { dir: 'TX', symbol: '-' } })
    const { container } = render(<Morse />)
    expect(container.innerHTML).not.toMatch(/NaN/)
  })

  it('offers the sketch even when there is no board to attach it to', () => {
    render(<Morse />)
    expect(screen.getByText('Get the .ino sketch')).toBeTruthy()
  })

  it('says a board is offline instead of passing stale counters off as live', () => {
    useNodesStore.setState({ nodes: [{ ...node(1, 'P1'), online: false }] })
    useStreamStore.getState().ingestTelemetry(1, 'morse.symbols', [1, 9])
    render(<Morse />)
    expect(screen.getByText(/Offline/)).toBeTruthy()
  })

  it('finds an esps_morse board by its NDB when the fw string says nothing', async () => {
    // The whole path, not a shortcut: the fleet arrives from the mount's
    // load(), the section sees a node it cannot classify, asks for its
    // detail, and only the NDB tells it this is a Morse station.
    const n = node(7, 'REAL', '0.1.0')
    vi.mocked(gatewayClient.listNodes).mockResolvedValue([n])
    vi.mocked(gatewayClient.getNode).mockResolvedValue(morseDetail(n))
    render(<Morse />)
    expect(await screen.findByText('REAL — receiving')).toBeTruthy()
    expect(vi.mocked(gatewayClient.getNode)).toHaveBeenCalledWith(7)
  })

  it('does not ask a board that already named itself for its detail', async () => {
    vi.mocked(gatewayClient.listNodes).mockResolvedValue([node(1, 'P1')])
    render(<Morse />)
    expect(await screen.findByText('P1 — receiving')).toBeTruthy()
    expect(vi.mocked(gatewayClient.getNode)).not.toHaveBeenCalled()
  })

  it('says the counts are pulses, not letters', () => {
    useNodesStore.setState({ nodes: [node(1, 'P1')] })
    render(<Morse />)
    expect(screen.getByText(/pulses, not letters/)).toBeTruthy()
  })
})

// @vitest-environment jsdom

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { cleanup, render, screen } from '@testing-library/react'
import { useNodesStore } from '../store/nodesStore'
import { useStreamStore } from '../store/streamStore'
import type { NodeSummary } from '../lib/apiTypes'

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

beforeEach(() => {
  useNodesStore.setState({ nodes: [], activeNodeId: null, detailByNode: new Map() })
  useStreamStore.getState().reset()
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
    expect(screen.getByText('SO')).toBeTruthy()
    expect(screen.getByText('E')).toBeTruthy()
    expect(screen.queryByText('SOX')).toBeNull()
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
  })

  it('reports a mismatch instead of hiding it', () => {
    useNodesStore.setState({ nodes: [node(1, 'P1'), node(2, 'P2')] })
    const t = useStreamStore.getState().ingestTelemetry
    t(1, 'morse.symbols', [1, 9])
    t(2, 'morse.symbols', [1, 7])
    render(<Morse />)
    expect(screen.getByText(/They differ/)).toBeTruthy()
  })

  it('says the counts are pulses, not letters', () => {
    useNodesStore.setState({ nodes: [node(1, 'P1')] })
    render(<Morse />)
    expect(screen.getByText(/pulses, not letters/)).toBeTruthy()
  })
})

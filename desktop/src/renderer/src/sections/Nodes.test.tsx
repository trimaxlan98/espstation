// @vitest-environment jsdom

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { cleanup, render, screen } from '@testing-library/react'
import { ToastProvider } from '../components/ui'
import { useNodesStore } from '../store/nodesStore'
import type { NodeSummary } from '../lib/apiTypes'

// Nodes.tsx (and, transitively, nodesStore.ts) talk to the gateway through
// this singleton — replace it wholesale so the render test never touches
// fetch/WebSocket. Only the methods Nodes.tsx's mount effects call are
// exercised; everything resolves immediately with an empty/default result.
vi.mock('../lib/gatewaySingleton', () => ({
  gatewayClient: {
    listLinks: vi.fn().mockResolvedValue([]),
    listPorts: vi.fn().mockResolvedValue([]),
    listNodes: vi.fn().mockResolvedValue([]),
    getNode: vi.fn().mockResolvedValue(null),
    sendCommand: vi.fn(),
    createLink: vi.fn(),
    deleteLink: vi.fn(),
    spawnSim: vi.fn()
  },
  gatewayStream: {
    onStateChange: vi.fn(() => () => undefined),
    onMessage: vi.fn(() => () => undefined),
    connect: vi.fn(),
    disconnect: vi.fn(),
    getState: vi.fn(() => 'idle')
  }
}))

import { Nodes } from './Nodes'

const ONLINE_NODE: NodeSummary = {
  node_id: 1,
  label: 'bench-a',
  mac: '24:6f:28:aa:bb:cc',
  state: 'running',
  online: true,
  last_seen: Date.now() / 1000,
  uptime_ms: 3_723_000,
  heap_free: 182_304,
  rssi: -61,
  fw: '0.1.0',
  target: 'esp32',
  link_id: 'l-1'
}

const OFFLINE_NODE: NodeSummary = {
  ...ONLINE_NODE,
  node_id: 2,
  label: 'bench-b',
  state: 'boot',
  online: false,
  link_id: null
}

function resetNodesStore(): void {
  useNodesStore.setState({
    nodes: [],
    activeNodeId: null,
    detailByNode: new Map(),
    loading: false,
    error: null
  })
}

beforeEach(() => {
  resetNodesStore()
})

afterEach(() => {
  cleanup()
})

function renderNodes(): ReturnType<typeof render> {
  return render(
    <ToastProvider>
      <Nodes />
    </ToastProvider>
  )
}

describe('Nodes section', () => {
  it('renders an empty-fleet state when there are no nodes', () => {
    renderNodes()
    expect(screen.getByText('No nodes yet')).toBeInTheDocument()
  })

  it('renders an online node with its running-state badge and label', () => {
    useNodesStore.setState({ nodes: [ONLINE_NODE], activeNodeId: 1 })
    renderNodes()
    expect(screen.getByText('bench-a')).toBeInTheDocument()
    expect(screen.getAllByText('running').length).toBeGreaterThan(0)
  })

  it('renders an offline node dimmed with an "offline" badge, not its stale state', () => {
    useNodesStore.setState({ nodes: [OFFLINE_NODE], activeNodeId: null })
    renderNodes()
    expect(screen.getByText('bench-b')).toBeInTheDocument()
    expect(screen.getByText('offline')).toBeInTheDocument()
    expect(screen.getByText('bench-b').className).toContain('node-row__label--offline')
  })

  it('shows a spinner while the fleet is loading', () => {
    useNodesStore.setState({ loading: true })
    renderNodes()
    expect(screen.getByRole('status')).toBeInTheDocument()
  })
})

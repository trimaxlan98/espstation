import { create } from 'zustand'
import { gatewayClient } from '../lib/gatewaySingleton'
import type { NodeDetail, NodeSummary } from '../lib/apiTypes'

interface NodesState {
  nodes: NodeSummary[]
  activeNodeId: number | null
  detailByNode: Map<number, NodeDetail>
  loading: boolean
  error: string | null
  load: () => Promise<void>
  selectNode: (nodeId: number | null) => void
  loadDetail: (nodeId: number) => Promise<void>
  /** Applies a WS `kind: "node"` push without waiting for the next poll — see lib/streamPipeline.ts. */
  upsertFromStream: (summary: NodeSummary) => void
}

export const useNodesStore = create<NodesState>((set) => ({
  nodes: [],
  activeNodeId: null,
  detailByNode: new Map(),
  loading: false,
  error: null,

  load: async () => {
    set({ loading: true })
    try {
      const nodes = await gatewayClient.listNodes()
      set((s) => ({
        nodes,
        loading: false,
        error: null,
        // Default to the first node once one exists and nothing is selected yet.
        activeNodeId: s.activeNodeId ?? (nodes.length > 0 ? (nodes[0]?.node_id ?? null) : null)
      }))
    } catch (err) {
      set({ loading: false, error: err instanceof Error ? err.message : String(err) })
    }
  },

  selectNode: (nodeId) => set({ activeNodeId: nodeId }),

  loadDetail: async (nodeId) => {
    const detail = await gatewayClient.getNode(nodeId)
    set((s) => {
      const next = new Map(s.detailByNode)
      next.set(nodeId, detail)
      return { detailByNode: next }
    })
  },

  upsertFromStream: (summary) => {
    set((s) => {
      const idx = s.nodes.findIndex((n) => n.node_id === summary.node_id)
      const nodes = idx === -1 ? [...s.nodes, summary] : s.nodes.map((n, i) => (i === idx ? summary : n))
      return { nodes, activeNodeId: s.activeNodeId ?? summary.node_id }
    })
  }
}))

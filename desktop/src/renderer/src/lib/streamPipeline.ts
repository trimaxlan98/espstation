/**
 * App-wide WS ingestion: connects the shared GatewayStream once, fans each
 * message out to the store it belongs in. Runs independent of which section
 * is mounted (App.tsx effect) so telemetry keeps accumulating in the Live
 * ring buffers even while looking at, say, Settings.
 */
import { gatewayStream } from './gatewaySingleton'
import { useConnectionStore } from '../store/connectionStore'
import { useNodesStore } from '../store/nodesStore'
import { useStreamStore } from '../store/streamStore'
import type { EventStreamData, LogStreamData, NodeSummary, TelemetryStreamData } from './apiTypes'

function isTelemetryData(data: unknown): data is TelemetryStreamData {
  return typeof data === 'object' && data !== null && 'channel' in data && 'value' in data
}

function isLogData(data: unknown): data is LogStreamData {
  return typeof data === 'object' && data !== null && 'message' in data && 'level' in data
}

function isEventData(data: unknown): data is EventStreamData {
  return typeof data === 'object' && data !== null && 'code' in data && 'severity' in data
}

function isNodeSummary(data: unknown): data is NodeSummary {
  return typeof data === 'object' && data !== null && 'node_id' in data && 'state' in data
}

/** Starts the pipeline; returns a teardown function (React effect cleanup). */
export function startStreamPipeline(): () => void {
  const unsubState = gatewayStream.onStateChange((state) => useConnectionStore.getState().setWsState(state))

  const unsubMessage = gatewayStream.onMessage((msg) => {
    switch (msg.kind) {
      case 'telemetry':
        if (msg.node_id !== null && isTelemetryData(msg.data)) {
          useStreamStore.getState().ingestTelemetry(msg.node_id, msg.data.channel, [msg.ts, msg.data.value])
        }
        break
      case 'log':
        if (isLogData(msg.data)) {
          useStreamStore.getState().ingestLog({ ts: msg.ts, node_id: msg.node_id, ...msg.data })
        }
        break
      case 'event':
        if (isEventData(msg.data)) {
          useStreamStore.getState().ingestEvent({ ts: msg.ts, node_id: msg.node_id, ...msg.data })
        }
        break
      case 'node':
        if (isNodeSummary(msg.data)) {
          useNodesStore.getState().upsertFromStream(msg.data)
        }
        break
      case 'heartbeat':
      case 'link':
      case 'raw':
        // Heartbeats are covered by the node poll; link/raw have no store binding yet
        // (Networks/S6 territory) — deliberately not wired to avoid a fake surface.
        break
    }
  })

  gatewayStream.connect()

  return () => {
    unsubState()
    unsubMessage()
    gatewayStream.disconnect()
  }
}

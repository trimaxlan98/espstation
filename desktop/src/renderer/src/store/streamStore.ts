/**
 * Live data ingested off the gateway WS stream: per-channel telemetry ring
 * buffers, plus bounded logs/events rails. Kept separate from `nodesStore`
 * (REST-sourced fleet state) because this store's data arrives at a much
 * higher rate and must never trigger a re-render of, say, the Nodes list.
 */
import { create } from 'zustand'
import { TelemetryRingBuffer } from '../lib/ringBuffer'
import type { EventStreamData, LogStreamData, TelemetryPoint } from '../lib/apiTypes'

const DEFAULT_CAPACITY = 1200 // ~ 20 min at 1 Hz, or 24 s at 50 Hz — plenty for a live chart window
const LOG_CAPACITY = 500
/**
 * One rail, shared by every node and every event code, so a chatty node
 * evicts a quiet one's history.
 *
 * 200 was sized for nodes that emit an event when something notable happens.
 * The Morse practice broke that assumption: a board emits `morse.symbol` per
 * dot and dash, so two operators keying at ~15 wpm push well over 5 events/s
 * and 200 slots is about 40 seconds of history. The Morse panel rebuilds the
 * received text from this rail, so past that point its headline text silently
 * loses its oldest letters.
 *
 * 2000 buys ~7 minutes at that rate and costs a few hundred kB. It is a
 * MITIGATION, not the fix: any rate eventually outruns any bound. The real
 * fix is for a panel that needs a transcript to accumulate one as events
 * arrive, instead of re-deriving it from a window that is allowed to forget.
 */
const EVENT_CAPACITY = 2000

function channelKey(nodeId: number, channel: string): string {
  return `${nodeId}:${channel}`
}

export interface LogEntry extends LogStreamData {
  ts: number
  node_id: number | null
}

export interface EventEntry extends EventStreamData {
  ts: number
  node_id: number | null
}

interface StreamState {
  capacity: number
  buffers: Map<string, TelemetryRingBuffer>
  /** Bumped on every ingest so selectors relying on a specific channel can subscribe cheaply. */
  version: number
  logs: LogEntry[]
  events: EventEntry[]
  ingestTelemetry: (nodeId: number, channel: string, point: TelemetryPoint) => void
  getSeries: (nodeId: number, channel: string) => TelemetryPoint[]
  /** The last `n` samples only — see TelemetryRingBuffer.recent(). */
  getRecent: (nodeId: number, channel: string, n: number) => TelemetryPoint[]
  getLatest: (nodeId: number, channel: string) => TelemetryPoint | undefined
  ingestLog: (entry: LogEntry) => void
  ingestEvent: (entry: EventEntry) => void
  clearNode: (nodeId: number) => void
  reset: () => void
}

function pushBounded<T>(list: T[], item: T, capacity: number): T[] {
  const next = list.length >= capacity ? list.slice(list.length - capacity + 1) : list.slice()
  next.push(item)
  return next
}

export const useStreamStore = create<StreamState>((set, get) => ({
  capacity: DEFAULT_CAPACITY,
  buffers: new Map(),
  version: 0,
  logs: [],
  events: [],

  ingestTelemetry: (nodeId, channel, point) => {
    const key = channelKey(nodeId, channel)
    const { buffers, capacity } = get()
    let buf = buffers.get(key)
    if (!buf) {
      buf = new TelemetryRingBuffer(capacity)
      buffers.set(key, buf)
    }
    buf.push(point)
    set((s) => ({ version: s.version + 1 }))
  },

  getSeries: (nodeId, channel) => {
    return get().buffers.get(channelKey(nodeId, channel))?.toArray() ?? []
  },
  getRecent: (nodeId, channel, n) => {
    return get().buffers.get(channelKey(nodeId, channel))?.recent(n) ?? []
  },

  getLatest: (nodeId, channel) => {
    return get().buffers.get(channelKey(nodeId, channel))?.latest()
  },

  ingestLog: (entry) => {
    set((s) => ({ logs: pushBounded(s.logs, entry, LOG_CAPACITY) }))
  },

  ingestEvent: (entry) => {
    set((s) => ({ events: pushBounded(s.events, entry, EVENT_CAPACITY) }))
  },

  clearNode: (nodeId) => {
    const { buffers } = get()
    const prefix = `${nodeId}:`
    for (const key of [...buffers.keys()]) {
      if (key.startsWith(prefix)) buffers.delete(key)
    }
    set((s) => ({
      version: s.version + 1,
      logs: s.logs.filter((l) => l.node_id !== nodeId),
      events: s.events.filter((e) => e.node_id !== nodeId)
    }))
  },

  reset: () => {
    get().buffers.clear()
    set({ version: 0, logs: [], events: [] })
  }
}))

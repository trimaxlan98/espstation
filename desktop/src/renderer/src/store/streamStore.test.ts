import { beforeEach, describe, expect, it } from 'vitest'
import { useStreamStore } from './streamStore'

beforeEach(() => {
  useStreamStore.getState().reset()
})

describe('streamStore', () => {
  it('ingests telemetry into a per-(node,channel) series, keyed independently', () => {
    const { ingestTelemetry, getSeries } = useStreamStore.getState()
    ingestTelemetry(1, 'adc.a0', [1, 3.3])
    ingestTelemetry(1, 'sys.heap_free', [1, 182000])
    ingestTelemetry(2, 'adc.a0', [1, 1.1])

    expect(getSeries(1, 'adc.a0')).toEqual([[1, 3.3]])
    expect(getSeries(1, 'sys.heap_free')).toEqual([[1, 182000]])
    expect(getSeries(2, 'adc.a0')).toEqual([[1, 1.1]])
  })

  it('handles out-of-order ingestion, keeping the series sorted', () => {
    const { ingestTelemetry, getSeries } = useStreamStore.getState()
    ingestTelemetry(1, 'adc.a0', [3, 3])
    ingestTelemetry(1, 'adc.a0', [1, 1])
    ingestTelemetry(1, 'adc.a0', [2, 2])
    expect(getSeries(1, 'adc.a0')).toEqual([
      [1, 1],
      [2, 2],
      [3, 3]
    ])
  })

  it('a replayed sample (same ts, drain-on-reconnect) overwrites rather than duplicates', () => {
    const { ingestTelemetry, getSeries } = useStreamStore.getState()
    ingestTelemetry(1, 'adc.a0', [1, 1])
    ingestTelemetry(1, 'adc.a0', [1, 999])
    expect(getSeries(1, 'adc.a0')).toEqual([[1, 999]])
  })

  it('getLatest reflects the most recent timestamp seen for that channel', () => {
    const { ingestTelemetry, getLatest } = useStreamStore.getState()
    ingestTelemetry(1, 'adc.a0', [5, 50])
    ingestTelemetry(1, 'adc.a0', [2, 20])
    expect(getLatest(1, 'adc.a0')).toEqual([5, 50])
  })

  it('an unseen channel returns an empty series, never throws', () => {
    expect(useStreamStore.getState().getSeries(99, 'nope')).toEqual([])
    expect(useStreamStore.getState().getLatest(99, 'nope')).toBeUndefined()
  })

  it('ingestLog and ingestEvent append to bounded rails', () => {
    const { ingestLog, ingestEvent } = useStreamStore.getState()
    ingestLog({ ts: 1, node_id: 1, level: 'info', tag: 'boot', message: 'hello' })
    ingestEvent({ ts: 1, node_id: 1, code: 'exp.trigger', severity: 'info' })
    expect(useStreamStore.getState().logs).toHaveLength(1)
    expect(useStreamStore.getState().events).toHaveLength(1)
  })

  it('clearNode drops that node’s buffers, logs and events but leaves other nodes intact', () => {
    const { ingestTelemetry, ingestLog, clearNode } = useStreamStore.getState()
    ingestTelemetry(1, 'adc.a0', [1, 1])
    ingestTelemetry(2, 'adc.a0', [1, 1])
    ingestLog({ ts: 1, node_id: 1, level: 'info', tag: 'x', message: 'm' })
    ingestLog({ ts: 1, node_id: 2, level: 'info', tag: 'x', message: 'm' })

    clearNode(1)

    expect(useStreamStore.getState().getSeries(1, 'adc.a0')).toEqual([])
    expect(useStreamStore.getState().getSeries(2, 'adc.a0')).toEqual([[1, 1]])
    expect(useStreamStore.getState().logs.map((l) => l.node_id)).toEqual([2])
  })
})

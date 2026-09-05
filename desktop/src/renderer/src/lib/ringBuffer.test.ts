import { describe, expect, it } from 'vitest'
import { TelemetryRingBuffer } from './ringBuffer'

describe('TelemetryRingBuffer', () => {
  it('keeps points sorted by timestamp even when pushed out of order', () => {
    const buf = new TelemetryRingBuffer(10)
    buf.push([3, 30])
    buf.push([1, 10])
    buf.push([2, 20])
    expect(buf.toArray()).toEqual([
      [1, 10],
      [2, 20],
      [3, 30]
    ])
  })

  it('overwrites an exact-timestamp duplicate (a replayed sample) in place rather than duplicating it', () => {
    const buf = new TelemetryRingBuffer(10)
    buf.push([1, 10])
    buf.push([2, 20])
    buf.push([1, 999]) // replay of ts=1 with a corrected/re-sent value
    expect(buf.length).toBe(2)
    expect(buf.toArray()).toEqual([
      [1, 999],
      [2, 20]
    ])
  })

  it('evicts the oldest point once over capacity', () => {
    const buf = new TelemetryRingBuffer(3)
    buf.push([1, 1])
    buf.push([2, 2])
    buf.push([3, 3])
    buf.push([4, 4])
    expect(buf.toArray()).toEqual([
      [2, 2],
      [3, 3],
      [4, 4]
    ])
  })

  it('eviction respects sort order, not push order — an out-of-order backfill can evict the true oldest', () => {
    const buf = new TelemetryRingBuffer(3)
    buf.push([5, 5])
    buf.push([6, 6])
    buf.push([1, 1]) // backfilled sample older than everything already buffered
    // Capacity 3 is not exceeded yet (3 distinct points) — nothing evicted.
    expect(buf.toArray()).toEqual([
      [1, 1],
      [5, 5],
      [6, 6]
    ])
    buf.push([7, 7]) // now over capacity — the true oldest (ts=1) is evicted
    expect(buf.toArray()).toEqual([
      [5, 5],
      [6, 6],
      [7, 7]
    ])
  })

  it('latest() returns the most recent point by timestamp, not by insertion order', () => {
    const buf = new TelemetryRingBuffer(10)
    buf.push([5, 50])
    buf.push([2, 20]) // inserted after, but earlier in time
    expect(buf.latest()).toEqual([5, 50])
  })

  it('latest() is undefined for an empty buffer', () => {
    const buf = new TelemetryRingBuffer(10)
    expect(buf.latest()).toBeUndefined()
  })

  it('clear() empties the buffer', () => {
    const buf = new TelemetryRingBuffer(10)
    buf.push([1, 1])
    buf.clear()
    expect(buf.toArray()).toEqual([])
    expect(buf.length).toBe(0)
  })

  it('toArray() returns a snapshot that further pushes do not mutate', () => {
    const buf = new TelemetryRingBuffer(10)
    buf.push([1, 1])
    const snapshot = buf.toArray()
    buf.push([2, 2])
    expect(snapshot).toEqual([[1, 1]])
  })

  it('rejects a non-positive capacity', () => {
    expect(() => new TelemetryRingBuffer(0)).toThrow()
  })
})

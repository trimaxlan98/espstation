/**
 * Bounded, time-ordered buffer for one channel's telemetry (protocol/
 * PROTOCOL.md §4.4/§5: TELEMETRY batches can arrive out of order across
 * link drops, and a reconnect drains stored samples with `replay: true`
 * interleaved with live data — the exact same (ts, channel) pair can show
 * up twice). Kept as a plain, framework-free class so it's unit-testable
 * without zustand/React — store/streamStore.ts wraps one per channel.
 */
import type { TelemetryPoint } from './apiTypes'

export class TelemetryRingBuffer {
  private points: TelemetryPoint[] = []

  constructor(private readonly capacity: number) {
    if (capacity <= 0) throw new Error('TelemetryRingBuffer: capacity must be positive')
  }

  /** Binary search for the insertion index of `ts` (first index whose point.ts >= ts). */
  private lowerBound(ts: number): number {
    let lo = 0
    let hi = this.points.length
    while (lo < hi) {
      const mid = (lo + hi) >>> 1
      const midPoint = this.points[mid]
      if (midPoint !== undefined && midPoint[0] < ts) lo = mid + 1
      else hi = mid
    }
    return lo
  }

  /**
   * Inserts a sample in timestamp order. An exact `ts` match (a replayed
   * duplicate) overwrites the existing value in place rather than creating
   * a second point — a node's drain-on-reconnect can legitimately resend
   * the same sample, and treating it as a new point would double it up on
   * the chart. Evicts the oldest point once over capacity.
   */
  push(point: TelemetryPoint): void {
    const [ts] = point
    const idx = this.lowerBound(ts)
    const existing = this.points[idx]
    if (existing !== undefined && existing[0] === ts) {
      this.points[idx] = point
      return
    }
    this.points.splice(idx, 0, point)
    if (this.points.length > this.capacity) {
      this.points.shift()
    }
  }

  /** Returns a snapshot (safe to hold onto — never mutated in place). */
  toArray(): TelemetryPoint[] {
    return [...this.points]
  }

  get length(): number {
    return this.points.length
  }

  latest(): TelemetryPoint | undefined {
    return this.points[this.points.length - 1]
  }

  clear(): void {
    this.points = []
  }
}

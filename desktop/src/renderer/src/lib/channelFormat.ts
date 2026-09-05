/**
 * NDB-driven value formatting (protocol/PROTOCOL.md §4.1: "the station never
 * hard-codes channel ids; every chart, unit and limit is driven by what the
 * node declares"). Nothing here may special-case a channel key — every
 * decision comes from the channel's declared `type`, `unit`, `min` and
 * `max`, so a brand-new sensor a node adds at runtime formats correctly with
 * zero changes here.
 */
import type { ChannelType, NdbChannel } from './apiTypes'

const INTEGER_TYPES: ReadonlySet<ChannelType> = new Set(['u8', 'i8', 'u16', 'i16', 'u32', 'i32'])

/**
 * Decimal places to display, derived from the channel's declared numeric
 * range (`min`/`max`) rather than a hard-coded per-unit table — a channel
 * spanning a narrow range (e.g. 0–3.3 V) reads better with more precision
 * than one spanning thousands (e.g. a heap counter in bytes, which is an
 * integer type anyway and short-circuits above). Falls back to 2 decimals
 * when the node hasn't declared a range.
 */
export function decimalsForChannel(channel: Pick<NdbChannel, 'type' | 'min' | 'max'>): number {
  if (channel.type === 'bool') return 0
  if (INTEGER_TYPES.has(channel.type)) return 0
  if (channel.min === undefined || channel.max === undefined) return 2
  const span = Math.abs(channel.max - channel.min)
  if (span <= 0) return 2
  if (span <= 5) return 3
  if (span <= 20) return 2
  if (span <= 200) return 1
  return 0
}

/** Appends a unit with the spacing convention that reads naturally (`"42 %"` looks off; `"42%"` doesn't). */
export function appendUnit(text: string, unit: string): string {
  if (!unit) return text
  if (unit === '%' || unit === '°') return `${text}${unit}`
  return `${text} ${unit}`
}

/** Formats a raw sample value for display, given the NDB channel that declares it. */
export function formatChannelValue(channel: Pick<NdbChannel, 'type' | 'unit' | 'min' | 'max'>, value: number): string {
  if (channel.type === 'bool') return value !== 0 ? 'true' : 'false'
  if (!Number.isFinite(value)) return '—'
  const decimals = decimalsForChannel(channel)
  return appendUnit(value.toFixed(decimals), channel.unit)
}

/** True when a sample falls outside the channel's declared [min, max] — drives a warning tone in the UI. */
export function isOutOfRange(channel: Pick<NdbChannel, 'min' | 'max'>, value: number): boolean {
  if (channel.min !== undefined && value < channel.min) return true
  if (channel.max !== undefined && value > channel.max) return true
  return false
}

/** Groups NDB channels by their declared `group` field, preserving first-seen group order. */
export function groupChannels(channels: NdbChannel[]): Map<string, NdbChannel[]> {
  const groups = new Map<string, NdbChannel[]>()
  for (const ch of channels) {
    const list = groups.get(ch.group)
    if (list) list.push(ch)
    else groups.set(ch.group, [ch])
  }
  return groups
}

/**
 * The square wave of the key, rebuilt from events.
 *
 * WHY NOT FROM `morse.tx` / `morse.rx`. Those two NDB channels carry the line
 * level, which sounds like exactly the right source and is not: main.c's
 * telemetry task publishes every channel once a second, so the boards declare
 * and send them at 1 Hz. A dot lasts ~100 ms. The firmware's own NDB comment
 * records the measurement — "a whole SOS went by with morse.tx never once
 * sampled high" — so a wave drawn from them is a wave of mostly nothing.
 *
 * The events ARE the record of what was keyed: `morse.symbol` fires on the
 * edge that ended the pulse and carries its duration in `ms`, so the pulse
 * occupied [ts - ms, ts] and the silence is whatever lies between two of
 * them. That reconstruction is exact to the millisecond the board measured,
 * which is the same number the practice compares between the two ends
 * (bench/practicas/morse-duplex/SPEC-DUPLEX.md, "Lo que si se puede medir").
 *
 * WHAT IT CANNOT SHOW. A pulse only exists once it has ENDED, because that is
 * when the board can say how long it was. While a key is held down there is
 * nothing to draw and the view shows a growing silence instead — see
 * components/MorseWave.tsx, which draws the silence since the last symbol as
 * a live number. Faking an in-flight pulse would mean inventing a duration.
 */
import type { EventEntry } from '../store/streamStore'

export type MorseDir = 'TX' | 'RX'

export interface WavePulse {
  /** Milliseconds, in EventEntry.ts's base (gateway epoch seconds x1000). */
  t0: number
  t1: number
  sym: '.' | '-'
}

export interface WaveLetter {
  /** When the decoder closed the letter. */
  t: number
  /** Start of the first symbol and end of the last one that formed it. */
  from: number
  to: number
  /** The decoded character, or the raw code when there was no match. */
  text: string
  code: string
  ok: boolean
}

export interface WaveLane {
  pulses: WavePulse[]
  letters: WaveLetter[]
  /** Word gaps, as timestamps. */
  words: number[]
  /** Pulses the debounce rejected: noise on a line that should be quiet. */
  filtered: number[]
  /** Newest event timestamp in this lane, or null when it has none. */
  lastTs: number | null
}

export interface NodeWave {
  TX: WaveLane
  RX: WaveLane
  /** Newest timestamp across both lanes — the anchor the view's clock uses. */
  lastTs: number | null
}

/**
 * How much history a lane keeps, and the hard cap under it.
 *
 * The span is what bounds the SVG's coordinate range (the view lays every
 * pulse out in absolute units and slides one transform over them), and the
 * count is what bounds the DOM when someone keys fast for a minute. Both are
 * far beyond any window the view offers, so neither ever eats the visible
 * part.
 */
export const WAVE_SPAN_MS = 60_000
const MAX_PULSES = 400

function emptyLane(): WaveLane {
  return { pulses: [], letters: [], words: [], filtered: [], lastTs: null }
}

/** `data.dir` as the firmware stamps it; anything else is not a direction. */
function dirOf(e: EventEntry): MorseDir | null {
  const d = e.data?.dir
  return d === 'TX' || d === 'RX' ? d : null
}

function msOf(e: EventEntry): number {
  const ms = e.data?.ms
  return typeof ms === 'number' && Number.isFinite(ms) && ms > 0 ? ms : 0
}

function strOf(v: unknown): string {
  // An event that reached the rail malformed must not paint "[object Object]"
  // into the drawing, the same rule the decoded text line follows.
  return typeof v === 'string' || typeof v === 'number' ? String(v) : ''
}

/**
 * One pass over the shared event rail, producing both lanes of every station
 * asked for.
 *
 * One pass, not one per station per direction: the rail is shared by the whole
 * fleet and holds thousands of entries, and this runs again on every event
 * that lands.
 */
export function buildWaves(events: EventEntry[], nodeIds: number[]): Map<number, NodeWave> {
  const out = new Map<number, NodeWave>()
  if (nodeIds.length === 0) return out
  for (const id of nodeIds) out.set(id, { TX: emptyLane(), RX: emptyLane(), lastTs: null })

  /** Symbols seen since the last letter/unknown, per node and direction. */
  const pending = new Map<string, WavePulse[]>()
  const take = (key: string): WavePulse[] => {
    const p = pending.get(key) ?? []
    pending.set(key, [])
    return p
  }

  for (const e of events) {
    if (e.node_id === null) continue
    const wave = out.get(e.node_id)
    if (wave === undefined) continue
    const dir = dirOf(e)
    if (dir === null) continue
    const lane = wave[dir]
    const t = e.ts * 1000
    const key = `${e.node_id}:${dir}`

    switch (e.code) {
      case 'morse.symbol': {
        const sym = e.data?.symbol
        if (sym !== '.' && sym !== '-') continue
        const pulse: WavePulse = { t0: t - msOf(e), t1: t, sym }
        lane.pulses.push(pulse)
        pending.set(key, [...(pending.get(key) ?? []), pulse])
        break
      }
      case 'morse.letter':
      case 'morse.unknown': {
        const ok = e.code === 'morse.letter'
        const code = strOf(e.data?.code)
        const text = ok ? strOf(e.data?.letter) : code
        if (text === '') {
          // Nothing to label, but the symbols still belong to a closed group:
          // dropping them here would glue them onto the NEXT letter's span.
          take(key)
          continue
        }
        const group = take(key)
        const first = group[0]
        const last = group[group.length - 1]
        lane.letters.push({
          t,
          // A letter whose symbols fell off the end of the rail still gets
          // drawn, as a zero-width mark at the moment it closed.
          from: first?.t0 ?? t,
          to: last?.t1 ?? t,
          text,
          code,
          ok
        })
        break
      }
      case 'morse.word':
        lane.words.push(t)
        break
      case 'morse.filtered':
        lane.filtered.push(t)
        break
      default:
        continue
    }
    if (lane.lastTs === null || t > lane.lastTs) lane.lastTs = t
    if (wave.lastTs === null || t > wave.lastTs) wave.lastTs = t
  }

  for (const wave of out.values()) {
    for (const dir of ['TX', 'RX'] as const) trimLane(wave[dir], wave.lastTs)
  }
  return out
}

/**
 * Drops what the view can never show again.
 *
 * Trimmed against the station's newest event in EITHER direction, not the
 * lane's own: the two lanes share one time axis on screen, so a lane that has
 * been quiet for a minute must empty as the other one advances — otherwise
 * its last pulse would sit frozen at the left edge pretending to be recent.
 */
function trimLane(lane: WaveLane, lastTs: number | null): void {
  if (lastTs === null) return
  const floor = lastTs - WAVE_SPAN_MS
  lane.pulses = lane.pulses.filter((p) => p.t1 >= floor)
  if (lane.pulses.length > MAX_PULSES) lane.pulses = lane.pulses.slice(-MAX_PULSES)
  lane.letters = lane.letters.filter((l) => l.t >= floor)
  lane.words = lane.words.filter((t) => t >= floor)
  lane.filtered = lane.filtered.filter((t) => t >= floor)
}

/** End of the newest pulse in the lane, or null when it has none. */
export function lastPulseEnd(lane: WaveLane): number | null {
  return lane.pulses[lane.pulses.length - 1]?.t1 ?? null
}

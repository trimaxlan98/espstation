/**
 * Cadence: what an operator's hand actually measured, from their own key.
 *
 * The wave shows the shape; this is the number behind it. The practice's real
 * question is never "was that a dot" but "are my dots and my dashes far
 * enough apart that a threshold between them is safe", and the same for my
 * gaps. Both are answerable from the pulses the board already reported — no
 * setting has to be typed in and nothing is inferred from the firmware.
 *
 * READ FROM THE TX LANE. TX is the operator's own hand as their board echoed
 * it; RX is the other person's. Judging your cadence from RX would be judging
 * someone else's.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO: recommend a `letra_ms`. The bench
 * session that tried found the two gap populations OVERLAP — the longest gap
 * inside a letter was longer than the shortest gap between letters — and when
 * that happens no threshold separates them and picking one from a single
 * session's rhythm is how a wrong number gets written down as a finding. So
 * the overlap is reported as an overlap, and a midpoint is offered only for
 * `punto_raya_ms`, only when there is a real empty band, and only ever
 * described as "on these N symbols".
 */
import type { WaveLane } from './morseWave'

export interface Spread {
  n: number
  min: number
  max: number
  median: number
}

export interface Cadence {
  /** Symbols the numbers below were taken from. */
  sample: number
  dots: Spread | null
  dashes: Spread | null
  /**
   * The empty band between the longest dot and the shortest dash. Negative
   * when the two populations overlap, which means no `punto_raya_ms` could
   * have classified all of them correctly.
   */
  band: number | null
  /** Midpoint of the band; null unless the band is real and positive. */
  midpoint: number | null
  /** Gaps between two symbols of the same letter. */
  intra: Spread | null
  /** Gaps that closed a letter. */
  letterGaps: Spread | null
  /** True when the two gap populations overlap, so no `letra_ms` separates them. */
  gapsOverlap: boolean
  /** PARIS words per minute implied by the median dot, rounded to one decimal. */
  wpm: number | null
}

/**
 * The bench's own rule of thumb, from docs/PRACTICA-MORSE.md: below this the
 * dot/dash threshold is fragile and the fix is the hand, not the number.
 */
export const BAND_COMFORTABLE_MS = 40

function spread(values: number[]): Spread | null {
  if (values.length === 0) return null
  const sorted = [...values].sort((a, b) => a - b)
  const mid = sorted.length >> 1
  const lo = sorted[mid - 1]
  const hi = sorted[mid]
  // An even count has no single middle sample, so the median is the mean of
  // the two that straddle it.
  const median = sorted.length % 2 === 1 ? (hi ?? 0) : ((lo ?? 0) + (hi ?? 0)) / 2
  return {
    n: sorted.length,
    min: sorted[0] ?? 0,
    max: sorted[sorted.length - 1] ?? 0,
    median
  }
}

/**
 * Whether a letter (or an unmatched code) closed inside this silence.
 *
 * The decoder decides a letter is finished when the gap passes `letra_ms`, so
 * the event lands inside the gap it ended — that is what tells the two gap
 * populations apart here, rather than a threshold this code would have to
 * guess.
 */
function closesALetter(lane: WaveLane, from: number, to: number): boolean {
  return lane.letters.some((l) => l.t > from && l.t <= to) || lane.words.some((t) => t > from && t <= to)
}

/**
 * `sampleN` bounds how far back a number reaches. A cadence is a statement
 * about how someone is keying NOW; averaging in the first hesitant minute of
 * a session makes it a statement about nothing.
 */
export function cadenceOf(lane: WaveLane, sampleN = 40): Cadence {
  const pulses = lane.pulses.slice(-sampleN)
  const dots: number[] = []
  const dashes: number[] = []
  for (const p of pulses) {
    const ms = p.t1 - p.t0
    if (p.sym === '.') dots.push(ms)
    else dashes.push(ms)
  }

  const intra: number[] = []
  const letterGaps: number[] = []
  for (let i = 1; i < pulses.length; i++) {
    const prev = pulses[i - 1]
    const cur = pulses[i]
    if (prev === undefined || cur === undefined) continue
    const gap = cur.t0 - prev.t1
    if (gap < 0) continue
    if (closesALetter(lane, prev.t1, cur.t0)) letterGaps.push(gap)
    else intra.push(gap)
  }

  const d = spread(dots)
  const D = spread(dashes)
  const band = d !== null && D !== null ? D.min - d.max : null
  const i = spread(intra)
  const L = spread(letterGaps)

  return {
    sample: pulses.length,
    dots: d,
    dashes: D,
    band,
    midpoint: band !== null && band > 0 && d !== null && D !== null ? Math.round((d.max + D.min) / 2) : null,
    intra: i,
    letterGaps: L,
    // The overlap that makes `letra_ms` unsolvable, stated rather than
    // papered over with an average.
    gapsOverlap: i !== null && L !== null && i.max >= L.min,
    // PARIS: the dot IS the unit, and a word is 50 units, so 1200/unit ms.
    wpm: d !== null && d.median > 0 ? Math.round((1200 / d.median) * 10) / 10 : null
  }
}

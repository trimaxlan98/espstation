import { describe, expect, it } from 'vitest'
import { BAND_COMFORTABLE_MS, cadenceOf } from './morseCadence'
import type { WaveLane, WavePulse } from './morseWave'

function lane(pulses: WavePulse[], letters: number[] = [], words: number[] = []): WaveLane {
  return {
    pulses,
    letters: letters.map((t) => ({ t, from: t, to: t, text: 'X', code: '.', ok: true })),
    words,
    filtered: [],
    lastTs: pulses[pulses.length - 1]?.t1 ?? null
  }
}

/** A pulse of `ms` starting at `t0`. */
const p = (t0: number, ms: number, sym: '.' | '-'): WavePulse => ({ t0, t1: t0 + ms, sym })

describe('cadenceOf', () => {
  it('says nothing at all when nothing has been keyed', () => {
    const c = cadenceOf(lane([]))
    expect(c.sample).toBe(0)
    expect(c.dots).toBeNull()
    expect(c.band).toBeNull()
    expect(c.wpm).toBeNull()
  })

  it('separates dots from dashes and reports each spread', () => {
    const c = cadenceOf(lane([p(0, 90, '.'), p(200, 110, '.'), p(500, 300, '-'), p(1000, 340, '-')]))
    expect(c.dots).toEqual({ n: 2, min: 90, max: 110, median: 100 })
    expect(c.dashes).toEqual({ n: 2, min: 300, max: 340, median: 320 })
  })

  it('takes the median of an odd count as the middle sample', () => {
    const c = cadenceOf(lane([p(0, 90, '.'), p(200, 100, '.'), p(400, 200, '.')]))
    expect(c.dots?.median).toBe(100)
  })

  it('measures the empty band and puts the midpoint in the middle of it', () => {
    const c = cadenceOf(lane([p(0, 100, '.'), p(300, 300, '-')]))
    expect(c.band).toBe(200)
    expect(c.midpoint).toBe(200)
  })

  it('reports an overlap as a negative band and refuses to suggest a midpoint', () => {
    // The longest dot is LONGER than the shortest dash: no threshold splits
    // these, and offering a number would be inventing one.
    const c = cadenceOf(lane([p(0, 260, '.'), p(400, 240, '-')]))
    expect(c.band).toBeLessThanOrEqual(0)
    expect(c.midpoint).toBeNull()
  })

  it('calls a band under the bench threshold what it is', () => {
    const c = cadenceOf(lane([p(0, 100, '.'), p(300, 120, '-')]))
    expect(c.band).toBe(20)
    expect(c.band as number).toBeLessThan(BAND_COMFORTABLE_MS)
  })

  it('tells a gap inside a letter from one that closed a letter', () => {
    // Two symbols, a short silence, then a letter event inside the NEXT
    // silence — so the first gap is intra and the second is a letter gap.
    const pulses = [p(0, 100, '.'), p(200, 100, '.'), p(900, 100, '.')]
    const c = cadenceOf(lane(pulses, [500]))
    expect(c.intra?.n).toBe(1)
    expect(c.intra?.median).toBe(100)
    expect(c.letterGaps?.n).toBe(1)
    expect(c.letterGaps?.median).toBe(600)
  })

  it('counts a word gap as a letter gap, not as one inside a letter', () => {
    const pulses = [p(0, 100, '.'), p(900, 100, '.')]
    const c = cadenceOf(lane(pulses, [], [500]))
    expect(c.letterGaps?.n).toBe(1)
    expect(c.intra).toBeNull()
  })

  it('reports overlapping gap populations instead of averaging them away', () => {
    // The finding the bench actually made: the longest silence inside a
    // letter is longer than the shortest one between letters, so no letra_ms
    // separates them.
    const pulses = [p(0, 100, '.'), p(700, 100, '.'), p(1400, 100, '.'), p(1900, 100, '.')]
    // letter closes in the gap before the third pulse (600 ms) but the gap
    // before the second (600 ms) is inside a letter
    const c = cadenceOf(lane(pulses, [1000]))
    expect(c.gapsOverlap).toBe(true)
  })

  it('does not claim an overlap when the two populations are clearly apart', () => {
    const pulses = [p(0, 100, '.'), p(200, 100, '.'), p(1200, 100, '.')]
    const c = cadenceOf(lane(pulses, [700]))
    expect(c.gapsOverlap).toBe(false)
  })

  it('derives wpm from the dot median the PARIS way', () => {
    // 100 ms dot = 12 wpm: a word is 50 units and the dot is the unit.
    expect(cadenceOf(lane([p(0, 100, '.')])).wpm).toBe(12)
    expect(cadenceOf(lane([p(0, 60, '.')])).wpm).toBe(20)
  })

  it('only looks at the recent symbols, not at the whole session', () => {
    const many = Array.from({ length: 60 }, (_, i) => p(i * 1000, i < 20 ? 500 : 100, '.'))
    const c = cadenceOf(many.length > 0 ? lane(many) : lane([]), 40)
    expect(c.sample).toBe(40)
    // the first twenty, which were keyed at a different rhythm, are gone
    expect(c.dots?.max).toBe(100)
  })
})

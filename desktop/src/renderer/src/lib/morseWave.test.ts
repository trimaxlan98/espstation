import { describe, expect, it } from 'vitest'
import { WAVE_SPAN_MS, buildWaves, lastPulseEnd } from './morseWave'
import type { EventEntry } from '../store/streamStore'

function ev(node: number, ts: number, code: string, data: Record<string, unknown>): EventEntry {
  return { node_id: node, ts, code, severity: 'debug', data }
}

const sym = (node: number, ts: number, s: string, ms: number, dir = 'RX'): EventEntry =>
  ev(node, ts, 'morse.symbol', { dir, symbol: s, ms })

describe('buildWaves', () => {
  it('places a pulse on the interval it actually occupied', () => {
    // The event fires when the pulse ENDS and carries its length, so the
    // pulse is [ts - ms, ts] — getting this backwards would draw every dot
    // and dash one pulse-length late.
    const w = buildWaves([sym(1, 10, '.', 120)], [1]).get(1)
    expect(w?.RX.pulses).toEqual([{ t0: 10_000 - 120, t1: 10_000, sym: '.' }])
  })

  it('keeps the two directions in separate lanes', () => {
    const w = buildWaves([sym(1, 10, '.', 90, 'TX'), sym(1, 11, '-', 300, 'RX')], [1]).get(1)
    expect(w?.TX.pulses.map((p) => p.sym)).toEqual(['.'])
    expect(w?.RX.pulses.map((p) => p.sym)).toEqual(['-'])
  })

  it('ignores an event with no direction rather than guessing one', () => {
    const w = buildWaves([sym(1, 10, '.', 90, '?'), ev(1, 11, 'morse.symbol', { symbol: '.' })], [1]).get(1)
    expect(w?.RX.pulses).toEqual([])
    expect(w?.TX.pulses).toEqual([])
    expect(w?.lastTs).toBeNull()
  })

  it('ignores a symbol that is not a dot or a dash', () => {
    const w = buildWaves([sym(1, 10, 'x', 90), sym(1, 11, '', 90)], [1]).get(1)
    expect(w?.RX.pulses).toEqual([])
  })

  it('survives a symbol event that carries no duration', () => {
    // The firmware omits `ms` when it is zero, so this is a real shape and
    // not a hypothetical; the pulse becomes an instant, not a NaN.
    const w = buildWaves([ev(1, 10, 'morse.symbol', { dir: 'RX', symbol: '.' })], [1]).get(1)
    expect(w?.RX.pulses).toEqual([{ t0: 10_000, t1: 10_000, sym: '.' }])
  })

  it('spans a letter from its first symbol to its last', () => {
    const events = [
      sym(1, 10.0, '.', 100),
      sym(1, 10.3, '.', 100),
      sym(1, 10.6, '.', 100),
      ev(1, 11, 'morse.letter', { dir: 'RX', letter: 'S', code: '...' })
    ]
    const letter = buildWaves(events, [1]).get(1)?.RX.letters[0]
    expect(letter?.text).toBe('S')
    expect(letter?.code).toBe('...')
    expect(letter?.ok).toBe(true)
    expect(letter?.from).toBe(10_000 - 100)
    expect(letter?.to).toBe(10_600)
  })

  it('does not glue one letter onto the symbols of the previous one', () => {
    const events = [
      sym(1, 10, '.', 100),
      ev(1, 10.5, 'morse.letter', { dir: 'RX', letter: 'E', code: '.' }),
      sym(1, 11, '-', 300),
      ev(1, 11.5, 'morse.letter', { dir: 'RX', letter: 'T', code: '-' })
    ]
    const letters = buildWaves(events, [1]).get(1)?.RX.letters
    expect(letters?.map((l) => l.text)).toEqual(['E', 'T'])
    expect(letters?.[1]?.from).toBe(11_000 - 300)
  })

  it('shows an unknown code as the code itself, marked not-ok', () => {
    const events = [sym(1, 10, '.', 100), ev(1, 11, 'morse.unknown', { dir: 'RX', code: '......' })]
    const letter = buildWaves(events, [1]).get(1)?.RX.letters[0]
    expect(letter?.text).toBe('......')
    expect(letter?.ok).toBe(false)
  })

  it('never renders an object as a letter', () => {
    const events = [sym(1, 10, '.', 100), ev(1, 11, 'morse.letter', { dir: 'RX', letter: { a: 1 } })]
    const w = buildWaves(events, [1]).get(1)
    expect(w?.RX.letters).toEqual([])
    // the symbols it closed are still consumed, so they cannot be absorbed
    // into whatever letter arrives next
    expect(w?.RX.pulses.length).toBe(1)
  })

  it('collects word gaps and rejected pulses as their own marks', () => {
    const events = [ev(1, 10, 'morse.word', { dir: 'RX' }), ev(1, 11, 'morse.filtered', { dir: 'RX', ms: 3 })]
    const lane = buildWaves(events, [1]).get(1)?.RX
    expect(lane?.words).toEqual([10_000])
    expect(lane?.filtered).toEqual([11_000])
  })

  it('walks the shared rail once and still separates the stations', () => {
    const waves = buildWaves([sym(1, 10, '.', 100), sym(2, 10, '-', 300), sym(3, 10, '.', 100)], [1, 2])
    expect(waves.get(1)?.RX.pulses.length).toBe(1)
    expect(waves.get(2)?.RX.pulses[0]?.sym).toBe('-')
    // node 3 was not asked for: it must not appear at all
    expect(waves.has(3)).toBe(false)
  })

  it('empties a lane that has gone quiet as the other one advances', () => {
    // The two lanes share one axis on screen. A TX pulse a full span old must
    // not stay pinned at the left edge pretending to be recent just because
    // TX has been silent since.
    const old = 10
    const recent = old + WAVE_SPAN_MS / 1000 + 1
    const w = buildWaves([sym(1, old, '.', 100, 'TX'), sym(1, recent, '.', 100, 'RX')], [1]).get(1)
    expect(w?.TX.pulses).toEqual([])
    expect(w?.RX.pulses.length).toBe(1)
  })

  it('returns nothing at all when no station was asked for', () => {
    expect(buildWaves([sym(1, 10, '.', 100)], []).size).toBe(0)
  })
})

describe('lastPulseEnd', () => {
  it('is the end of the newest pulse, or null on an empty lane', () => {
    const w = buildWaves([sym(1, 10, '.', 100), sym(1, 12, '-', 300)], [1]).get(1)
    expect(w === undefined ? null : lastPulseEnd(w.RX)).toBe(12_000)
    expect(w === undefined ? null : lastPulseEnd(w.TX)).toBeNull()
  })
})

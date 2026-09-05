import { describe, expect, it } from 'vitest'
import { appendUnit, decimalsForChannel, formatChannelValue, groupChannels, isOutOfRange } from './channelFormat'
import type { NdbChannel } from './apiTypes'

const adc: NdbChannel = { id: 16, key: 'adc.a0', name: 'ADC ch0', unit: 'V', type: 'f32', rate_hz: 50, group: 'analog', min: 0, max: 3.3 }
const heap: NdbChannel = { id: 1, key: 'sys.heap_free', name: 'Heap free', unit: 'B', type: 'u32', rate_hz: 1, group: 'system' }
const rssi: NdbChannel = { id: 2, key: 'sys.rssi', name: 'WiFi RSSI', unit: 'dBm', type: 'i8', rate_hz: 1, group: 'system' }
const flag: NdbChannel = { id: 30, key: 'exp.armed', name: 'Armed', unit: '', type: 'bool', rate_hz: 1, group: 'experiment' }
const pct: NdbChannel = { id: 31, key: 'exp.progress', name: 'Progress', unit: '%', type: 'f32', rate_hz: 1, group: 'experiment', min: 0, max: 100 }

describe('decimalsForChannel', () => {
  it('integer types always get 0 decimals regardless of declared range', () => {
    expect(decimalsForChannel(heap)).toBe(0)
    expect(decimalsForChannel(rssi)).toBe(0)
  })

  it('bool always gets 0 decimals', () => {
    expect(decimalsForChannel(flag)).toBe(0)
  })

  it('derives precision from a narrow declared range (0–3.3 V -> 3 decimals)', () => {
    expect(decimalsForChannel(adc)).toBe(3)
  })

  it('derives coarser precision from a wide declared range (0–100 % -> 1 decimal)', () => {
    expect(decimalsForChannel(pct)).toBe(1)
  })

  it('falls back to 2 decimals when no range is declared', () => {
    expect(decimalsForChannel({ type: 'f32', min: undefined, max: undefined })).toBe(2)
  })
})

describe('appendUnit', () => {
  it('joins with a space for a normal unit', () => {
    expect(appendUnit('3.300', 'V')).toBe('3.300 V')
  })

  it('joins percent/degree without a space', () => {
    expect(appendUnit('42.0', '%')).toBe('42.0%')
    expect(appendUnit('21.5', '°')).toBe('21.5°')
  })

  it('returns the bare text when the unit is empty', () => {
    expect(appendUnit('true', '')).toBe('true')
  })
})

describe('formatChannelValue', () => {
  it('formats an f32 channel with unit and NDB-derived precision', () => {
    expect(formatChannelValue(adc, 3.3)).toBe('3.300 V')
  })

  it('formats an integer channel with no decimals', () => {
    expect(formatChannelValue(heap, 182304)).toBe('182304 B')
  })

  it('formats a bool channel as true/false, never a number', () => {
    expect(formatChannelValue(flag, 1)).toBe('true')
    expect(formatChannelValue(flag, 0)).toBe('false')
  })

  it('renders a non-finite value as an em dash rather than "NaN"', () => {
    expect(formatChannelValue(adc, NaN)).toBe('—')
  })
})

describe('isOutOfRange', () => {
  it('flags a value above max or below min', () => {
    expect(isOutOfRange(adc, 3.4)).toBe(true)
    expect(isOutOfRange(adc, -0.1)).toBe(true)
    expect(isOutOfRange(adc, 1.5)).toBe(false)
  })

  it('never flags a channel with no declared range', () => {
    expect(isOutOfRange(heap, 999999999)).toBe(false)
  })
})

describe('groupChannels', () => {
  it('groups by the declared `group` field, preserving first-seen order', () => {
    const groups = groupChannels([heap, rssi, adc, flag])
    expect([...groups.keys()]).toEqual(['system', 'analog', 'experiment'])
    expect(groups.get('system')).toEqual([heap, rssi])
    expect(groups.get('analog')).toEqual([adc])
  })
})

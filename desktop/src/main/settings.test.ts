import { mkdtempSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { DEFAULT_SETTINGS } from '@shared/types'
import { configDir, loadSettings, saveSettings } from './settings'

let dir: string

beforeEach(() => {
  dir = mkdtempSync(join(tmpdir(), 'espstation-settings-'))
  process.env['ESPSTATION_CONFIG_DIR'] = dir
})

afterEach(() => {
  delete process.env['ESPSTATION_CONFIG_DIR']
  rmSync(dir, { recursive: true, force: true })
})

describe('settings persistence', () => {
  it('resolves configDir from ESPSTATION_CONFIG_DIR', () => {
    expect(configDir()).toBe(dir)
  })

  it('returns defaults when no file exists yet', () => {
    expect(loadSettings()).toEqual(DEFAULT_SETTINGS)
  })

  it('round-trips a save through load', () => {
    const saved = saveSettings({ gatewayUrl: 'http://127.0.0.1:9000', gatewayToken: 'secret' })
    expect(saved.gatewayUrl).toBe('http://127.0.0.1:9000')
    expect(saved.gatewayToken).toBe('secret')
    // Untouched fields keep their defaults.
    expect(saved.theme).toBe(DEFAULT_SETTINGS.theme)

    expect(loadSettings()).toEqual(saved)
  })

  it('merges a patch onto the previously saved settings rather than replacing them', () => {
    saveSettings({ gatewayUrl: 'http://127.0.0.1:9000' })
    const second = saveSettings({ theme: 'dark' })
    expect(second.gatewayUrl).toBe('http://127.0.0.1:9000')
    expect(second.theme).toBe('dark')
  })

  it('falls back to defaults on a corrupt settings file rather than throwing', () => {
    writeFileSync(join(dir, 'settings.json'), '{not json', 'utf8')
    expect(loadSettings()).toEqual(DEFAULT_SETTINGS)
  })

  it('rejects an invalid theme/gatewayMode from a hand-edited file', () => {
    writeFileSync(join(dir, 'settings.json'), JSON.stringify({ theme: 'purple', gatewayMode: 'telepathic' }), 'utf8')
    const loaded = loadSettings()
    expect(loaded.theme).toBe(DEFAULT_SETTINGS.theme)
    expect(loaded.gatewayMode).toBe(DEFAULT_SETTINGS.gatewayMode)
  })
})

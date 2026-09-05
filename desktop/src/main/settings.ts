/**
 * Settings persistence: a single JSON file in `~/.config/espstation-desktop/`
 * (spec'd location, independent of Electron's per-productName `userData`
 * default so it stays stable across renames/rebrands). Plain `node:fs`, no
 * SQLite — settings are a handful of scalar fields, not a dataset; runs and
 * samples are the gateway's job (its own SQLite, per docs/ARCHITECTURE.md).
 */
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { homedir } from 'node:os'
import { join } from 'node:path'
import type { Settings, SettingsPatch } from '@shared/types'
import { DEFAULT_SETTINGS } from '@shared/types'

export function configDir(): string {
  // ESPSTATION_CONFIG_DIR lets headless dev/CI runs (and PISTATION-style
  // isolated smoke runs) point at a scratch directory instead of the
  // operator's real config, mirroring PiStation's PISTATION_USERDATA_DIR.
  return process.env['ESPSTATION_CONFIG_DIR'] || join(homedir(), '.config', 'espstation-desktop')
}

function settingsPath(): string {
  return join(configDir(), 'settings.json')
}

function isValidTheme(v: unknown): v is Settings['theme'] {
  return v === 'light' || v === 'dark' || v === 'system'
}

function isValidGatewayMode(v: unknown): v is Settings['gatewayMode'] {
  return v === 'managed' || v === 'attach'
}

/** Defensive parse: a hand-edited or partially-written settings.json must never crash the app. */
function sanitize(raw: unknown): Settings {
  const obj = typeof raw === 'object' && raw !== null ? (raw as Record<string, unknown>) : {}
  return {
    gatewayUrl: typeof obj['gatewayUrl'] === 'string' ? obj['gatewayUrl'] : DEFAULT_SETTINGS.gatewayUrl,
    gatewayToken: typeof obj['gatewayToken'] === 'string' ? obj['gatewayToken'] : DEFAULT_SETTINGS.gatewayToken,
    theme: isValidTheme(obj['theme']) ? obj['theme'] : DEFAULT_SETTINGS.theme,
    gatewayMode: isValidGatewayMode(obj['gatewayMode']) ? obj['gatewayMode'] : DEFAULT_SETTINGS.gatewayMode,
    gatewayPythonPath:
      typeof obj['gatewayPythonPath'] === 'string' ? obj['gatewayPythonPath'] : DEFAULT_SETTINGS.gatewayPythonPath
  }
}

export function loadSettings(): Settings {
  const path = settingsPath()
  if (!existsSync(path)) return { ...DEFAULT_SETTINGS }
  try {
    const raw = JSON.parse(readFileSync(path, 'utf8')) as unknown
    return sanitize(raw)
  } catch {
    // Corrupt file — fall back to defaults rather than fail startup.
    return { ...DEFAULT_SETTINGS }
  }
}

export function saveSettings(patch: SettingsPatch): Settings {
  const current = loadSettings()
  const merged = sanitize({ ...current, ...patch })
  mkdirSync(configDir(), { recursive: true })
  writeFileSync(settingsPath(), JSON.stringify(merged, null, 2), 'utf8')
  return merged
}

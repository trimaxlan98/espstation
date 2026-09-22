import type { GatewayLogLine, GatewayStatus, Settings, SettingsPatch } from './types'

/** IPC channel names, centralized so main and preload never drift on a string literal. */
export enum IpcChannel {
  SettingsGet = 'settings:get',
  SettingsSet = 'settings:set',
  AppVersion = 'app:version',
  GatewayStart = 'gateway:start',
  GatewayStop = 'gateway:stop',
  GatewayRestart = 'gateway:restart',
  GatewayStatus = 'gateway:status',
  GatewayLog = 'gateway:log',
  SketchSave = 'sketch:save',
  SketchCopy = 'sketch:copy'
}

/** What the renderer gets back after offering to save a sketch. */
export interface SketchSaveResult {
  saved: boolean
  path?: string
  reason?: string
}

/** The typed shape `contextBridge.exposeInMainWorld('espstation', ...)` exposes to the renderer. */
export interface EspStationBridge {
  settings: {
    get: () => Promise<Settings>
    set: (patch: SettingsPatch) => Promise<Settings>
  }
  app: {
    version: () => Promise<string>
  }
  gateway: {
    start: () => Promise<GatewayStatus>
    stop: () => Promise<GatewayStatus>
    restart: () => Promise<GatewayStatus>
    status: () => Promise<GatewayStatus>
    onLog: (cb: (line: GatewayLogLine) => void) => () => void
  }
  /**
   * The bench sketches, on their way out to the Arduino IDE. The renderer
   * supplies the text (it is bundled there, see lib/sketches.ts); main owns
   * the dialog, the filesystem and the clipboard, as it owns every other
   * Node/Electron primitive.
   */
  sketch: {
    save: (file: string, content: string) => Promise<SketchSaveResult>
    copy: (content: string) => Promise<void>
  }
}

declare global {
  interface Window {
    espstation: EspStationBridge
  }
}

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
  GatewayLog = 'gateway:log'
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
}

declare global {
  interface Window {
    espstation: EspStationBridge
  }
}

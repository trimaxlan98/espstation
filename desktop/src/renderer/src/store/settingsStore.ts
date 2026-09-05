import { create } from 'zustand'
import type { Settings, SettingsPatch } from '@shared/types'
import { DEFAULT_SETTINGS } from '@shared/types'
import { gatewayClient, gatewayStream } from '../lib/gatewaySingleton'

interface SettingsState {
  settings: Settings
  loaded: boolean
  saving: boolean
  error: string | null
  load: () => Promise<void>
  save: (patch: SettingsPatch) => Promise<void>
}

/** Reconfigures the shared gateway client/stream and reconnects the WS if the endpoint moved. */
function applyToGateway(settings: Settings): void {
  const before = gatewayClient.getConfig()
  gatewayClient.configure({ baseUrl: settings.gatewayUrl, token: settings.gatewayToken })
  const endpointChanged = before.baseUrl !== settings.gatewayUrl || before.token !== settings.gatewayToken
  if (endpointChanged && gatewayStream.getState() !== 'idle') {
    gatewayStream.disconnect()
    gatewayStream.connect()
  }
}

export const useSettingsStore = create<SettingsState>((set) => ({
  settings: DEFAULT_SETTINGS,
  loaded: false,
  saving: false,
  error: null,

  load: async () => {
    try {
      const settings = await window.espstation.settings.get()
      applyToGateway(settings)
      set({ settings, loaded: true, error: null })
    } catch (err) {
      set({ error: err instanceof Error ? err.message : String(err), loaded: true })
    }
  },

  save: async (patch: SettingsPatch) => {
    set({ saving: true })
    try {
      const settings = await window.espstation.settings.set(patch)
      applyToGateway(settings)
      set({ settings, saving: false, error: null })
    } catch (err) {
      set({ saving: false, error: err instanceof Error ? err.message : String(err) })
    }
  }
}))

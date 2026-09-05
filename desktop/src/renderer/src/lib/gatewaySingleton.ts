/**
 * One GatewayClient / GatewayStream per app instance, reconfigured in place
 * whenever settings change (see store/settingsStore.ts) rather than
 * recreated — every section imports these same instances so a Settings
 * change takes effect everywhere without prop-drilling a client reference.
 */
import { DEFAULT_SETTINGS } from '@shared/types'
import { GatewayClient, GatewayStream } from './gatewayClient'

export const gatewayClient = new GatewayClient({
  baseUrl: DEFAULT_SETTINGS.gatewayUrl,
  token: DEFAULT_SETTINGS.gatewayToken
})

export const gatewayStream = new GatewayStream(() => gatewayClient.streamUrl())

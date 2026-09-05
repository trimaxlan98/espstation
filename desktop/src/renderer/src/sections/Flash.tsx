import { EmptyState } from '../components/ui'

export function Flash(): React.JSX.Element {
  return (
    <div className="section">
      <div className="section__header">
        <h1 className="section__title">Flash</h1>
      </div>
      <EmptyState
        title="Arriving in sprint S5"
        description="PlatformIO build + flash + boot monitor for espstation-fw, target selection (esp32/esp32s3/esp32c3/esp32c6), and confirmation-gated OTA over WiFi once image signing exists (docs/ARCHITECTURE.md security model — LAN-only, confirmation-gated until then)."
      />
    </div>
  )
}

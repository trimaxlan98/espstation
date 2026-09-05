import { EmptyState } from '../components/ui'

export function Networks(): React.JSX.Element {
  return (
    <div className="section">
      <div className="section__header">
        <h1 className="section__title">Networks</h1>
      </div>
      <EmptyState
        title="Arriving in sprint S6"
        description="The multi-node topology graph (GET /api/network/topology), a peer loss/latency/RSSI matrix built from NET_REPORT, protocol-test benches (loss_latency, throughput, range_sweep, flood), and fault injection against simulated or real ESP-NOW meshes."
      />
    </div>
  )
}

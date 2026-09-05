import { EmptyState } from '../components/ui'

export function Lab(): React.JSX.Element {
  return (
    <div className="section">
      <div className="section__header">
        <h1 className="section__title">Lab</h1>
      </div>
      <EmptyState
        title="Arriving in sprint S8"
        description="Offline analysis: replay a completed run tick-by-tick from the gateway's SQLite (GET /api/runs/{id}/samples), overlay multiple runs on the same axes, and export CSV/Parquet for external tooling."
      />
    </div>
  )
}

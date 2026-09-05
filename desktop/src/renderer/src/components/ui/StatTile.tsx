import type { ReactNode } from 'react'
import type { StatusTone } from './types'
import './primitives.css'

export interface StatTileProps {
  label: string
  value: string | number
  unit?: string
  tone?: StatusTone
  spark?: ReactNode
  sub?: string
}

/** The metric tile — label, big mono value, optional unit/spark/sub-line. */
export function StatTile({ label, value, unit, tone = 'neutral', spark, sub }: StatTileProps): React.JSX.Element {
  return (
    <div className={`ui-stat-tile ui-stat-tile--${tone}`}>
      <span className="ui-stat-tile__label">{label}</span>
      <div className="ui-stat-tile__value-row">
        <span className="ui-stat-tile__value">{value}</span>
        {unit ? <span className="ui-stat-tile__unit">{unit}</span> : null}
      </div>
      {spark ? <div>{spark}</div> : null}
      {sub ? <span className="ui-stat-tile__sub">{sub}</span> : null}
    </div>
  )
}

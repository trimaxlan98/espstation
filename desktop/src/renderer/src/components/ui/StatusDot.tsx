import type { StatusTone } from './types'
import './primitives.css'

export interface StatusDotProps {
  tone: StatusTone
  label?: string
}

export function StatusDot({ tone, label }: StatusDotProps): React.JSX.Element {
  return <span className={`ui-status-dot ui-status-dot--${tone}`} role={label ? 'img' : undefined} aria-label={label} />
}

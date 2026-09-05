import type { PropsWithChildren } from 'react'
import type { StatusTone } from './types'
import './primitives.css'

export interface BadgeProps extends PropsWithChildren {
  tone?: StatusTone
  variant?: 'soft' | 'outline'
}

/** Small status/label chip. */
export function Badge({ tone = 'neutral', variant = 'soft', children }: BadgeProps): React.JSX.Element {
  return <span className={`ui-badge ui-badge--${variant} ui-badge--${tone}`}>{children}</span>
}

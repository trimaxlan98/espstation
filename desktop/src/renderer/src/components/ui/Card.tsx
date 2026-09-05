import type { PropsWithChildren, ReactNode } from 'react'
import { StatusDot } from './StatusDot'
import type { StatusTone } from './types'
import './primitives.css'

export interface CardProps extends PropsWithChildren {
  title?: string
  subtitle?: string
  actions?: ReactNode
  status?: StatusTone
  /** 'dense' tightens body padding; 'flush' removes it entirely (a Table filling the whole card). */
  bodyPadding?: 'default' | 'dense' | 'flush'
  className?: string
}

/** The general-purpose panel/card: header strip (title + optional status dot + actions slot) and a body. */
export function Card({
  title,
  subtitle,
  actions,
  status,
  bodyPadding = 'default',
  className,
  children
}: CardProps): React.JSX.Element {
  const hasHeader = Boolean(title || subtitle || actions || status)
  return (
    <section className={`ui-card ${className ?? ''}`}>
      {hasHeader ? (
        <header className="ui-card__header">
          {status ? <StatusDot tone={status} /> : null}
          <div className="ui-card__heading">
            {title ? <h2 className="ui-card__title">{title}</h2> : null}
            {subtitle ? <p className="ui-card__subtitle">{subtitle}</p> : null}
          </div>
          {actions ? <div className="ui-card__actions">{actions}</div> : null}
        </header>
      ) : null}
      <div
        className={`ui-card__body ${bodyPadding === 'dense' ? 'ui-card__body--dense' : ''} ${bodyPadding === 'flush' ? 'ui-card__body--flush' : ''}`}
      >
        {children}
      </div>
    </section>
  )
}

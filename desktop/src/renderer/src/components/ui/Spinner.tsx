import './primitives.css'

export interface SpinnerProps {
  size?: 'sm' | 'md' | 'lg'
  label?: string
}

/** Indeterminate loading indicator. `label` is visually hidden but announced. */
export function Spinner({ size = 'md', label = 'Loading' }: SpinnerProps): React.JSX.Element {
  return (
    <span className={`ui-spinner ui-spinner--${size}`} role="status" aria-live="polite">
      <svg viewBox="0 0 24 24" fill="none">
        <circle cx="12" cy="12" r="9.5" stroke="currentColor" strokeOpacity="0.2" strokeWidth="2.5" />
        <path d="M21.5 12a9.5 9.5 0 0 0-9.5-9.5" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" />
      </svg>
      <span className="ui-sr-only">{label}</span>
    </span>
  )
}

import type { ThemeSetting } from '@shared/types'
import './primitives.css'

export interface ThemeToggleProps {
  value: ThemeSetting
  onChange: (value: ThemeSetting) => void
}

const OPTIONS: Array<{ id: ThemeSetting; label: string; icon: React.JSX.Element }> = [
  {
    id: 'light',
    label: 'Light',
    icon: (
      <svg viewBox="0 0 16 16" fill="none" aria-hidden="true">
        <circle cx="8" cy="8" r="3.2" stroke="currentColor" strokeWidth="1.4" />
        <path
          d="M8 1.5v1.4M8 13.1v1.4M14.5 8h-1.4M2.9 8H1.5M12.4 3.6l-1 1M4.6 11.4l-1 1M12.4 12.4l-1-1M4.6 4.6l-1-1"
          stroke="currentColor"
          strokeWidth="1.4"
          strokeLinecap="round"
        />
      </svg>
    )
  },
  {
    id: 'system',
    label: 'System',
    icon: (
      <svg viewBox="0 0 16 16" fill="none" aria-hidden="true">
        <rect x="1.5" y="2.5" width="13" height="8.5" rx="1" stroke="currentColor" strokeWidth="1.3" />
        <path d="M5.5 14h5M8 11v3" stroke="currentColor" strokeWidth="1.3" strokeLinecap="round" />
      </svg>
    )
  },
  {
    id: 'dark',
    label: 'Dark',
    icon: (
      <svg viewBox="0 0 16 16" fill="none" aria-hidden="true">
        <path
          d="M13.8 9.7A5.8 5.8 0 0 1 6.3 2.2a5.8 5.8 0 1 0 7.5 7.5Z"
          stroke="currentColor"
          strokeWidth="1.4"
          strokeLinejoin="round"
        />
      </svg>
    )
  }
]

/** Tri-state light/system/dark switch — drives `Settings.theme`, which `ThemeProvider` resolves. */
export function ThemeToggle({ value, onChange }: ThemeToggleProps): React.JSX.Element {
  return (
    <div className="ui-theme-toggle" role="radiogroup" aria-label="Theme">
      {OPTIONS.map((opt) => (
        <button
          key={opt.id}
          type="button"
          role="radio"
          aria-checked={value === opt.id}
          aria-label={opt.label}
          title={opt.label}
          className={`ui-theme-toggle__option ${value === opt.id ? 'ui-theme-toggle__option--active' : ''}`}
          onClick={() => onChange(opt.id)}
        >
          {opt.icon}
        </button>
      ))}
    </div>
  )
}

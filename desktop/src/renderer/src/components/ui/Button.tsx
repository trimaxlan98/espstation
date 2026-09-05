import type { ButtonHTMLAttributes, ReactNode } from 'react'
import { Spinner } from './Spinner'
import type { ControlSize } from './types'
import './primitives.css'

export type ButtonVariant = 'primary' | 'default' | 'ghost' | 'danger' | 'subtle'

export interface ButtonProps extends Omit<ButtonHTMLAttributes<HTMLButtonElement>, 'type'> {
  variant?: ButtonVariant
  size?: ControlSize
  loading?: boolean
  iconLeft?: ReactNode
  iconRight?: ReactNode
  fullWidth?: boolean
  type?: 'button' | 'submit' | 'reset'
}

/** The base action button. `loading` swaps `iconLeft` for a Spinner and disables the button. */
export function Button({
  variant = 'default',
  size = 'md',
  loading = false,
  iconLeft,
  iconRight,
  fullWidth = false,
  className,
  disabled,
  children,
  type = 'button',
  ...rest
}: ButtonProps): React.JSX.Element {
  const classes = [
    'ui-btn',
    `ui-btn--${variant}`,
    `ui-btn--${size}`,
    fullWidth ? 'ui-btn--full' : '',
    className ?? ''
  ]
    .filter(Boolean)
    .join(' ')

  return (
    <button type={type} className={classes} disabled={disabled || loading} {...rest}>
      {loading ? (
        <Spinner size="sm" />
      ) : iconLeft ? (
        <span className="ui-btn__icon" aria-hidden="true">
          {iconLeft}
        </span>
      ) : null}
      {children ? <span>{children}</span> : null}
      {!loading && iconRight ? (
        <span className="ui-btn__icon" aria-hidden="true">
          {iconRight}
        </span>
      ) : null}
    </button>
  )
}

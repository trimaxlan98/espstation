import type { InputHTMLAttributes, PropsWithChildren, ReactNode, SelectHTMLAttributes } from 'react'
import { useId } from 'react'
import './primitives.css'

export interface FieldProps extends PropsWithChildren {
  label: string
  hint?: string
  htmlFor?: string
}

/** Label + control + optional hint, with the label properly associated via `htmlFor`. */
export function Field({ label, hint, htmlFor, children }: FieldProps): React.JSX.Element {
  return (
    <div className="ui-field">
      <label className="ui-field__label" htmlFor={htmlFor}>
        {label}
      </label>
      {children}
      {hint ? <span className="ui-field__hint">{hint}</span> : null}
    </div>
  )
}

export function Input(props: InputHTMLAttributes<HTMLInputElement>): React.JSX.Element {
  const autoId = useId()
  const { id = autoId, className, ...rest } = props
  return <input id={id} className={`ui-input ${className ?? ''}`} {...rest} />
}

export interface SelectOption {
  value: string
  label: string
}

export interface SelectProps extends Omit<SelectHTMLAttributes<HTMLSelectElement>, 'children'> {
  options: SelectOption[]
  placeholder?: ReactNode
}

export function Select({ options, placeholder, className, ...rest }: SelectProps): React.JSX.Element {
  const autoId = useId()
  const { id = autoId } = rest
  return (
    <select id={id} className={`ui-select ${className ?? ''}`} {...rest}>
      {placeholder ? (
        <option value="" disabled>
          {placeholder}
        </option>
      ) : null}
      {options.map((opt) => (
        <option key={opt.value} value={opt.value}>
          {opt.label}
        </option>
      ))}
    </select>
  )
}

import type { PropsWithChildren, ThHTMLAttributes, TdHTMLAttributes, HTMLAttributes } from 'react'
import './primitives.css'

export interface TableProps extends PropsWithChildren {
  zebra?: boolean
  dense?: boolean
  className?: string
}

/** Styled `<table>` with a sticky `<thead>`. Wrap in a scrolling container for the sticky-header effect. */
export function Table({ zebra = false, dense = false, className, children }: TableProps): React.JSX.Element {
  const classes = ['ui-table', zebra ? 'ui-table--zebra' : '', dense ? 'ui-table--dense' : '', className ?? '']
    .filter(Boolean)
    .join(' ')
  return <table className={classes}>{children}</table>
}

export function THead({ children, ...rest }: PropsWithChildren<HTMLAttributes<HTMLTableSectionElement>>): React.JSX.Element {
  return <thead {...rest}>{children}</thead>
}

export function TBody({ children, ...rest }: PropsWithChildren<HTMLAttributes<HTMLTableSectionElement>>): React.JSX.Element {
  return <tbody {...rest}>{children}</tbody>
}

export function TR({ children, ...rest }: PropsWithChildren<HTMLAttributes<HTMLTableRowElement>>): React.JSX.Element {
  return <tr {...rest}>{children}</tr>
}

export interface THProps extends ThHTMLAttributes<HTMLTableCellElement> {
  align?: 'left' | 'right' | 'center'
}

export function TH({ align = 'left', className, children, ...rest }: THProps): React.JSX.Element {
  return (
    <th className={`ui-th ui-th--${align} ${className ?? ''}`} {...rest}>
      {children}
    </th>
  )
}

export interface TDProps extends TdHTMLAttributes<HTMLTableCellElement> {
  align?: 'left' | 'right' | 'center'
  /** Right-aligns + sets mono font — for numeric/telemetry cell values. */
  numeric?: boolean
}

export function TD({ align, numeric = false, className, children, ...rest }: TDProps): React.JSX.Element {
  const resolvedAlign = align ?? (numeric ? 'right' : 'left')
  const classes = ['ui-td', `ui-td--${resolvedAlign}`, numeric ? 'ui-td--numeric' : '', className ?? '']
    .filter(Boolean)
    .join(' ')
  return (
    <td className={classes} {...rest}>
      {children}
    </td>
  )
}

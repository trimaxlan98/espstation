import type { HTMLAttributes, PropsWithChildren } from 'react'
import './primitives.css'

export interface ToolbarProps extends PropsWithChildren<HTMLAttributes<HTMLDivElement>> {}

/** Horizontal action strip. Insert `<ToolbarSpacer />` to push trailing items to the right. */
export function Toolbar({ className, children, ...rest }: ToolbarProps): React.JSX.Element {
  return (
    <div className={`ui-toolbar ${className ?? ''}`} {...rest}>
      {children}
    </div>
  )
}

export function ToolbarSpacer(): React.JSX.Element {
  return <div className="ui-toolbar__spacer" />
}

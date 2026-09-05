import { EmptyState } from '../components/ui'

export function Experiments(): React.JSX.Element {
  return (
    <div className="section">
      <div className="section__header">
        <h1 className="section__title">Experiments</h1>
      </div>
      <EmptyState
        title="Arriving in sprint S3"
        description="The declarative experiment designer: build a spec (channels, triggers, network block), validate it against a node's NDB and rate budget, push it to one or many nodes with EXP_SET, and browse run records with side-by-side comparison. See docs/EXPERIMENTS.md for the spec this section will edit."
      />
    </div>
  )
}

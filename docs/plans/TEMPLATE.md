# S<n> — <theme>

> Copy this file to `docs/plans/S<n>-<slug>.md` and fill it in **before**
> dispatching any builder. A vague spec produces vague code; the time spent
> here is repaid several times over.

## Goal

One sentence. What can a person do after this sprint that they could not before?

## Why now

Which risk does this retire, or which later sprint does it unblock? If the
answer is "it seemed like the next thing", reorder.

## Invariants that apply

List the specific rules from `AGENTS.md` and `docs/DECISIONS.md` this work could
plausibly break, and how it will not. Naming them makes a reviewer's job
concrete instead of impressionistic.

## Modules

For each, one table row: which component, which agent role
(`builder` / `firmware-specialist`), and whether it can run in parallel with
the others.

| Module | Component | Role | Depends on |
|---|---|---|---|

## Specs

One subsection per module — this is what gets pasted to the builder.

### M<n> — <name>
- **Files:** exactly which to create or modify.
- **Contract:** what it must honour (protocol sections, API endpoints, existing
  interfaces). Quote the section, do not paraphrase.
- **Tests:** which cases, named. Include the failure paths, not only the happy
  path — the failure paths are the product here.
- **Verification command:** the exact command whose output the builder must
  paste.
- **Out of scope:** what it must NOT touch. Be explicit; builders expand scope
  when the boundary is vague.

## Definition of done

Verifiable checks, each one a command or an observable behaviour on real
hardware. Not "the feature works" — *how you will know*.

- [ ]

## Risks and unknowns

What could make this sprint fail or take twice as long, and what would settle
each question early. Say plainly what cannot be verified without hardware.

## Decisions expected

Choices this sprint will force. They get `D-n` entries in
`docs/DECISIONS.md` when made — with the reason *and* the consequence.

## What and why

<!-- What changes, and what problem it solves. If it implements part of a
sprint, link the brief in docs/plans/. -->

## The unplug test

<!-- Required for anything touching firmware, the protocol or the link.
What happens to a node running this code when the cable is pulled? "Not
applicable" is a valid answer — say so and why. -->

## Verification

<!-- Paste the REAL output of the suites you ran. "Tests pass" is not evidence.
Say what you could NOT verify (no hardware, no second board, etc.) — an
unverified claim someone builds on costs more than an admitted gap. -->

```
```

## Checklist

- [ ] The suites covering my area pass, and their output is pasted above.
- [ ] If I changed the wire format, I changed **all four** places in this PR:
      `protocol/PROTOCOL.md`, `protocol/espstation.protocol.yaml`, the firmware
      codec, the gateway codec — and `tools/check_protocol.py` is green.
- [ ] If I made a choice a future contributor could reasonably have made
      differently, I appended a `D-n` entry to `docs/DECISIONS.md` with its
      reason **and** its consequence.
- [ ] New behaviour has tests covering its **failure** paths, not only the
      happy path.
- [ ] No packed struct is built by casting a pointer.
- [ ] Mutating actions go through a UI confirmation.

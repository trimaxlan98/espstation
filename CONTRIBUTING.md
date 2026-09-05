# Contributing to EspStation

**If you are an AI coding agent, read [`AGENTS.md`](AGENTS.md) instead —** it is
written for you and it is the authoritative version of everything below.

## Before you start

1. [`docs/SETUP.md`](docs/SETUP.md) — get the suites running. You do not need an
   ESP32; the simulator speaks the real protocol.
2. [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — what each component owns.
3. [`docs/DECISIONS.md`](docs/DECISIONS.md) — **check here before proposing a
   change to an existing choice.** If your idea is already recorded with a
   reason, argue against the reason rather than restating the idea.
4. [`docs/ROADMAP.md`](docs/ROADMAP.md) — which sprint your work belongs to.

## The invariant

> **The node is autonomous; the station is a laboratory.**

Before writing a feature, answer: *what happens when the cable is unplugged?*
If the answer is "it stops working", the design is wrong. Every other rule here
descends from this one.

## Hard rules

1. **`protocol/PROTOCOL.md` is law.** Changing the wire format means changing
   the prose spec, `espstation.protocol.yaml`, the firmware codec and the
   gateway codec **in one commit**. `tools/check_protocol.py` gates it.
2. **`esps_proto` stays pure C11** — only `<stdint.h> <stddef.h> <string.h>
   <stdbool.h>`, no allocation, no ESP-IDF headers, so it stays host-testable.
3. **Never cast a pointer to build a packed struct.** Explicit byte writes. Host
   and target disagree about padding, and the resulting bug appears only on
   hardware.
4. **One codec.** The simulator and the real path share it. Never write a second
   implementation for tests.
5. **Every mutating action is confirmed in the UI.**
6. **The renderer never touches Node APIs** — everything privileged goes through
   the typed preload bridge.
7. **Log your reasoning.** Any choice a future contributor could reasonably have
   made differently gets a `D-n` entry in `docs/DECISIONS.md`, with its reason
   *and* its consequence.

## Pull requests

- One coherent change per PR. A protocol change plus a UI feature is two PRs,
  unless the UI feature is what forced the protocol change.
- Paste the **real output** of the suites you ran. "Tests pass" is not evidence.
- Say what you did *not* do, and what you could not verify. An unverified
  timing or radio claim is worse than an admitted gap — someone will build on it.
- New behaviour comes with tests that cover its failure paths, not only its
  happy path. The failure paths are the product here: dropped links, truncated
  frames, full storage, a sensor that stops responding.

## Style

- Comments explain *why*, and only where a reader would otherwise wonder. Do not
  narrate what the code plainly says.
- C11 · Python 3.11-compatible with type hints · TypeScript strict, no `any`
  without a comment justifying it.
- Match the surrounding code. Consistency beats personal preference.

## Testing philosophy

The suites exist because this system fails in ways that are expensive to
diagnose after the fact: a node deployed somewhere inconvenient, holding a spec
it cannot run, with a link that dropped six hours ago. Test the boundaries —
COBS block edges at 254 and 255 bytes, `uint32` millisecond wraparound,
sequence-number wrap, a frame split across every possible chunk offset, storage
that fills, a `TELEM_ACK` for data that was never committed.

If you find a bug in one of those, add the test that would have caught it
before you fix it.

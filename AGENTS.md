# AGENTS.md — working on EspStation

Entry point for AI coding agents (Claude Code, Codex, Cursor, Aider, and
whatever comes next) and for humans who want the short version. Read this
before touching anything.

## What this is

EspStation is a workbench for **ESP32 experiments and node networks**. Three
components, one repo:

| Path | What | Language |
|---|---|---|
| `firmware/` | `espstation-fw`, runs on the ESP32 | C11 / ESP-IDF 5.x via PlatformIO |
| `gateway/` | `espstation-gateway`, holds the ports, speaks the protocol, serves REST+WS | Python 3.11+ / FastAPI |
| `desktop/` | `espstation-desktop`, the operator's app | TypeScript / Electron + React |
| `protocol/` | the wire contract between all three | Markdown + YAML + JSON Schema |

## The one idea you must not violate

> **The node is autonomous; the station is a laboratory.**

The experiment lives on the ESP32, in NVS, and runs with or without a station
connected. The station observes, configures and orchestrates. It is **never in
a control loop.**

Before you write a feature, answer: *what happens when the cable is unplugged?*
If the answer is "it stops working", the design is wrong. This is [D-1] and
every other invariant descends from it.

## Read these before writing code

1. **[`protocol/PROTOCOL.md`](protocol/PROTOCOL.md) — this is law.** Byte
   offsets, CRC parameters, framing, message table. Nothing may deviate.
2. [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — what each component owns.
3. [`docs/EXPERIMENTS.md`](docs/EXPERIMENTS.md) — the experiment spec.
4. [`docs/DECISIONS.md`](docs/DECISIONS.md) — why things are the way they are.
   **Check here before proposing a change to an existing choice.** If your idea
   is already there with a reason, argue against the reason, not the choice.
5. [`docs/ROADMAP.md`](docs/ROADMAP.md) — which sprint your work belongs to.
6. [`SPRINT_STATUS.md`](SPRINT_STATUS.md) — what is happening right now, and
   the resume point if a session was interrupted.

## Hard rules

1. **Protocol changes are atomic across four places, in one commit:**
   `protocol/PROTOCOL.md`, `protocol/espstation.protocol.yaml`,
   `firmware/components/esps_proto/`, and
   `gateway/espstation_gateway/protocol/` (+ the desktop's `apiTypes.ts`).
   `tools/check_protocol.py` fails CI otherwise. Never "just add a field".
2. **`esps_proto` stays pure C11.** Only `<stdint.h> <stddef.h> <string.h>
   <stdbool.h>`. No ESP-IDF headers, no allocation, no logging. It must keep
   compiling with plain gcc so the host test suite can gate it. [D-4]
3. **Never build packed structs by casting a pointer.** Explicit byte writes.
   Host and target disagree about alignment and padding, and the bug that
   causes appears only on hardware.
4. **The simulator and the real path share one codec.** Never write a second
   implementation "just for tests". [D-8]
5. **Every mutating action is confirmed in the UI** — flash, erase, reboot,
   `EXP_SET`, `store.erase`, detach. [D-15]
6. **The renderer never touches Node APIs.** Everything privileged goes through
   the typed `contextBridge` preload.
7. **Timestamps:** `uint32` monotonic ms on the node, float Unix epoch seconds
   from the gateway outward. Exactly one conversion point. [D-10]
8. **Never commit unless you are the orchestrator** and were asked to. Sub-agents
   report; the orchestrator commits.
9. **No placeholder code presented as finished.** A stub is fine when it is
   labelled `TODO(Sn)` and the report says so. Silently returning fake data is
   not.

## How to run everything

See [`docs/SETUP.md`](docs/SETUP.md) for first-time setup (including the
`dialout` group, which you will hit immediately). Day to day:

```bash
# Firmware — host tests (no ESP32 needed, run these constantly)
make -C firmware/test/host test

# Firmware — build / flash / monitor (needs the toolchain, see SETUP.md)
.venv-tools/bin/pio run -d firmware -e esp32dev
.venv-tools/bin/pio run -d firmware -e esp32dev -t upload
tools/enlp_sniff.py /dev/ttyUSB0            # decoded frame dump

# Gateway
cd gateway && .venv/bin/python -m pytest tests/ -q
.venv/bin/python -m espstation_gateway --sim --port 8787   # simulated nodes
.venv/bin/python -m espstation_gateway --serial /dev/ttyUSB0

# Desktop (needs a gateway running)
cd desktop && npm run typecheck && npm test && npm run build
npm run dev

# Protocol drift gate
python3 tools/check_protocol.py
```

**You do not need hardware to contribute.** The gateway's simulated nodes speak
the real protocol through the real codec, so the whole desktop and most of the
gateway can be built and verified with `--sim`. [D-8]

## Definition of done for any change

- The suites that cover your area pass, and you pasted the real output.
- If you touched the protocol, all four places changed and
  `tools/check_protocol.py` is green.
- If you made a choice a future contributor could reasonably have made
  differently, you appended a `D-n` entry to `docs/DECISIONS.md`.
- If the change ends a work session, `SPRINT_STATUS.md` reflects reality —
  including what is *not* done.

## Reporting

Finish with a compact report: files created or modified, the exact commands you
ran and their real output, deviations from the spec you were given and why, and
anything the next agent must know. **Report failures as failures.** A test you
could not run is not a test that passed, and saying so costs nothing compared
to the cost of someone finding out later.

## Agent roles

Pre-defined roles live in `.claude/agents/` (Claude Code) and `.codex/agents/`
(Codex); they are the same roles in two formats.

| Role | Model | Job |
|---|---|---|
| `orchestrator` | Opus-class | Splits work into specs, dispatches builders, verifies, commits. The only role that touches git. |
| `builder` | Sonnet-class | Implements one module from a precise spec. Writes and runs its tests. Never commits. |
| `reviewer` | Opus-class | Read-only adversarial audit. Reports findings ranked by severity; never fixes them itself. |
| `firmware-specialist` | Opus-class | Embedded-specific work: timing, memory, RTOS, radio, power. |

The working cycle that produced this repo, and the one to keep using:
**spec → builder → orchestrator verifies against reality → reviewer → fix →
atomic commit.** The step people skip is "verifies against reality" — running
the thing, not just the tests.

[D-1]: docs/DECISIONS.md
[D-4]: docs/DECISIONS.md
[D-8]: docs/DECISIONS.md
[D-10]: docs/DECISIONS.md
[D-15]: docs/DECISIONS.md

---
name: espstation
description: How to work on the EspStation monorepo — the ESP32 experiment and node-network workbench. Architecture, invariants, how to run the simulator, firmware, gateway and desktop, and this machine's environment gotchas. Use when developing, debugging or extending any part of EspStation (firmware, gateway, desktop, protocol, docs).
---

# Working on EspStation

Three components, one contract: `firmware/` (C11/ESP-IDF on the ESP32),
`gateway/` (Python/FastAPI on the host, owns the ports and the database),
`desktop/` (Electron+React operator app). `protocol/PROTOCOL.md` is **law**.
Read `AGENTS.md` first — it is the short version of everything here.

## The invariant everything descends from

**The node is autonomous; the station is a laboratory.** The experiment lives
on the ESP32 in NVS and runs with or without a station. Before writing any
feature, answer *what happens when the cable is unplugged?* If the answer is
"it stops working", the design is wrong (D-1).

## Key design invariants
- Protocol changes are atomic across four places in ONE commit: `protocol/PROTOCOL.md`, `protocol/espstation.protocol.yaml`, `firmware/components/esps_proto/`, `gateway/espstation_gateway/protocol/` (+ desktop `apiTypes.ts`). `tools/check_protocol.py` gates it.
- `esps_proto` is pure C11 — only `<stdint.h> <stddef.h> <string.h> <stdbool.h>`, no allocation, no ESP-IDF headers — so it stays host-testable with gcc+ASan (D-4).
- Never build packed structs by casting a pointer; explicit byte writes only. Host and target disagree on padding.
- Simulated nodes are the same gateway with a different transport, sharing one codec — never a second implementation (D-8).
- Every mutating action is confirmed in the UI (D-15). The gate is station-side; the node trusts the link.
- Renderer never touches Node APIs — everything privileged goes through the typed `contextBridge` preload.
- Time: `uint32` monotonic ms on the node, float Unix epoch seconds from the gateway outward, one conversion point (D-10).
- Log autonomous choices as `D-n` in `docs/DECISIONS.md`, with reason *and* consequence.

## Run things
```bash
make -C firmware/test/host test                     # codec tests, no hardware, seconds
cd gateway && .venv/bin/python -m pytest tests/ -q
.venv/bin/python -m espstation_gateway --sim --port 8787    # simulated nodes
cd desktop && npm run typecheck && npm test && npm run build
npm run dev                                          # needs a gateway up
python3 tools/check_protocol.py                      # protocol drift gate
python3 tools/sync_agents.py --check                 # .codex mirrors .claude

.venv-tools/bin/pio run -d firmware -e esp32dev              # build
.venv-tools/bin/pio run -d firmware -e esp32dev -t upload    # flash
tools/enlp_sniff.py /dev/ttyUSB0                             # decoded frame dump
```

## Environment gotchas (this dev machine)
- `python3 -m venv` FAILS (Python 3.14, no `ensurepip`). Use `~/.local/bin/virtualenv .venv`. Target **3.11** compatibility anyway.
- **The user is not in the `dialout` group**, so `/dev/ttyUSB0` (`root:dialout 660`) cannot be opened — the symptom is a node that never appears. `sudo usermod -aG dialout $USER` then **log out and back in**. See `docs/SETUP.md` §5.
- No system `cmake`; PlatformIO vendors its own. Do not install one to "fix" a build.
- PlatformIO lives in `.venv-tools/`, not on `PATH`.
- Electron in this sandbox needs `ELECTRON_DISABLE_SANDBOX=1` plus dev-only `--no-sandbox`/`--disable-gpu` switches (already wired, same as PiStation). GPU noise in dev logs is environmental, not app errors.
- Headless UI verification: the renderer console is relayed to stdout as `[renderer:LEVEL] …`. Capture `npm run dev` to a FILE (`> log 2>&1`) — piping to grep loses lines to buffering. Success criterion: `grep -c renderer:ERROR log` → 0.
- The board on the bench is an **ESP32 WROOM DevKit** behind a CP2102. S3/C3/C6 environments exist but no hardware has validated them (D-13).

## Workflow conventions
- Roles in `.claude/agents/`: `orchestrator` (opus, plans + verifies + the only one that commits), `builder` (sonnet, implements from a spec), `reviewer` (opus, read-only adversarial audit), `firmware-specialist` (opus, timing/memory/RTOS/radio/power). `.codex/agents/*.toml` is **generated** — edit the Markdown and run `tools/sync_agents.py`.
- Cycle per module: spec → builder → **orchestrator verifies against reality** (run the thing, not just the tests) → reviewer → fix → atomic commit.
- `SPRINT_STATUS.md` is the crash-recovery log; keep it current mid-session, and record what is *not* done as carefully as what is. Long autonomous sessions lose agents to API failures and spend limits — frequent atomic commits are the safety net.

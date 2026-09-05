# Roadmap — sprints

Each sprint is a **vertical slice**: firmware + gateway + desktop move together
and end in something you can actually run. A sprint is done when its
*Definition of done* passes on this machine, not when the code compiles.

The sprints are ordered so that the riskiest assumption is tested first. That
assumption is not "can we write a UI" — it is **"does the link survive real
hardware, real noise, and being unplugged."** Everything else is downstream of
that, so S1 goes to real silicon before a single chart is drawn.

| Sprint | Theme | Status |
|---|---|---|
| [S0](#s0--foundation) | Foundation: contracts, scaffolding, CI, agent conventions | **in progress** |
| [S1](#s1--the-link-is-real) | The link is real — firmware on hardware, live telemetry | next |
| [S2](#s2--the-station) | The station — desktop shell, Nodes + Live | |
| [S3](#s3--declarative-experiments) | Declarative experiments end to end | |
| [S4](#s4--autonomy) | Autonomy — store-and-forward, unplug and walk away | |
| [S5](#s5--flash--ota) | Flash & OTA from the app | |
| [S6](#s6--networks) | Networks — ESP-NOW, topology, protocol benches | |
| [S7](#s7--sensors) | Sensors & drivers — the channel registry grows | |
| [S8](#s8--laboratory) | Laboratory — replay, comparison, export | |
| [S9](#s9--iot--copilot) | IoT integration & AI copilot | |
| [S10](#s10--aerospace) | Aerospace — CCSDS bridge to PiStation | |

---

## S0 — Foundation

**Goal.** A repository another agent can walk into and contribute to without
asking a question.

**Deliverables.** ENLP protocol spec (prose + machine-readable) · experiment
spec + JSON Schema · architecture · this roadmap · decision log · agent
conventions (`.claude/agents`, `.codex/agents`, `AGENTS.md`, skills) ·
firmware skeleton with a host-testable pure-C codec · gateway package with
simulated nodes and a REST/WS API · desktop shell with the Nodes and Live
sections · CI running every suite.

**Definition of done.**
- `make -C firmware/test/host test` green.
- `gateway/.venv/bin/python -m pytest tests/ -q` green.
- `cd desktop && npm run typecheck && npm test && npm run build` green.
- `python -m espstation_gateway --sim` serves `/api/nodes` with simulated nodes.
- `tools/check_protocol.py` green.
- Repo pushed to GitHub with CI passing.

---

## S1 — The link is real

**Goal.** An ESP32 on the bench streams telemetry into the gateway over USB,
and survives being unplugged and replugged.

**Why first.** Every later sprint assumes the link works under real conditions.
Serial framing against boot-ROM noise, CRC behaviour on a marginal cable, and
reconnection are exactly the things that look fine in a simulator and fail on a
desk. Find that out now, not in S6 with twenty nodes.

**Deliverables.**
- Firmware: real `HELLO`/`HEARTBEAT`/`TELEMETRY`/`LOG`/`CMD` over UART on
  `esp32dev`, with the log hook and the system channels.
- Gateway: serial transport proven against the real board; raw boot-ROM output
  surfaced, not dropped.
- `tools/enlp_sniff.py` — a CLI that dumps decoded frames from a port. The
  debugging tool you will want at 2 a.m.
- **Fix the `dialout` group** (see `docs/SETUP.md`) and document it.

**Definition of done.** Flash the connected board; `enlp_sniff.py` shows a
valid `HELLO` with the NDB, 1 Hz heartbeats and telemetry; `node.ping` gets a
`CMD_ACK` under 100 ms; unplug and replug reconnects with no gateway restart;
a full boot log (including the ROM banner) is visible.

---

## S2 — The station

**Goal.** The desktop app is how you use EspStation, not `curl`.

**Deliverables.** Nodes section against real + simulated nodes side by side ·
Live dashboard with NDB-driven charts, log pane and event rail · history from
the gateway's SQLite with zoom · connection state and reconnection handled
visibly · light/dark themes · AppImage/`.deb` packaging.

**Definition of done.** Launch the app with the simulator, watch a 3-node
network; attach the real board and see it appear; kill the gateway and watch
the UI degrade honestly and recover; `npm run dist` produces a launchable
AppImage.

---

## S3 — Declarative experiments

**Goal.** The core promise: reconfigure a node by editing a document.

**Deliverables.** `esps_experiment` runtime on the node (spec from NVS,
scheduler, triggers, actions, lifecycle) · all three validation gates from
`docs/EXPERIMENTS.md` · Experiments section: editor with schema validation,
push to N nodes, run control, run records · run comparison in the gateway's
store.

**Definition of done.** Author a spec in the UI, push it to the real board and
two simulated ones, watch all three run it; a trigger fires and is visible as
an event; power-cycle the board and the experiment resumes from NVS; push an
invalid spec and confirm the previous one survives.

---

## S4 — Autonomy

**Goal.** Unplug it and walk away. This is the sprint the product is named for.

**Deliverables.** `esps_store`: RAM ring + LittleFS persistence, the
`TELEM_ACK` durability watermark, drain-on-reconnect interleaved with live data
· `standalone: true` production mode with duty cycling · brownout and
watchdog-reset accounting · degraded-mode behaviour when a driver fails.

**Definition of done.** Start a run, unplug the USB cable for ten minutes, plug
it back in: **zero samples lost**, backfill visible in the chart, the live view
never stalls during the drain. Pull power mid-run and confirm the run resumes
and the persisted samples survive.

---

## S5 — Flash & OTA

**Goal.** Never leave the app to run a build.

**Deliverables.** PlatformIO build + flash driven from the desktop (progress,
errors parsed into the UI) · boot monitor · OTA over WiFi with a signed image ·
firmware version tracking per node · rollback on failed boot (the `ota_0`/
`ota_1` partitions are already in place for this).

**Definition of done.** Build and flash the connected board from the UI; push
an OTA update to a WiFi node and have it come back on the new version; force a
bad image and watch it roll back.

---

## S6 — Networks

**Goal.** The multi-node reason this project exists.

**Deliverables.** `esps_net`: ESP-NOW peer discovery, roles, link-quality
accounting · network experiment block in the spec · protocol test benches
(loss/latency, throughput, range sweep, flood) · Networks section: topology
graph, peer matrix, loss/latency/RSSI heatmaps, fault injection · the simulated
network at 20+ nodes with configurable loss.

**Definition of done.** Three real boards form a mesh and report a peer table
that matches reality; a loss/latency bench runs and its numbers track a
deliberately degraded link; the same experiment runs on 20 simulated nodes.

---

## S7 — Sensors

**Goal.** Measure real things without touching station code.

**Deliverables.** Channel provider registry with I2C/SPI/ADC drivers (BME280,
MPU6050, INA219, DS18B20, generic ADC) · per-channel calibration and scaling ·
hot-plug: a new driver appears in the NDB and charts itself · a documented
"add a driver in 50 lines" guide.

**Definition of done.** Wire a real sensor, add its driver, and see it charted
in the UI **with no desktop or gateway change**. That constraint is the test.

---

## S8 — Laboratory

**Goal.** Answer questions about runs after they are over.

**Deliverables.** Run replay at variable speed · overlay and compare runs ·
statistics and derived channels · export CSV / Parquet / JSON · annotations
and marks on the timeline · session reports.

---

## S9 — IoT & copilot

**Deliverables.** MQTT bridge (publish telemetry, subscribe commands) ·
Home Assistant discovery · InfluxDB/Prometheus export · AI copilot over the
`claude` CLI with propose-then-approve and a full audit trail, exactly as
PiStation does it (`pistation/docs/COPILOT.md` is the reference).

---

## S10 — Aerospace

**Goal.** The ESP32 network becomes a payload segment for PiStation's ground
segment.

**Deliverables.** CCSDS Space Packet encapsulation of node telemetry · a bridge
that feeds PiStation's existing CCSDS pipeline (and therefore Yamcs and
Open MCT) · FDIR: watchdog chain, safe mode, limit monitoring on the node ·
CubeSat subsystem emulation profiles (EPS, ADCS, thermal) · time correlation
against the ground segment.

**Definition of done.** An ESP32 running a thermal profile appears as a
telemetry source in PiStation's mission console, through real CCSDS packets.

---

## Standing constraints

These hold in every sprint and a change to any of them is a decision that must
be logged in [DECISIONS.md](DECISIONS.md):

1. The node runs without the station. No exceptions, no "just for this feature".
2. `protocol/PROTOCOL.md` is law; changing the wire format touches the spec and
   all three implementations in one commit.
3. Every mutating action is confirmed in the UI.
4. Everything is demoable with zero hardware, via the simulator.
5. The simulator and the real path share one codec. Never two implementations.
6. `esps_proto` stays pure C with no ESP-IDF dependency, so it stays
   host-testable.

## Hardware backlog

`esp32dev` (WROOM, CP2102) is on the bench. ESP32-S3, ESP32-C3/C6 and LoRa
boards are to be acquired. Build environments for all of them exist in
`platformio.ini` from S0 — **untested until the silicon arrives**, and marked
as such rather than claimed as supported.

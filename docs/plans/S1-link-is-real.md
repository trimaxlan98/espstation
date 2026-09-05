# S1 — The link is real

**Status:** ready to execute. Prerequisite: S0 definition of done passes.

## Goal

An ESP32 on the bench streams telemetry into the gateway over USB, answers
commands, and survives being unplugged and replugged — with no gateway restart
and no lost boot log.

## Why now

Every later sprint assumes the link works under real conditions. Serial framing
against boot-ROM noise, CRC behaviour on a marginal cable, and reconnection are
precisely the things that look fine in a simulator and fail on a desk. The UI
is not the risk; the link is. Find out now, not in S6 with twenty nodes.

## Invariants that apply

- **D-1 (autonomy).** The firmware must reach a running state and keep sampling
  whether or not `HELLO_ACK` ever arrives. A state machine that stalls waiting
  for the station is the exact failure this sprint must prove absent.
- **D-4 (`esps_proto` purity).** The link layer may depend on the codec; the
  codec may never depend on ESP-IDF.
- **D-5 (shared UART).** Undecodable bytes go to the raw path, never to
  `/dev/null`. The boot-ROM banner reaching the UI is a test case, not a
  side effect.
- **Non-blocking transmit.** Every send path drops-and-flags when its queue is
  full. A blocking write on a disconnected port stalls the sampler, which
  breaks D-1 quietly.

## Modules

| Module | Component | Role | Depends on |
|---|---|---|---|
| M1 — `esps_link` UART + reconnection | firmware | `firmware-specialist` | S0 codec |
| M2 — `esps_core` health, log hook, time sync | firmware | `firmware-specialist` | M1 interface |
| M3 — serial transport hardening | gateway | `builder` | S0 codec |
| M4 — `tools/enlp_sniff.py` | tools | `builder` | S0 gateway codec |

M3 and M4 run in parallel with M1/M2.

## Specs

### M1 — `esps_link` UART transport and reconnection

- **Files:** `firmware/components/esps_link/{include/esps_link.h,src/link.c,src/link_uart.c,CMakeLists.txt}`.
- **Contract:** `protocol/PROTOCOL.md` §2.1 (COBS framing, raw passthrough) and
  §5 (autonomy, buffering, reconnection). The interface must already accept the
  TCP and ESP-NOW implementations that S6 adds — shape it now, stub those.
- **Behaviour:** UART0 at 115200, driver-installed, TX drained by a dedicated
  FreeRTOS task from a bounded queue; RX fed into `esps_enlp_stream_t`. A full
  TX queue **drops the oldest telemetry and sets `gap_before`** — it never
  blocks. Link state is derived from `HELLO_ACK` and inbound traffic, and
  losing it changes nothing about sampling.
- **Tests:** host tests for the queue policy (fill, overflow, drain order) by
  factoring the queue logic into a pure-C unit. On-target verification is M1's
  definition of done, not a unit test.
- **Verification:** `make -C firmware/test/host test`, then
  `pio run -d firmware -e esp32dev`.
- **Out of scope:** WiFi, ESP-NOW, OTA, the experiment runtime.

### M2 — `esps_core` health, log hook, time sync

- **Files:** `firmware/components/esps_core/{include/,src/}` —
  `esps_node_id.c`, `esps_health.c`, `esps_log_hook.c`, `esps_time.c`.
- **Contract:** `PROTOCOL.md` §3.1 (identity), §4.3 (`HEARTBEAT` layout), §4.5
  (`LOG` layout), §4.11 (`TIME_SYNC`).
- **Care required:** the log hook is **re-entrancy sensitive** — it must never
  log from inside itself and must drop rather than block when the queue is
  full. `esp_reset_reason()` latches `brownout_since_boot` and
  `watchdog_reset` into the heartbeat flags; these are the fields that will
  explain a field failure six weeks from now, so get them right.
- **Tests:** host tests for the heartbeat and log payload builders (pure
  functions over a buffer). Reset-reason mapping verified on target.
- **Out of scope:** GNSS, NTP, the store.

### M3 — gateway serial transport hardening

- **Files:** `gateway/espstation_gateway/transports/serial_port.py` and tests.
- **Behaviour:** hot-plug — a port that disappears mid-session must surface as
  a link-down event and the transport must re-open when it returns, without a
  gateway restart. `PermissionError` produces an actionable message naming the
  `dialout` group and pointing at `docs/SETUP.md` §5, not a traceback.
- **Tests:** a PTY-backed fake serial device exercising: reconnection after the
  device vanishes, partial reads across frame boundaries, garbage-then-frame,
  and a write to a closed port.
- **Verification:** `gateway/.venv/bin/python -m pytest tests/ -q`.

### M4 — `tools/enlp_sniff.py`

- **Files:** `tools/enlp_sniff.py`.
- **Behaviour:** open a port, decode ENLP frames using the gateway codec (never
  a second implementation — D-8), print them human-readably with timestamps,
  and print raw non-frame bytes distinctly. Flags: `--filter <type>`,
  `--raw-only`, `--hex`, `--json`.
- **Why it exists:** it is the tool you will want at 2 a.m. when the app shows
  nothing and you need to know whether the problem is the board, the cable or
  the gateway. Build it before you need it.

## Definition of done

- [ ] `pio run -d firmware -e esp32dev -t upload` flashes the connected board.
- [ ] `tools/enlp_sniff.py /dev/ttyUSB0` shows: the boot-ROM banner as **raw
      text**, then a decoded `HELLO` carrying the NDB, then heartbeats at 1 Hz
      with plausible heap and uptime.
- [ ] `node.ping` returns a `CMD_ACK` in under 100 ms, measured.
- [ ] Unplug the USB cable for 60 s and replug: the gateway reconnects with no
      restart, and the node's uptime shows it **never rebooted**.
- [ ] Telemetry sampled while unplugged is not silently lost — either it
      arrives with the `replay` flag or the `gap_before` flag is set. (Full
      store-and-forward is S4; S1 only has to be honest about the gap.)
- [ ] `dialout` fixed and `docs/SETUP.md` §5 confirmed accurate on this machine.
- [ ] Free heap stable over a 30-minute run — no leak in the link path.

## Risks and unknowns

- **The `dialout` group requires a logout.** This blocks every on-target check
  and only the user can do it. Resolve it first, not last.
- **Boot-ROM output is at a different baud on some boards** (74880 on some
  ESP8266-era designs; the ESP32 ROM prints at 115200 by default but this is
  configurable by strapping). If the banner is garbage, that is why — document
  the finding rather than suppressing the output.
- **CP2102 latency.** The default USB latency timer can add tens of
  milliseconds, which will show up in the `node.ping` measurement. Measure
  before concluding the firmware is slow.
- **Log-hook re-entrancy** is the most likely source of a hard-to-reproduce
  crash in this sprint. Review it specifically.

## Decisions expected

- TX queue depth and the drop policy under sustained overflow (oldest vs
  newest). Oldest-first preserves recency, which is what an operator watching a
  live chart wants; say so in the entry.
- Whether link state is derived purely from inbound traffic or also from an
  explicit station keepalive.
- How the gateway detects and handles the `uint32` millisecond wrap (D-10 says
  it must; S1 is where it first becomes reachable).

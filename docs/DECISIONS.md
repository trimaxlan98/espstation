# Decision Log

Autonomous and semi-autonomous decisions, with rationale and consequences.
Format: **D-n — decision** / why / consequence.

Append here whenever you make a choice a future contributor could reasonably
have made differently. A decision without a recorded reason gets re-litigated
every six months; a decision with one gets improved instead.

---

## D-1 — The node is autonomous; the station is a laboratory
The experiment lives on the node, in NVS, and runs with or without a station.
The station observes, configures and orchestrates — it is never in a control
loop.
**Why:** an ESP32 is deployed in a field, on a battery, inside a payload, with
nobody watching. PiStation's inverse principle (the Pi is a sensor, the laptop
is the brain) is right for a mains-powered general-purpose computer and wrong
for this.
**Consequence:** every feature must answer "what happens when the cable is
out?". Drives store-and-forward (D-9), declarative experiments (D-6) and the
reconnection-first protocol design.

## D-2 — ESP-IDF via PlatformIO, not Arduino, not bare idf.py
ESP-IDF gives FreeRTOS, ESP-NOW, OTA, NVS and partition control natively;
PlatformIO gives multi-target build environments and vendors its own
cmake/ninja/toolchain, so a contributor runs one `pip install platformio`
instead of a system-wide IDF setup.
**Why:** the roadmap ends in mesh networking, OTA and aerospace profiles.
Arduino-ESP32 has a ceiling well below that.
**Consequence:** a steeper on-ramp. Mitigated by D-4 (the most-touched code is
host-compilable) and by `docs/SETUP.md`.

## D-3 — Split encoding: JSON control plane, packed binary data plane
`cJSON` ships with ESP-IDF and control messages are low-rate and schema-fluid;
telemetry is high-rate and must fit ESP-NOW's 240-byte payload budget without
allocating.
**Why:** one encoding for both would either waste the radio budget or make the
parts that change weekly painful to change.
**Consequence:** two codecs to keep in sync across three implementations —
which is exactly what `protocol/espstation.protocol.yaml` and
`tools/check_protocol.py` exist to police.

## D-4 — `esps_proto` is pure C11 with zero ESP-IDF dependencies
The codec includes only `<stdint.h> <stddef.h> <string.h> <stdbool.h>`, takes
caller-provided buffers, and never allocates.
**Why:** it is the code most likely to have subtle bugs (framing, CRC, byte
offsets) and the code least able to be debugged on target. Making it
host-compilable means gcc + ASan + UBSan + a real test suite gate it on every
push, and a contributor with no ESP32 can still fix it.
**Consequence:** no `ESP_LOGx` inside the codec; errors are return codes.

## D-5 — COBS + CRC-16 on serial; the link shares UART0 with the boot ROM
COBS with a `0x00` delimiter resynchronises unambiguously mid-stream. ESP-IDF
logs are wrapped into `LOG` frames by an `esp_log_set_vprintf` hook; bytes that
fail to decode are surfaced as raw console output rather than dropped.
**Why:** a second UART is a wire the user has to add, and the boot-ROM banner
is genuinely useful — the first thing you want when a board misbehaves is its
boot log. Dropping undecodable bytes would throw exactly that away.
**Consequence:** the gateway's decoder has two output paths (frames and raw),
and both are tested.

## D-6 — Experiments are declarative JSON stored in NVS, not compiled code
**Why:** reconfiguring twenty nodes must be editing a document, not twenty
rebuild-and-flash cycles. This is the feature that makes the system worth
building rather than being a nicer serial monitor.
**Consequence:** a spec interpreter on the node (cost: flash and complexity)
and three validation gates (`docs/EXPERIMENTS.md`), including one at boot —
because the expensive failure is a node in an unreachable place holding a spec
it cannot run.

## D-7 — The NDB (Node Database): the node declares its own channels
The station hard-codes no channel ids, units or limits; charts, formatting and
validation are driven by the `ndb` array in `HELLO`.
**Why:** directly lifted from PiStation's Mission Database, and it is what
makes D-6 and S7 possible — adding a sensor driver must not require a desktop
or gateway change. That constraint is S7's definition of done.

## D-8 — Simulated nodes are the same gateway with a different transport
`transports/sim/` speaks byte-identical ENLP through the same codec as the
serial path.
**Why:** PiStation's D-1, and it earned its keep there. Contract drift becomes
impossible by construction, and a 20-node mesh is demoable on a laptop.
**Consequence:** the simulator is production code with production tests, not a
mock. It costs more and is worth it.

## D-9 — `TELEM_ACK` is a durability watermark, not a transport ack
The station acknowledges the highest sequence number it has **committed to
SQLite**; only then may the node free that storage.
**Why:** per-frame acks would put the link in the reliability path, which D-1
forbids. A watermark keeps reliability end-to-end and lets the node decide
locally when to overwrite.
**Consequence:** the node must tolerate never being acked (it overwrites
oldest-first), and the gateway must never ack optimistically.

## D-10 — Node time is monotonic ms; the host converts, once
The node reports `uint32` milliseconds since boot and its clock is never
rewritten. The gateway maps to float Unix epoch seconds using a per-node
`TIME_SYNC` offset, in exactly one place.
**Why:** rewriting a node's clock corrupts the ordering of already-buffered
samples — the store-and-forward case where correctness matters most. Float
epoch seconds at the host boundary keeps PiStation's convention.
**Consequence:** `uint32` ms wraps at ~49.7 days; the gateway must detect and
handle the wrap for long-running deployments.

## D-11 — Gateway port 8787
Adjacent to PiStation's 8737 so the two can run on one host without collision
and are memorable together.

## D-12 — Two OTA partitions from the very first flash
`partitions.csv` reserves `ota_0`/`ota_1` and a LittleFS `store` partition
before OTA exists (S5).
**Why:** repartitioning a deployed fleet is not an option. Getting the layout
wrong now means physically recovering every node later.

## D-13 — Multi-target build environments declared in S0, honestly marked untested
`platformio.ini` carries `esp32s3`, `esp32c3` and `esp32c6` environments while
only `esp32dev` hardware exists.
**Why:** shaping the capability HAL for multiple targets from the start is
cheap; retrofitting it after the code assumes one chip is not.
**Consequence:** those environments are **unverified** until the silicon
arrives, and the roadmap says so rather than implying support.

## D-14 — Message codes `0x80`–`0xFF` are reserved for experiments
The gateway passes them through opaquely to the UI.
**Why:** a protocol with no extension point gets extended anyway, badly, by
overloading an existing message. Giving experiments their own range keeps that
traffic out of the core spec.

## D-15 — Confirmation gates live on the station, not the node
The node executes any well-formed `CMD` it receives; the desktop requires an
explicit operator confirmation for every mutating action.
**Why:** the threat model is a bench and a LAN. Putting the gate on the node
would mean an approval protocol over a link that D-1 says may not exist.
**Consequence:** exposing the gateway beyond localhost is opt-in and
token-gated, and this decision must be revisited before any deployment where
the link is not trusted.

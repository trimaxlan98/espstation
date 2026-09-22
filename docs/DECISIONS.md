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

---

## Out-of-band slice: the digital link (D-16 … D-21)

Decisions from the two-board digital-link practice and its integration into
EspStation (see `bench/practicas/enlace-digital/` and the ROADMAP note on this
slice). The link contract itself lives in
`bench/practicas/enlace-digital/SPEC-LINK.md`.

## D-16 — The inter-node link is synchronous two-wire (explicit clock), not a bit-banged UART
Data on one line, clock on another; the receiver samples on the rising clock
edge.
**Why:** an asynchronous UART needs both boards to agree on a bit period to
within a few percent and to sample mid-bit, which a task-scheduled GPIO
bit-banger on a dual-core RTOS does poorly; with an explicit clock the data
only has to be stable around the edge, so scheduling jitter lengthens the
period instead of corrupting bits. It is also the easier link to explain in a
classroom. The cost is one extra wire and one extra pin pair.
**Consequence:** the speed limit is set by ISR latency and edge integrity, not
by clock agreement — and is reported as a measured finding, not hidden. It
prefigures the bus-with-addressing extension for S6.

## D-17 — Link signals are NDB channels and `EVENT`s; no new protocol messages
`dio.tx`, `dio.rx`, `link.rtt_us`, `link.frames_ok`, `link.frames_err`,
`link.ber` are declared in the node's `HELLO` (ids 16–21, the experiment range
of PROTOCOL.md §4.1); edges, CRC errors and link loss are `EVENT`s. The `0x80`
range (D-14) is not used.
**Why:** everything the station needs already fits the existing contract, and
the slice doubles as an early test of S7's central constraint — a new driver
must not require a station change. The desktop's Live section charts these
channels with **zero TypeScript changes**.
**Consequence:** the desktop's presentation limits (one Y axis, four charted
channels) apply. A logic-analyzer panel would be separate work, justified by
those limits and not by any inability of the NDB to carry the signal.

## D-18 — Pin assignment; GPIO12 is never used; the link owns its pins
`TX_DATA` 26, `RX_DATA` 25, `TX_CLK` 27, `RX_CLK` 14, LED 4; same on every
board so one firmware serves all and only a role constant changes. GPIO12 is the
MTDI strapping pin: high at reset selects 1.8 V VDD_SDIO and a 3.3 V flash may
not boot. GPIO6–11 are the flash bus, 34–39 have no pull-down, and 0/2/5/15 are
strapping pins to avoid. `set_gpio` may drive only the safe output list, and
refuses the link's own pins (`reason: owned_by_link`): 14 and 25, the link's
*inputs*, always — a node cannot know that the other end of that wire is not an
output — and 26 and 27, its outputs, unless the node is in manual mode.
**Why:** a stray `set_gpio` on a pin the link is driving would create bus
contention between two boards — the one electrical fault this design must not
allow.
**Consequence:** GPIO14 is reported in the classic ESP32 pin references as
emitting a short PWM burst at boot — from general knowledge, **not yet measured
on this bench**. Since `RX_CLK` is GPIO14 and the other board's `TX_CLK` drives
the same wire, series resistors on all four lines are part of the wiring rules,
and the effect is on the hardware-verification checklist.

## D-19 — One link contract, three implementations, guarded by golden vectors
The frame (`0xAA·LEN·PAYLOAD·CRC-8/SMBUS`), the test-payload generator and the
BER accounting exist in the Arduino sketches, in `esps_dio` (C11, pure) and in
the simulator (Python). This is a duplication D-8 would normally forbid; it is
acceptable here because what goes over ENLP is still the one real codec, and
the link layer is small, frozen and pinned by shared vectors: the golden CRC and
frame vectors plus a bit-flip sweep that must give exactly 13 spurious
`FRAME_OK` on 8000 `LEN` flips and 0 on 139 976 payload/CRC flips, asserted in
the C suite, the Python suite and — through `make bench-test`, which compiles
the real `n3_bytes.ino` against a host mock of the Arduino core and cross-checks
its counters against `dio_link.py` — the sketch itself. That last test proves
logic, never timing, and was added after the review found the sketch was the
only one of the three implementations with no test in the repo.
**Why:** the sketches must be self-contained (an academic deliverable), and the
simulator cannot link C.
**Consequence:** any change to the link contract must land in all three plus
their vectors in one commit. **Measured weakness:** CRC-8 does not protect `LEN`
from itself — a flipped `LEN` bit gives a spurious `FRAME_OK` about 0.16 % of
the time — so `frames_ok` with `bit_errors > 0` is a real, visible outcome.

## D-20 — An oversized NDB is announced as several partial `HELLO`s
Three `sys.*` channels plus the six link channels serialise to 1211 B on the
real firmware (≈1450 B with the simulator's five `sys.*`), over the 1024 B
`MAX_PAYLOAD` — and over the firmware's real ceiling of 885 B, which is what its
900 B UART frame buffer leaves after COBS and header. Even the simulator's
default node already uses 959 B. So the node sends several complete `HELLO`s,
each with a slice of `ndb`, and the station merges them.
**Why:** PROTOCOL.md §4.1 already lets a node extend its NDB by re-sending
`HELLO`; slicing reuses that rule and needs no new message or field. The
alternatives — shorter descriptors, or a new message — would either lose the
human-readable `name`/`unit` that D-7 depends on or be a real protocol change.
**This is a wire-format non-change but a station behaviour change**, and the
first version of this entry understated that. The gateway's store used to
overwrite a node's stored NDB with whatever the last `HELLO` carried; it now
merges. PROTOCOL.md §4.1 therefore states three rules as requirements on the
station (merge, never delete, one `HELLO_ACK` per `HELLO`) and one on the node
(re-announce until every slice of the round is acknowledged). The last two exist
because of a bug the review found: the node used to stop at the first ACK, so a
lost second slice was never resent and the station silently dropped that
slice's samples.
**Consequence:** `tools/check_protocol.py` cannot police any of this — it
compares frames and fields, not station behaviour — so the rules are pinned by
gateway tests (`test_store_ndb.py`, the HELLO-slice and unknown-channel tests).
The minor version in `espstation.protocol.yaml` is **not** bumped, because
nothing on the wire changed; revisit if §6 is read to require it. The station
never deletes a channel it has learned, so a node that reboots with a smaller
NDB leaves stale channels in the registry and in SQLite until that is designed
properly — there is no cleanup path today. One announcement of N slices is one
*session*: slices from the same node on the same link less than 5 s apart share
a `session`, while each slice still gets its own `HELLO_ACK` and its own
time-sync anchor.

## D-21 — Link firmware: role by build variant, receive path in IRAM, `set_gpio` declared but unreachable
`ESPS_DIO_ROLE` is a compile-time constant (0 = disabled, the default and
byte-for-byte the S0 behaviour of RAM; 1 = initiator/transmitter, 2 =
responder/receiver), selected by the PlatformIO envs `esp32dev_dio_a` and
`esp32dev_dio_b`. The receive path (`frame.o`, `crc8.o`) is placed in IRAM by a
linker fragment and the GPIO ISR service is installed with
`ESP_INTR_FLAG_IRAM`. The phase task runs at priority 11 pinned to core 1 — which puts it away from
the esp_timer task and ISR (both pinned to core 0 in this build) but does **not**
isolate it from the UART link tasks, which have no affinity and may land on
core 1, where priority 11 starves them for up to ~28 ms per frame (~370 ms per
burst); the tighter side is RX, whose 2048 B driver ring is ~4.2 kB short of
370 ms of continuous traffic at 115200 baud — harmless while the station only
sends occasional CMDs, not a guarantee. The task stack is 6144 B, sized from a
static call-graph measurement (~3760 B worst case) plus interrupt and ISR frames. Role A
masks its own `RX_DATA` interrupt during an N3 burst. `esps_dio_set_gpio()`
exists and its validation is host-tested, but nothing invokes it.
**Why:** there is no experiment runtime before S3, so the role cannot come from
an `EXP_SET`; a build variant is the honest stand-in (`TODO(S3)`). Without IRAM
placement, the ISR is masked whenever this node writes its own flash (NVS at
boot, `node.set_label`), which would inject errors into the very link being
measured, invisibly. Role B mirrors every `RX_DATA` edge back onto its
`TX_DATA`, which lands on A's `RX_DATA` — about one interrupt per data bit on
the task that is bit-banging with a 50 µs half period — so A must mask it. A
`set_gpio` CMD op would be a protocol change, so it is deliberately not added.
**Consequence:** none of this has run on hardware. Claims that depend on it (the
IRAM benefit, 10 kbit/s holding, ISR keep-up, the 6144 B stack budget) are
hypotheses with a named bench test each in the hardware-verification prompt.
Two boards must be flashed with different roles: two role-B boards look inert
and two role-A boards both report `link.lost`.

## D-22 — The Morse bench boards are adapted into ENLP, not reimplemented as nodes
`bench/practicas/morse-duplex/` runs an Arduino sketch that prints plain text
at 115200. `gateway/espstation_gateway/transports/morse_sketch.py` makes those
boards visible in the app by sitting in the **FrameDecoder** slot: text goes
in, real ENLP frames come out, built with `protocol.frames`/`protocol.messages`
and immediately re-parsed. The station learns the link through **NDB channels
22-29** (`morse.*`), which are inside the node-defined range 16-127, so no
protocol change is involved and `tools/check_protocol.py` is untouched. The
adapter declares only `sys.uptime` of the mandatory system channels. The link
is created with `kind: "morse"`; `kind: "morse-replay"` feeds the same adapter
from a recorded `captura_serie.py` capture instead of a port.
**Why:** the alternative — teaching the gateway to speak a second device
protocol — would put a second codec in the station and break D-8. As a decoder,
the Morse path reaches the registry, the store, REST, WS and the desktop as an
ordinary node with no special cases anywhere downstream. Announcing
`sys.heap_free` or `sys.rssi` was rejected: the adapter cannot know them, and a
channel that never produces a sample is a lie the station charts as a gap.
**Consequence:** these boards are **not** espstation-fw nodes and must never be
mistaken for them — `fw.build` is `arduino-sketch` and `caps` carries
`read_only`. Three things follow. (1) **Commands cannot reach them.** A `CMD`
or `EXP_SET` raises `TransportError` rather than being swallowed, because an
operator must not believe a command landed when the board cannot parse it;
supporting `p/l/w/d/k` from the app needs a `morse.*` op and therefore the
full four-place protocol change. (2) **The adapter does write two things**:
`r` (a pure read of thresholds and counters) on a 5 s poll, and at most one
`v` per attach if the board reports verbose off. Opening the port resets the
board, which clears verbose and zeroes the counters, so without this the eight
declared channels stay empty forever — this is a driver configuring its own
source, not the station commanding a node. (3) **Timestamps are a
reconstruction.** The sketch stamps only `# TX flanco` and `# resumen`, so the
adapter anchors on the last stamp and extrapolates with the station's clock
between anchors; they are not the board's own millisecond clock and must not
be read as such. The real-firmware path, where all three limitations
disappear, is `firmware/components/esps_morse/`: its pure C11 half (table,
decoder, key debounce) exists and is gated on the host; its ESP-IDF half is
not written (see D-23).

## D-23 — `esps_morse` ships its pure C11 half only, and the host gate is runnable without `make`
`firmware/components/esps_morse/` is the table, the pulse/silence state machine
and the key debounce: pure C11 over caller-owned state, no allocation, no
globals, no ESP-IDF, compiled verbatim by `test/host/` under `-Werror` with
ASan+UBSan. There is deliberately **no** `esps_morse.c` — no GPIO setup, no
edge ISR, no FreeRTOS task, none of what `esps_dio.c` is for the digital link.
The decoder reports events through a caller-supplied array (`esps_morse_edge`,
`esps_morse_tick`) rather than a callback, and `tick()` may write two of them
(letter then word) in that fixed order. Alongside the Makefile there is now
`firmware/test/host/run_tests.py`, which compiles and runs exactly the same
files with the same flags.
**Why:** an ESP-IDF layer cannot be verified without the toolchain and two
boards, and writing firmware glue that nobody has compiled is the placeholder
AGENTS.md rule 9 forbids. The pure half, in contrast, is the part the practice
actually rests on and is fully gated today — including two things no bench
session can reach: the ~71.6 min `micros()` wrap and the ~49.7 day `millis()`
wrap [D-10], both covered by vectors. A callback API was rejected because it
would put station-shaped decisions (what to publish, how to rate-limit) inside
a component that must stay decidable on a host. `run_tests.py` exists because
`make` is not present on a stock Windows install, and a contributor who cannot
run the gate will not run it; the Makefile stays the reference and CI keeps
using it, and the runner warns when the two file lists have drifted.
**Consequence:** the Morse boards on the bench still run the Arduino sketch and
reach the station through the gateway adapter (D-22), with the three
limitations named there. `esps_morse` is a third implementation of the same
link — sketch, C11, Python — so the golden vectors in SPEC-DUPLEX.md are now
load-bearing across three languages: change one, change all of them and the
SPEC in the same commit. The `tick()` contract needs `max >= 2`; with `max ==
1` a gap that closes both a letter and a word reports the letter and loses the
word, which is documented at the declaration and is not detected at runtime.

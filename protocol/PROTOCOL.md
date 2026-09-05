# ENLP — EspStation Node Link Protocol v0.1

> **This document is law.** Firmware, gateway and desktop all implement the same
> wire format. No component may add, remove or reshape a message without
> changing this file, `protocol/espstation.protocol.yaml`, and every
> implementation in the same commit. See [CONTRIBUTING.md](../CONTRIBUTING.md).

ENLP carries telemetry, logs, events, commands and experiment state between an
**ESP32 node** and the **station** (the host running the gateway). It is
transport-agnostic: the same frame body travels over USB/UART, TCP/WebSocket,
and ESP-NOW.

The protocol is designed around one invariant: **the node is autonomous.**
ENLP is an observation and configuration channel, never a control loop. A node
that loses the link keeps running its experiment, keeps buffering telemetry,
and re-synchronises on reconnect.

## 1. Layers

```
+-----------------------------------------------------------+
| Message payloads   JSON (control plane) | packed (data)    |
+-----------------------------------------------------------+
| Frame body         ver|type|node|seq|len|payload|crc16     |
+-----------------------------------------------------------+
| Framing            COBS+0x00 (serial) | u16-prefix (TCP/WS)|
|                                       | raw (ESP-NOW)      |
+-----------------------------------------------------------+
| Transport          UART/USB | TCP | WebSocket | ESP-NOW    |
+-----------------------------------------------------------+
```

### 1.1 Control plane vs data plane

| Plane | Encoding | Why |
|---|---|---|
| Control (HELLO, CMD, EXP_SET, EVENT, NET_*) | **JSON**, UTF-8, no NUL | Low rate, schema evolves fast, `cJSON` ships with ESP-IDF, trivially debuggable |
| Data (TELEMETRY, HEARTBEAT, BULK_CHUNK) | **Packed little-endian** | High rate, must fit ESP-NOW's 250 B and not allocate on the node |

Mixing the two is deliberate: the parts that change often are readable, the
parts that run hot are cheap.

## 2. Framing

### 2.1 Serial (UART / USB-CDC) — COBS

Every frame body is [COBS]-encoded and terminated with a single `0x00`
delimiter. COBS removes `0x00` from the encoded body, so the delimiter is
unambiguous and a receiver that joins mid-stream resynchronises at the next
`0x00`.

**Bytes between delimiters that fail to COBS-decode, or whose CRC is wrong, are
not an error.** The gateway surfaces them as *raw console output* — this is how
the ESP32 boot ROM banner and any `printf` before the link starts reach the UI
instead of being silently dropped.

ESP-IDF log output does **not** go out as raw text: firmware installs an
`esp_log_set_vprintf` hook that wraps every log line into a `LOG` frame (§4.5),
so logs arrive structured (level + tag + message) and interleave correctly with
telemetry. Boot-ROM output precedes the hook and therefore arrives raw.

### 2.2 TCP / WebSocket — length prefix

`uint16` big-endian body length, then the body. WebSocket uses **binary**
frames, one frame body per WS message (the length prefix is still present so
the same parser serves both).

### 2.3 ESP-NOW — raw

The frame body is placed directly in the ESP-NOW payload. ESP-NOW carries
250 B, the header+CRC cost 10 B, so **the payload budget is 240 B**. Messages
larger than that must use `BULK_*` fragmentation (§4.9).

## 3. Frame body

All multi-byte integers are **little-endian** (native ESP32 byte order; the
gateway converts explicitly, never by casting).

```
offset  size  field    type      notes
0       1     ver      u8        protocol major version, 0x01 for v0.1
1       1     type     u8        message type (§4)
2       2     node     u16       node short id; 0 = the station itself
4       2     seq      u16       per-sender counter, wraps at 65535
6       2     len      u16       payload length in bytes, 0..1024
8       len   payload  bytes     see §4
8+len   2     crc16    u16       CRC-16/CCITT-FALSE over bytes [0, 8+len)
```

- **Header is 8 bytes, CRC is 2** → 10 bytes of overhead per frame.
- **`MAX_PAYLOAD` = 1024** on serial/TCP, **240** on ESP-NOW.
- **CRC-16/CCITT-FALSE**: poly `0x1021`, init `0xFFFF`, no reflection, no final
  XOR. Chosen over CRC-32 because ESP-NOW already CRCs the radio frame and
  10 B of overhead matters at 240 B payloads; the CRC here guards the *serial*
  path, where corruption is real.
- **`seq`** is per-sender and monotonic. Gaps are how the station detects loss.
  It is not a retransmission window: reliability for telemetry comes from
  store-and-forward (§5), not from per-frame ACKs.

### 3.1 Node identity

`node` is a `u16` **short id**, derived on first boot from the factory MAC
(`crc16(mac[6])`, remapped away from 0) and stored in NVS. The full 6-byte MAC
and a human label live in the `HELLO` descriptor. Short ids may collide across a
large fleet; the station detects a collision (two different MACs claiming one
short id) and asks the operator to reassign. Reassignment is a `CMD`
(`node.set_id`), persisted to NVS.

## 4. Message types

| Code | Name | Dir | Plane | Purpose |
|---|---|---|---|---|
| `0x01` | `HELLO` | N→S | JSON | Node descriptor + channel table + capabilities |
| `0x02` | `HELLO_ACK` | S→N | JSON | Session accepted, host time, station policy |
| `0x03` | `HEARTBEAT` | N→S | packed | Liveness + resource vitals |
| `0x10` | `TELEMETRY` | N→S | packed | Batched channel samples |
| `0x11` | `TELEM_ACK` | S→N | packed | Durable-storage watermark (§5) |
| `0x20` | `LOG` | N→S | packed | One structured log line |
| `0x21` | `EVENT` | N→S | JSON | Structured event (state change, fault, trigger) |
| `0x30` | `CMD` | S→N | JSON | Request an action on the node |
| `0x31` | `CMD_ACK` | N→S | JSON | Result of a `CMD` |
| `0x40` | `EXP_SET` | S→N | JSON | Install/replace the experiment spec |
| `0x41` | `EXP_STATE` | N→S | JSON | Experiment run lifecycle |
| `0x50` | `BULK_BEGIN` | N→S | JSON | Start of a bulk stream |
| `0x51` | `BULK_CHUNK` | N→S | packed | Bulk payload fragment |
| `0x52` | `BULK_END` | N→S | JSON | End + integrity check |
| `0x60` | `NET_REPORT` | N→S | JSON | Peer table / link quality |
| `0x61` | `NET_CMD` | S→N | JSON | Network-experiment control |
| `0x70` | `TIME_SYNC` | both | packed | Clock offset estimation |

Codes `0x80`–`0xFF` are **reserved for experiment-defined messages** and are
passed through by the gateway untouched (delivered to the UI as opaque blobs).
This is the extension point: an experiment can invent traffic without amending
this document.

### 4.1 `HELLO` (0x01) — node → station

Sent on every link establishment and re-sent every 30 s until `HELLO_ACK`
arrives. Idempotent.

```json
{
  "mac": "24:6f:28:aa:bb:cc",
  "node_id": 4711,
  "label": "node-a",
  "chip": { "model": "esp32", "revision": 3, "cores": 2, "features": ["wifi","bt","ble"] },
  "fw": { "version": "0.1.0", "build": "2026-09-04T20:11:00Z", "idf": "5.3.1", "target": "esp32" },
  "caps": ["telemetry","experiment","espnow","store_forward","ota"],
  "boot": { "count": 12, "reason": "power_on", "uptime_ms": 1840 },
  "ndb": [
    { "id": 1, "key": "sys.heap_free", "name": "Heap free",  "unit": "B",  "type": "u32", "rate_hz": 1,  "group": "system" },
    { "id": 2, "key": "sys.rssi",      "name": "WiFi RSSI",  "unit": "dBm","type": "i8",  "rate_hz": 1,  "group": "system" },
    { "id": 16,"key": "adc.a0",        "name": "ADC ch0",    "unit": "V",  "type": "f32", "rate_hz": 50, "group": "analog",
      "min": 0.0, "max": 3.3 }
  ]
}
```

**The `ndb` (Node Database) is the channel contract.** The station never
hard-codes channel ids; every chart, unit and limit is driven by what the node
declares. A node may extend its NDB at runtime (a new sensor is attached) by
re-sending `HELLO`.

Channel id ranges: `1–15` system, `16–127` experiment/sensor, `128–255`
reserved for network diagnostics.

### 4.2 `HELLO_ACK` (0x02) — station → node

```json
{ "session": "0f3c…", "host_time": 1788573060.412, "accepted": true,
  "policy": { "telemetry_rate_limit_hz": 200, "log_level": "info" } }
```

`accepted: false` with a `reason` tells the node to keep running autonomously
but stop transmitting (used when the station is only listening to another node).

### 4.3 `HEARTBEAT` (0x03) — packed, 16 B

```
0   4  uptime_ms   u32
4   4  heap_free   u32
8   4  heap_min    u32   lowest free heap since boot
12  1  state       u8    0 boot, 1 idle, 2 running, 3 degraded, 4 safe
13  1  flags       u8    bit0 buffered-data-pending, bit1 link-was-lost,
                         bit2 brownout-since-boot, bit3 watchdog-reset
14  1  rssi        i8    dBm, 0 if no radio link
15  1  reserved    u8
```

Cadence: 1 Hz by default. **A missed heartbeat is not a failure** — see §5.

### 4.4 `TELEMETRY` (0x10) — packed

```
0   4  base_ts_ms  u32   node monotonic ms of the first sample
4   1  count       u8    1..64
5   1  flags       u8    bit0 replay (drained from storage), bit1 gap-before
6   ...      count × sample
```

Each sample:
```
0   1  ch          u8    NDB channel id
1   2  dt_ms       u16   offset from base_ts_ms
3   1  enc         u8    0 u8, 1 i8, 2 u16, 3 i16, 4 u32, 5 i32, 6 f32, 7 bool
4   n  value             1, 2 or 4 bytes per enc
```

`enc` is repeated per sample rather than taken from the NDB so that a node can
downgrade precision under pressure (send `i16` for a channel declared `f32`)
without renegotiating. The station converts using the NDB's declared type as
the semantic type and `enc` as the transport type.

### 4.5 `LOG` (0x20) — packed

```
0   4  ts_ms    u32
4   1  level    u8   0 none,1 error,2 warn,3 info,4 debug,5 verbose (ESP-IDF order)
5   1  tag_len  u8
6   n  tag      UTF-8, not NUL-terminated
6+n ...msg      UTF-8, remainder of the payload, not NUL-terminated
```

### 4.6 `EVENT` (0x21) — JSON

```json
{ "ts_ms": 91234, "code": "exp.trigger", "severity": "info",
  "data": { "run_id": "r-7", "channel": "adc.a0", "value": 2.91, "threshold": 2.9 } }
```

`severity` ∈ `debug | info | warning | error | critical`. `code` is a
dotted namespace; `sys.*`, `exp.*`, `net.*`, `fdir.*` are reserved.

### 4.7 `CMD` (0x30) / `CMD_ACK` (0x31) — JSON

```json
{ "id": 42, "op": "exp.start", "args": { "run_id": "r-7" } }
```
```json
{ "id": 42, "ok": true, "data": { "state": "running" } }
{ "id": 43, "ok": false, "err": { "code": "busy", "message": "run r-6 active" } }
```

Every `CMD` **must** be answered with a `CMD_ACK` carrying the same `id`,
within 2 s. The station retries an unacked `CMD` at most twice, then surfaces a
timeout. `CMD` handling on the node is single-threaded and non-blocking:
handlers that need to do work return `ok` with `"async": true` and report
completion as an `EVENT`.

Core ops (v0.1): `node.ping`, `node.info`, `node.reboot`, `node.set_id`,
`node.set_label`, `node.set_log_level`, `exp.start`, `exp.stop`, `exp.state`,
`store.drain`, `store.erase`, `net.scan`.

**Every mutating op requires operator confirmation in the desktop UI.** That is
enforced on the station side, not on the node — the node trusts the link.

### 4.8 `EXP_SET` (0x40) / `EXP_STATE` (0x41)

`EXP_SET` carries an **experiment spec** (see `docs/EXPERIMENTS.md`), which the
node validates, persists to NVS and — this is the point of the whole system —
runs on its own from then on, across reboots, with or without a station.

```json
{ "run_id": "r-7", "state": "running", "spec_hash": "9f2c…",
  "started_at_ms": 84020, "elapsed_ms": 7214, "progress": 0.12,
  "samples": 3607, "buffered": 0, "reason": null }
```

`state` ∈ `idle | armed | running | paused | done | aborted`.

### 4.9 `BULK_*` (0x50–0x52)

Used to drain stored telemetry, dump a file, or move any payload larger than
one frame. `BULK_BEGIN` names a `stream` and a `total` byte count;
`BULK_CHUNK` payloads are `u16 index` + bytes; `BULK_END` carries a CRC-32 of
the reassembled stream. A missing chunk index is requested again with
`CMD {op:"bulk.resend", args:{stream, index}}`.

### 4.10 `NET_REPORT` (0x60) / `NET_CMD` (0x61)

```json
{ "ts_ms": 120344, "role": "peer", "channel": 6,
  "peers": [ { "mac": "24:6f:28:11:22:33", "node_id": 8102, "rssi": -61,
               "tx": 1204, "rx": 1198, "lost": 6, "rtt_ms": 4.2, "last_seen_ms": 120100 } ] }
```

This is what feeds the topology graph and the loss/latency matrix in the
Networks section.

### 4.11 `TIME_SYNC` (0x70) — packed, 24 B

A three-timestamp exchange (station `t1`, node `t2`, node `t3`, station `t4`
measured on receipt) yielding offset and round-trip delay, NTP-style.

```
0   8  t1_host_us  u64   station send time, µs since Unix epoch
8   4  t2_node_ms  u32   node receive time, monotonic ms
12  4  t3_node_ms  u32   node send time, monotonic ms
16  8  reserved    u64
```

The station keeps `offset = t1 + rtt/2 − t2` per node and uses it to map node
monotonic ms → Unix epoch seconds. **Node time is never rewritten**: a node's
monotonic clock is the ground truth for intra-run ordering, and correcting it
would corrupt already-buffered samples.

## 5. Autonomy, buffering and reconnection

This section is the reason the protocol exists in this shape.

1. **The node never blocks on the link.** All transmit paths are non-blocking
   with a bounded queue; when the queue is full, telemetry is written to the
   store (§5.2) and the `gap-before` flag is set on the next live frame.
2. **Link loss is a normal state, not an error.** On loss the node continues
   the experiment, keeps sampling, and marks `flags bit1` in the next
   `HEARTBEAT` it manages to send.
3. **Store-and-forward.** Samples that could not be sent go to a ring buffer in
   RAM and, if the experiment declares `persist: true`, to NVS/LittleFS. On
   reconnect the node sends `HELLO`, then drains the store as `TELEMETRY`
   frames with `flags bit0 (replay)` set, interleaved with live data at a
   configurable ratio so the live view never stalls.
4. **`TELEM_ACK` is the durability watermark.** The station acknowledges the
   highest `seq` it has committed to its own database; the node may only then
   free that storage. Without an ack the node keeps the data until it must
   overwrite the oldest.
5. **Production mode.** A node whose experiment sets `standalone: true` runs
   with no expectation of a station at all: it stops sending `HELLO`
   retries after the configured window, drops to a low duty cycle, and only
   wakes the link when the experiment ends, storage crosses a threshold, or an
   operator physically resets it.

## 6. Versioning

`ver` in the header is the **major** version and must match exactly; a node and
a station that disagree exchange `HELLO`/`HELLO_ACK` and then stop, surfacing a
clear incompatibility to the operator. Additive changes (a new message type, a
new optional JSON field) do **not** bump `ver`; they bump the minor version
carried in `HELLO.fw` and in `espstation.protocol.yaml`.

**Rules for changing this protocol** — all three, in one commit:
1. `protocol/PROTOCOL.md` (this file) and `protocol/espstation.protocol.yaml`
2. `firmware/components/esps_proto/`
3. `gateway/espstation_gateway/protocol/` and the desktop's generated types

`tools/check_protocol.py` fails CI when the YAML and the implementations drift.

[COBS]: https://en.wikipedia.org/wiki/Consistent_Overhead_Byte_Stuffing

# Gateway API v0.1

The gateway listens on `http://127.0.0.1:8787` by default. Its generated
OpenAPI reference is available at `/docs` while it is running. This document
records the cross-component contract; ENLP itself is specified in
[`../../protocol/PROTOCOL.md`](../../protocol/PROTOCOL.md).

## Authentication

Every REST request requires `Authorization: Bearer <token>`. The development
token is `espstation-dev`; change it before exposing the gateway beyond
localhost. WebSocket clients pass the same token as the `token` query
parameter because browser WebSockets cannot set an authorization header.

```bash
curl -H 'Authorization: Bearer espstation-dev' \
  http://127.0.0.1:8787/api/nodes
```

Missing or invalid REST credentials return `401`. Invalid request bodies use
FastAPI's `422` response. Resource-specific `400`, `404`, `504` responses use
`{"detail": ...}`.

## REST endpoints

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/ping` | Gateway version and uptime |
| GET | `/api/ports` | Serial ports and whether each is in use |
| GET | `/api/links` | Active serial, TCP, and simulated links |
| POST | `/api/links` | Attach a link (`kind`, plus serial `path`/`baud` or TCP `host`/`port`) |
| DELETE | `/api/links/{link_id}` | Detach a link; the node remains autonomous |
| GET | `/api/nodes` | Fleet summaries |
| GET | `/api/nodes/{node_id}` | Summary plus NDB, capabilities, and boot descriptor |
| POST | `/api/nodes/{node_id}/command` | Send `{ "op": ..., "args": {...} }` and wait for `CMD_ACK` |
| GET | `/api/nodes/{node_id}/telemetry` | History grouped by NDB channel key |
| POST | `/api/nodes/{node_id}/experiment` | Validate and send an experiment spec |
| GET | `/api/experiments` | Stored experiment library |
| PUT | `/api/experiments/{exp_id}` | Store one spec; path and body IDs must match |
| POST | `/api/experiments/validate` | Structural and optional node/NDB validation |
| GET | `/api/runs` | Runs, optionally filtered by `node_id` |
| GET | `/api/runs/{run_id}` | One run |
| GET | `/api/runs/{run_id}/samples` | Samples belonging to a run |
| GET | `/api/network/topology` | Current simulated-network topology |
| GET | `/api/sim/scenarios` | Simulator scenario catalog |
| POST | `/api/sim/spawn` | Spawn `count` nodes with an optional `label_prefix` |
| POST | `/api/sim/fault` | Inject a supported simulator fault |

`GET /api/nodes` returns the exact `NodeSummary` consumed by the desktop:

```json
[{"node_id":1001,"label":"sim-1001","mac":"24:6f:28:00:03:e9",
  "state":"idle","online":true,"last_seen":1788579927.68,
  "uptime_ms":1024,"heap_free":180000,"rssi":-55,
  "fw":"0.1.0-sim","target":"esp32","link_id":"sim-1"}]
```

Telemetry accepts `since`, `until`, `channels` (comma-separated NDB keys), and
`max_points`. Timestamps are Unix epoch seconds and values are already
converted using the NDB.

```json
{"channels":{"adc.a0":[[1788579927.71,1.652],[1788579927.73,1.659]]}}
```

## WebSocket stream

Connect to `ws://127.0.0.1:8787/ws/stream?token=espstation-dev`. Every message
uses one envelope:

```json
{"kind":"telemetry","node_id":1001,"ts":1788579927.71,
 "data":{"channel":"adc.a0","value":1.652,"replay":false}}
```

`kind` is one of `heartbeat`, `telemetry`, `log`, `event`, `node`, `link`, or
`raw`. `node_id` is `null` for station/link-wide events. `ts` is always Unix
epoch seconds. Important payloads are:

- `telemetry`: `{channel, value, replay}` — one message per sample.
- `log`: `{level, tag, message}`, where level is a display name.
- `event`: `{code, severity, data}`.
- `node`: a complete REST `NodeSummary`, emitted on HELLO and heartbeat.

The stream is intentionally lossy UI delivery. Durable telemetry remains in
SQLite and can be recovered through the history endpoint.

## Simulator: the virtual cable (`--sim-dio`)

`python -m espstation_gateway --sim` serves the same three plain nodes as
always. `--sim-dio` (already on in `make gateway-run`) additionally attaches
two simulated nodes joined by a virtual two-wire cable, one wire per
direction, with a little loss, the odd glitch and a small non-zero BER so the
charts have something to show. It works with or without `--sim`.

These nodes publish the channels of
[`SPEC-LINK.md`](../../bench/practicas/enlace-digital/SPEC-LINK.md) as plain
NDB channels, exactly as the firmware will: `dio.tx` (16), `dio.rx` (17),
`link.rtt_us` (18), `link.frames_ok` (19), `link.frames_err` (20),
`link.ber` (21). No new message types and no desktop changes. Because ids
16-21 belong to the link, **a dio node has no `adc.a0`**. `link.rtt_us` is
only published when a handshake completes (a timeout is the absence of a
sample, plus a `link.lost` event).

Events use the exact `code`, severity, `data` keys and rate limits of the
"Semántica de eventos" table in SPEC-LINK.md, the same as the firmware:

| code | severity | `data` | limit |
|---|---|---|---|
| `dio.edge` | debug | `level`, `count` (running total) | 5 per 1000 ms |
| `link.up` | info | `rtt_us`, `timeouts` | none |
| `link.lost` (handshake) | warning | `timeouts`, `samples`, `reason:"handshake_timeout"` | none |
| `link.lost` (frames) | warning | `missed`, `seq`, `reason:"frames_missed"` | 1 per 5000 ms |
| `link.frame_ok` | info | `frames_ok`, `len` | 1 per 5000 ms |
| `link.crc_err` | warning | `frames_err`, `len`, `reason:"crc"` or `"len"` | 2 per 5000 ms |
| `dio.gpio` | info | `gpio`, `level` | none |
| `dio.gpio_rejected` | warning | `gpio`, `level`, `reason` | none |

A limit "N per W ms" is a fixed window admitting N events; the rest are
counted and reported as `data.suppressed` on the next event that gets through
(the key is absent when nothing was swallowed, and only limited events carry
it).

`CMD` `node.info` additionally returns `gpio_out`, the pins driven by
`set_gpio` (`{"4": 1}`). `set_gpio` only drives the pins SPEC-LINK allows;
anything else emits a `dio.gpio_rejected` warning and does nothing, with
`reason` `"pin_not_allowed"`, `"owned_by_link"` or `"bad_level"` (checked in
that order, as the firmware does). The link's input pins (14, 25) are always
`owned_by_link`; its output pins (26, 27) are too unless the node runs in
manual mode (`DioConfig(manual=True)`: no blink, handshakes or test frames,
`dio.tx` driven only by `set_gpio`). Nodes without dio refuse all four.
`--sim-dio` nodes are not manual, so demo `set_gpio` with
an experiment trigger on a free pin such as 4 (`{"action":"set_gpio","gpio":4,
"level":1}`); the result shows up as a `dio.gpio` event and in `gpio_out`.
Firmware does not run `set_gpio` yet (TODO(S3)).

**Chunked HELLO and sessions.** A dio node's NDB does not fit one frame, so it
announces itself with several complete `HELLO`s, each carrying a slice of the
channels; the gateway merges them (memory and SQLite alike) and answers **every
chunk with its own `HELLO_ACK`** (the node counts them). HELLOs from the same
node over the same link less than 5 s apart (`HELLO_ANNOUNCE_GAP_S`, measured
from the previous chunk) belong to one announcement and share one `session`,
fixed by the first chunk. A HELLO after 5 s or more, or over another link,
opens a new session (reconnect, reboot or the 30 s retry). Each chunk still
records its own time-sync anchor: every one is a valid clock measurement.

Simulator notes: dio must be enabled before a node starts (`spawn(dio=True)`,
`--sim-dio`); `SimNode.enable_dio()` on a node that already announced itself
raises `RuntimeError`, since the station cannot forget the `adc.a0` it was
told about. If a node's outbox is ever bounded and full, the frame is dropped
rather than blocking, and `node.info` reports the running count as
`outbox_dropped`.

`GET /api/network/topology` gains an additive `wires` array (`tx`, `rx`,
`delay_us`, `loss`, `glitch_rate`, `bit_error_rate`, `jitter_us`, plus
counters). A wire is reconfigured live through the fault endpoint:

```bash
curl -X POST -H 'Authorization: Bearer espstation-dev' -H 'Content-Type: application/json' \
  -d '{"kind":"wire","tx":1004,"rx":1005,"loss":1.0}' \
  http://127.0.0.1:8787/api/sim/fault      # cut the cable (loss back to 0 plugs it in)
```

Wire fields: `delay_us`, `loss` (0..1), `glitch_rate` (spurious pulses per
second), `bit_error_rate` (0..1, applied to test frames), `jitter_us`,
`glitch_width_us`. Every value must be a finite number in range and `tx`/`rx`
must be integer node ids; anything else (null, lists, strings, booleans,
NaN, `1e400`, unknown fields) is a 400 and the wire keeps its previous
configuration.

`GET /api/nodes/{id}` also reports `unknown_channel_samples`
(`{channel_id: count}`): telemetry samples dropped because the channel is not
in that node's NDB, e.g. after a lost HELLO chunk. The gateway logs a warning
the first time it happens for each (node, channel).

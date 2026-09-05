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

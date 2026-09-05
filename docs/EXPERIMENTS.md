# Experiments — the declarative spec

An **experiment** is data, not code. It is a JSON document that the station
pushes to a node (`EXP_SET`, 0x40), the node validates and persists to NVS, and
the node then executes on its own — across reboots, with or without a station.

This is the feature that makes the whole system worth building: **reconfiguring
a fleet of twenty nodes is editing a document, not twenty rebuild-and-flash
cycles.** Code changes are still needed for a *new kind* of measurement (a new
driver); they are not needed to change what is measured, how often, for how
long, or what happens when a threshold is crossed.

Canonical JSON Schema: [`protocol/experiment.schema.json`](../protocol/experiment.schema.json).

## Anatomy

```json
{
  "schema": 1,
  "id": "thermal-soak-v3",
  "name": "Thermal soak, 6 h",
  "standalone": true,
  "persist": true,
  "duration_ms": 21600000,
  "start": { "mode": "on_boot" },
  "channels": [
    { "key": "sys.heap_free", "rate_hz": 1 },
    { "key": "sys.temp",      "rate_hz": 2 },
    { "key": "adc.a0",        "rate_hz": 50, "enc": "i16", "scale": 0.0001 }
  ],
  "triggers": [
    { "when": { "channel": "sys.temp", "op": ">", "value": 70.0, "for_ms": 5000 },
      "emit": "exp.overtemp",
      "do":   [ { "action": "set_state", "state": "degraded" },
                { "action": "set_rate", "channel": "adc.a0", "rate_hz": 5 } ] }
  ],
  "network": null,
  "meta": { "operator": "alan", "notes": "chamber run 3" }
}
```

### Fields

| Field | Meaning |
|---|---|
| `schema` | Spec version. The node rejects a spec it does not understand rather than guessing. |
| `id` | Stable identifier; changing the body without changing the `id` is allowed (the hash distinguishes them). |
| `standalone` | `true` = production mode: after the `hello_window_ms` the node stops seeking a station and drops the link to a low duty cycle. This is the "unplug it and walk away" switch. |
| `persist` | Write telemetry to LittleFS as well as the RAM ring, so a power cycle does not lose the run. |
| `duration_ms` | `0` = run until stopped. |
| `start.mode` | `manual` (wait for `exp.start`), `on_boot` (arm and run at every boot), `at_ms` (node-monotonic deadline), `on_trigger`. |
| `channels[]` | Which NDB channels to sample and how. `enc` and `scale` let a node send `i16` for an `f32` channel to save bandwidth; the station reverses `scale` on ingest. |
| `triggers[]` | Condition → event + actions, evaluated on the node. `for_ms` is debounce: the condition must hold that long. |
| `network` | Optional network-experiment block (see below). `null` for a single-node run. |
| `meta` | Free-form; the station stores it with the run record. |

### Trigger actions (v1)

`set_state`, `set_rate`, `set_gpio`, `stop`, `reboot`, `mark` (annotate the
run), `burst` (temporarily raise a channel's rate for N ms — the classic
"capture the interesting bit at full resolution" pattern).

### Network experiments

```json
"network": {
  "mode": "espnow",
  "role": "peer",
  "channel": 6,
  "peers": "auto",
  "test": { "kind": "loss_latency", "rate_hz": 20, "payload_bytes": 64, "duration_ms": 300000 }
}
```

`mode` ∈ `espnow | wifi_sta | wifi_ap | mesh` (later: `ble`, `lora`,
`802154`). `test.kind` ∈ `loss_latency | throughput | range_sweep |
flood | custom`. Results surface as `NET_REPORT` (0x60) and drive the topology
graph and the loss/latency matrix in the desktop's Networks section.

## Lifecycle

```
        EXP_SET                exp.start / on_boot            duration or exp.stop
 idle ──────────► armed ─────────────────────────► running ──────────────────────► done
   ▲                │                                 │  │                            │
   │                └── validation failure ────────────┘  └── fault ──► aborted        │
   └──────────────────────── store.erase ──────────────────────────────────────────────┘
```

Every transition emits `EXP_STATE` (0x41) and an `EVENT` (0x21). A run gets a
`run_id` assigned by the station (or by the node when `standalone` and
unattended, as `n<node_id>-<boot_count>-<seq>`), and every sample the station
commits is tagged with it.

## Validation

Three gates, in this order:

1. **Station-side, before sending** — JSON Schema plus semantic checks the node
   cannot do cheaply: does every `channels[].key` exist in that node's NDB? Is
   the aggregate sample rate within the node's declared budget?
2. **Node-side, on receipt** — size, schema version, channel existence,
   rate limits, trigger arity. A rejected spec is answered with `CMD_ACK
   ok:false` and the *previous* spec stays installed. A node is never left
   without a valid experiment because a push failed.
3. **Node-side, on boot** — the persisted spec is re-validated against the
   current NDB. A driver that failed to initialise means its channels are gone;
   the node drops to `degraded` and runs the rest of the experiment rather than
   refusing to start.

## Why validation is this paranoid

Because the failure it prevents is the expensive one: a node deployed in a
place you cannot reach, holding a spec it cannot run, with no station to tell.
Rule 3 in particular — *run what you can, report what you can't* — is the
difference between a partial dataset and a wasted deployment.

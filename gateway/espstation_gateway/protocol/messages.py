# Typed encode/decode for every ENLP message (PROTOCOL.md section 4).
#
# JSON control-plane messages are pydantic models: cheap validation, and the
# desktop's TS types can be generated from the same shapes later without a
# second source of truth. Packed data-plane messages are plain dataclasses
# with hand-written struct.pack/unpack -- pydantic doesn't buy anything for
# fixed-layout little-endian binary, and struct format strings are the
# clearest way to keep the byte offsets honest against PROTOCOL.md.
#
# Message-type codes and the `enc` table are pulled from spec.py (i.e. from
# espstation.protocol.yaml itself), never hand-copied, so this file cannot
# silently drift from the wire contract the way a parallel constants list
# could.
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Any, ClassVar, Literal

from pydantic import BaseModel, ConfigDict, Field, model_validator

from . import spec

# ---------------------------------------------------------------------------
# Message type codes, sourced from the YAML.
# ---------------------------------------------------------------------------
_TYPES = spec.message_types()
TYPE_HELLO = _TYPES["HELLO"]
TYPE_HELLO_ACK = _TYPES["HELLO_ACK"]
TYPE_HEARTBEAT = _TYPES["HEARTBEAT"]
TYPE_TELEMETRY = _TYPES["TELEMETRY"]
TYPE_TELEM_ACK = _TYPES["TELEM_ACK"]
TYPE_LOG = _TYPES["LOG"]
TYPE_EVENT = _TYPES["EVENT"]
TYPE_CMD = _TYPES["CMD"]
TYPE_CMD_ACK = _TYPES["CMD_ACK"]
TYPE_EXP_SET = _TYPES["EXP_SET"]
TYPE_EXP_STATE = _TYPES["EXP_STATE"]
TYPE_BULK_BEGIN = _TYPES["BULK_BEGIN"]
TYPE_BULK_CHUNK = _TYPES["BULK_CHUNK"]
TYPE_BULK_END = _TYPES["BULK_END"]
TYPE_NET_REPORT = _TYPES["NET_REPORT"]
TYPE_NET_CMD = _TYPES["NET_CMD"]
TYPE_TIME_SYNC = _TYPES["TIME_SYNC"]

# enc code -> struct format char. The YAML's `encodings` table gives us the
# name+size; the struct char is a Python-side implementation detail (not
# part of the wire contract) so it's the one thing hand-mapped here.
_ENC_FMT: dict[str, str] = {
    "u8": "B", "i8": "b", "u16": "H", "i16": "h",
    "u32": "I", "i32": "i", "f32": "f", "bool": "B",
}
_ENC_BY_CODE = spec.encodings()
_ENC_CODE_BY_NAME = spec.encoding_by_name()


class MessageDecodeError(ValueError):
    """A message payload didn't match the shape its type code implies."""


def pack_sample_value(enc_code: int, value: Any) -> bytes:
    info = _ENC_BY_CODE.get(enc_code)
    if info is None:
        raise MessageDecodeError(f"unknown enc code {enc_code!r}")
    fmt = _ENC_FMT[info["name"]]
    if info["name"] == "bool":
        return struct.pack("<" + fmt, 1 if value else 0)
    return struct.pack("<" + fmt, value)


def unpack_sample_value(enc_code: int, data: bytes) -> Any:
    info = _ENC_BY_CODE.get(enc_code)
    if info is None:
        raise MessageDecodeError(f"unknown enc code {enc_code!r}")
    fmt = _ENC_FMT[info["name"]]
    size = info["size"]
    if len(data) < size:
        raise MessageDecodeError(f"need {size} bytes for enc={info['name']}, got {len(data)}")
    (raw,) = struct.unpack_from("<" + fmt, data, 0)
    if info["name"] == "bool":
        return bool(raw)
    return raw


def sample_value_size(enc_code: int) -> int:
    info = _ENC_BY_CODE.get(enc_code)
    if info is None:
        raise MessageDecodeError(f"unknown enc code {enc_code!r}")
    return info["size"]


# ===========================================================================
# JSON control-plane messages
# ===========================================================================

class _JsonMessage(BaseModel):
    """Shared (de)serialisation for JSON-plane payloads. `extra="allow"`
    everywhere: PROTOCOL.md section 6 says additive JSON fields don't bump
    the wire version, so an older gateway must tolerate fields it doesn't
    know about rather than rejecting the whole message."""

    model_config = ConfigDict(extra="allow", populate_by_name=True)

    def to_payload(self) -> bytes:
        # exclude_defaults matters more than it looks: HELLO's `ndb` list is
        # what most often pushes a control-plane message toward
        # MAX_PAYLOAD (1024 B, PROTOCOL.md section 3), and most NdbChannel
        # fields (rate_hz group, optional=False, ...) sit at their default
        # on most channels -- there's no reason to spend wire bytes on them.
        return self.model_dump_json(by_alias=True, exclude_none=True, exclude_defaults=True).encode("utf-8")

    @classmethod
    def from_payload(cls, payload: bytes):
        try:
            text = payload.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise MessageDecodeError(f"{cls.__name__}: payload is not valid UTF-8") from exc
        try:
            return cls.model_validate_json(text)
        except Exception as exc:  # pydantic.ValidationError, json errors
            raise MessageDecodeError(f"{cls.__name__}: {exc}") from exc


class ChipInfo(_JsonMessage):
    model: str
    revision: int
    cores: int
    features: list[str] = Field(default_factory=list)


class FwInfo(_JsonMessage):
    version: str
    build: str
    idf: str
    target: str


class BootInfo(_JsonMessage):
    count: int
    reason: str
    uptime_ms: int


class NdbChannel(_JsonMessage):
    """One row of the Node Database -- the channel contract (PROTOCOL.md
    section 4.1). `type` is the semantic type; a TELEMETRY sample's `enc`
    byte is the transport type and may differ (precision downgrade)."""

    id: int
    key: str
    name: str
    unit: str
    type: str
    rate_hz: float
    group: str
    min: float | None = None
    max: float | None = None
    optional: bool = False


class Hello(_JsonMessage):
    mac: str
    node_id: int
    label: str
    chip: ChipInfo
    fw: FwInfo
    caps: list[str] = Field(default_factory=list)
    boot: BootInfo
    ndb: list[NdbChannel] = Field(default_factory=list)


class HelloAckPolicy(_JsonMessage):
    telemetry_rate_limit_hz: float
    log_level: str = "info"


class HelloAck(_JsonMessage):
    session: str
    host_time: float
    accepted: bool
    reason: str | None = None
    policy: HelloAckPolicy | None = None


class Event(_JsonMessage):
    ts_ms: int
    code: str
    severity: str = "info"
    data: dict[str, Any] = Field(default_factory=dict)


class Cmd(_JsonMessage):
    id: int
    op: str
    args: dict[str, Any] = Field(default_factory=dict)


class CmdAckError(_JsonMessage):
    code: str
    message: str


class CmdAck(_JsonMessage):
    id: int
    ok: bool
    data: dict[str, Any] | None = None
    err: CmdAckError | None = None
    # "async" is a Python keyword; the wire field is exactly `async` per
    # PROTOCOL.md section 4.7 ("return ok with 'async': true").
    is_async: bool = Field(default=False, alias="async")


# The experiment-spec models below are deliberately strict (extra="forbid",
# matching the schema's additionalProperties:false) rather than the
# forward-compatible extra="allow" used everywhere else: EXPERIMENTS.md's
# whole validation philosophy is "the node rejects a spec it does not
# understand rather than guessing" (schema field literally versions this),
# so the station-side model should reject the same way, not silently accept
# fields a real node would refuse.
class _StrictJsonMessage(_JsonMessage):
    model_config = ConfigDict(extra="forbid", populate_by_name=True)


class ExperimentChannel(_StrictJsonMessage):
    key: str
    rate_hz: float = Field(gt=0, le=1000)
    enc: Literal["u8", "i8", "u16", "i16", "u32", "i32", "f32", "bool"] | None = None
    scale: float | None = None
    offset: float | None = None
    deadband: float | None = Field(default=None, ge=0)


class ExperimentTriggerWhen(_StrictJsonMessage):
    channel: str
    op: Literal[">", ">=", "<", "<=", "==", "!=", "delta"]
    value: float
    for_ms: int = Field(default=0, ge=0)


class ExperimentTriggerAction(_JsonMessage):
    """Kept permissive (not _StrictJsonMessage): the schema allows any
    combination of action-specific fields depending on `action`, which is a
    conditional shape JSON Schema expresses more precisely than a single
    pydantic model can without a discriminated union per action. Fields not
    relevant to a given action are simply ignored by the sim runtime."""

    action: Literal["set_state", "set_rate", "set_gpio", "stop", "reboot", "mark", "burst"]
    state: Literal["idle", "running", "degraded", "safe"] | None = None
    channel: str | None = None
    rate_hz: float | None = Field(default=None, gt=0)
    gpio: int | None = Field(default=None, ge=0, le=48)
    level: Literal[0, 1] | None = None
    ms: int | None = Field(default=None, ge=0)
    label: str | None = None


class ExperimentTrigger(_StrictJsonMessage):
    id: str | None = None
    when: ExperimentTriggerWhen
    emit: str | None = None
    once: bool = False
    do: list[ExperimentTriggerAction] = Field(default_factory=list, max_length=8)


class ExperimentStart(_StrictJsonMessage):
    mode: Literal["manual", "on_boot", "at_ms", "on_trigger"]
    at_ms: int | None = Field(default=None, ge=0)
    trigger: str | None = None

    @model_validator(mode="after")
    def _mode_requires_field(self) -> "ExperimentStart":
        # experiment.schema.json's allOf: mode=at_ms needs at_ms, mode=on_trigger needs trigger.
        if self.mode == "at_ms" and self.at_ms is None:
            raise ValueError("start.mode='at_ms' requires 'at_ms'")
        if self.mode == "on_trigger" and self.trigger is None:
            raise ValueError("start.mode='on_trigger' requires 'trigger'")
        return self


class ExperimentNetworkTest(_StrictJsonMessage):
    kind: Literal["loss_latency", "throughput", "range_sweep", "flood", "custom"]
    rate_hz: float | None = Field(default=None, gt=0)
    payload_bytes: int | None = Field(default=None, ge=1, le=240)
    duration_ms: int | None = Field(default=None, ge=0)


class ExperimentNetwork(_StrictJsonMessage):
    mode: Literal["espnow", "wifi_sta", "wifi_ap", "mesh"]
    role: Literal["peer", "root", "leaf", "sniffer"] = "peer"
    channel: int | None = Field(default=None, ge=1, le=14)
    ssid: str | None = None
    peers: Any = "auto"
    test: ExperimentNetworkTest | None = None


class ExperimentSpec(_StrictJsonMessage):
    """EXP_SET (0x40) payload -- the declarative experiment spec. Mirrors
    protocol/experiment.schema.json field-for-field (that file is the
    canonical JSON Schema named in docs/EXPERIMENTS.md); this model *is* the
    gateway's half of "Station-side, before sending" validation (gate 1 in
    EXPERIMENTS.md) -- pydantic's required/enum/range checks stand in for a
    JSON Schema validator so the dependency list doesn't need to grow.
    NDB channel-existence and aggregate-rate checks (the semantic half of
    gate 1) are protocol.ndb.NodeDatabase.validate_spec, since only the NDB
    has that information."""

    schema_: Literal[1] = Field(alias="schema")
    id: str = Field(min_length=1, max_length=64, pattern=r"^[A-Za-z0-9._-]+$")
    name: str = Field(default="", max_length=128)
    standalone: bool = False
    persist: bool = False
    duration_ms: int = Field(default=0, ge=0)
    hello_window_ms: int = Field(default=120_000, ge=0)
    start: ExperimentStart
    channels: list[ExperimentChannel] = Field(min_length=1, max_length=64)
    triggers: list[ExperimentTrigger] = Field(default_factory=list, max_length=16)
    network: ExperimentNetwork | None = None
    meta: dict[str, Any] = Field(default_factory=dict)


class ExpState(_JsonMessage):
    run_id: str
    state: str  # idle | armed | running | paused | done | aborted
    spec_hash: str
    started_at_ms: int = 0
    elapsed_ms: int = 0
    progress: float = 0.0
    samples: int = 0
    buffered: int = 0
    reason: str | None = None


class BulkBegin(_JsonMessage):
    stream: str
    total: int


class BulkEnd(_JsonMessage):
    stream: str
    crc32: int


class NetPeer(_JsonMessage):
    mac: str
    node_id: int
    rssi: int
    tx: int
    rx: int
    lost: int
    rtt_ms: float
    last_seen_ms: int


class NetReport(_JsonMessage):
    ts_ms: int
    role: str
    channel: int
    peers: list[NetPeer] = Field(default_factory=list)


class NetCmd(_JsonMessage):
    """PROTOCOL.md section 4.10 names NET_CMD ("network-experiment control")
    but gives no worked JSON example, unlike every other control message.
    Modelled after CMD/CMD_ACK's op+args shape (the pattern every other
    station->node control message in this protocol uses) since that is the
    only precedent in the document; `extra="allow"` means a differently-
    shaped real NET_CMD still round-trips through this model without being
    rejected."""

    id: int
    op: str
    args: dict[str, Any] = Field(default_factory=dict)


# ===========================================================================
# Packed data-plane messages
# ===========================================================================

_HEARTBEAT_STRUCT = struct.Struct("<IIIBBbB")  # uptime,heap_free,heap_min,state,flags,rssi,reserved


@dataclass(slots=True)
class Heartbeat:
    uptime_ms: int
    heap_free: int
    heap_min: int
    state: int
    flags: int
    rssi: int
    reserved: int = 0

    SIZE: ClassVar[int] = _HEARTBEAT_STRUCT.size

    def to_bytes(self) -> bytes:
        return _HEARTBEAT_STRUCT.pack(
            self.uptime_ms, self.heap_free, self.heap_min,
            self.state, self.flags, self.rssi, self.reserved,
        )

    @classmethod
    def from_bytes(cls, data: bytes) -> "Heartbeat":
        if len(data) != cls.SIZE:
            raise MessageDecodeError(f"HEARTBEAT must be exactly {cls.SIZE} bytes, got {len(data)}")
        uptime, heap_free, heap_min, state, flags, rssi, reserved = _HEARTBEAT_STRUCT.unpack(data)
        return cls(uptime, heap_free, heap_min, state, flags, rssi, reserved)

    def flag_set(self) -> set[str]:
        bits = spec.bitfields().get("heartbeat_flags", {})
        return {name for bit, name in bits.items() if self.flags & (1 << bit)}

    def state_name(self) -> str:
        return spec.enums().get("node_state", {}).get(self.state, f"unknown({self.state})")


@dataclass(slots=True)
class Sample:
    ch: int
    dt_ms: int
    enc: int
    value: Any

    def to_bytes(self) -> bytes:
        return struct.pack("<BHB", self.ch, self.dt_ms, self.enc) + pack_sample_value(self.enc, self.value)


@dataclass(slots=True)
class Telemetry:
    base_ts_ms: int
    flags: int
    samples: list[Sample] = field(default_factory=list)

    HEADER_SIZE: ClassVar[int] = 6

    def to_bytes(self) -> bytes:
        if not (1 <= len(self.samples) <= 64):
            raise ValueError(f"TELEMETRY count must be 1..64, got {len(self.samples)}")
        out = struct.pack("<IBB", self.base_ts_ms, len(self.samples), self.flags)
        for s in self.samples:
            out += s.to_bytes()
        return out

    @classmethod
    def from_bytes(cls, data: bytes) -> "Telemetry":
        if len(data) < cls.HEADER_SIZE:
            raise MessageDecodeError("TELEMETRY shorter than header")
        base_ts_ms, count, flags = struct.unpack_from("<IBB", data, 0)
        offset = cls.HEADER_SIZE
        samples: list[Sample] = []
        for _ in range(count):
            if offset + 4 > len(data):
                raise MessageDecodeError("TELEMETRY truncated sample header")
            ch, dt_ms, enc = struct.unpack_from("<BHB", data, offset)
            offset += 4
            size = sample_value_size(enc)
            if offset + size > len(data):
                raise MessageDecodeError("TELEMETRY truncated sample value")
            value = unpack_sample_value(enc, data[offset:offset + size])
            offset += size
            samples.append(Sample(ch=ch, dt_ms=dt_ms, enc=enc, value=value))
        return cls(base_ts_ms=base_ts_ms, flags=flags, samples=samples)

    def flag_set(self) -> set[str]:
        bits = spec.bitfields().get("telemetry_flags", {})
        return {name for bit, name in bits.items() if self.flags & (1 << bit)}


_TELEM_ACK_STRUCT = struct.Struct("<HHH")  # node, last_seq, flags


@dataclass(slots=True)
class TelemAck:
    node: int
    last_seq: int
    flags: int = 0

    SIZE: ClassVar[int] = _TELEM_ACK_STRUCT.size

    def to_bytes(self) -> bytes:
        return _TELEM_ACK_STRUCT.pack(self.node, self.last_seq, self.flags)

    @classmethod
    def from_bytes(cls, data: bytes) -> "TelemAck":
        if len(data) != cls.SIZE:
            raise MessageDecodeError(f"TELEM_ACK must be exactly {cls.SIZE} bytes, got {len(data)}")
        node, last_seq, flags = _TELEM_ACK_STRUCT.unpack(data)
        return cls(node, last_seq, flags)


@dataclass(slots=True)
class Log:
    ts_ms: int
    level: int
    tag: str
    msg: str

    HEADER_SIZE: ClassVar[int] = 6

    def to_bytes(self) -> bytes:
        tag_b = self.tag.encode("utf-8")
        msg_b = self.msg.encode("utf-8")
        if len(tag_b) > 255:
            raise ValueError("LOG tag must fit in a u8 length")
        return struct.pack("<IBB", self.ts_ms, self.level, len(tag_b)) + tag_b + msg_b

    @classmethod
    def from_bytes(cls, data: bytes) -> "Log":
        if len(data) < cls.HEADER_SIZE:
            raise MessageDecodeError("LOG shorter than header")
        ts_ms, level, tag_len = struct.unpack_from("<IBB", data, 0)
        offset = cls.HEADER_SIZE
        if offset + tag_len > len(data):
            raise MessageDecodeError("LOG truncated tag")
        tag = data[offset:offset + tag_len].decode("utf-8", errors="replace")
        offset += tag_len
        msg = data[offset:].decode("utf-8", errors="replace")
        return cls(ts_ms=ts_ms, level=level, tag=tag, msg=msg)

    def level_name(self) -> str:
        return spec.enums().get("log_level", {}).get(self.level, f"unknown({self.level})")


@dataclass(slots=True)
class BulkChunk:
    index: int
    data: bytes

    HEADER_SIZE: ClassVar[int] = 2

    def to_bytes(self) -> bytes:
        return struct.pack("<H", self.index) + self.data

    @classmethod
    def from_bytes(cls, data: bytes) -> "BulkChunk":
        if len(data) < cls.HEADER_SIZE:
            raise MessageDecodeError("BULK_CHUNK shorter than header")
        (index,) = struct.unpack_from("<H", data, 0)
        return cls(index=index, data=data[cls.HEADER_SIZE:])


_TIME_SYNC_STRUCT = struct.Struct("<QIIQ")  # t1_host_us, t2_node_ms, t3_node_ms, reserved


@dataclass(slots=True)
class TimeSync:
    t1_host_us: int
    t2_node_ms: int
    t3_node_ms: int
    reserved: int = 0

    SIZE: ClassVar[int] = _TIME_SYNC_STRUCT.size

    def to_bytes(self) -> bytes:
        return _TIME_SYNC_STRUCT.pack(self.t1_host_us, self.t2_node_ms, self.t3_node_ms, self.reserved)

    @classmethod
    def from_bytes(cls, data: bytes) -> "TimeSync":
        if len(data) != cls.SIZE:
            raise MessageDecodeError(f"TIME_SYNC must be exactly {cls.SIZE} bytes, got {len(data)}")
        t1, t2, t3, reserved = _TIME_SYNC_STRUCT.unpack(data)
        return cls(t1, t2, t3, reserved)


# ---------------------------------------------------------------------------
# Dispatch table: frame type code -> decoder callable(payload: bytes) -> obj
# ---------------------------------------------------------------------------
_DECODERS: dict[int, Any] = {
    TYPE_HELLO: Hello.from_payload,
    TYPE_HELLO_ACK: HelloAck.from_payload,
    TYPE_HEARTBEAT: Heartbeat.from_bytes,
    TYPE_TELEMETRY: Telemetry.from_bytes,
    TYPE_TELEM_ACK: TelemAck.from_bytes,
    TYPE_LOG: Log.from_bytes,
    TYPE_EVENT: Event.from_payload,
    TYPE_CMD: Cmd.from_payload,
    TYPE_CMD_ACK: CmdAck.from_payload,
    TYPE_EXP_SET: ExperimentSpec.from_payload,
    TYPE_EXP_STATE: ExpState.from_payload,
    TYPE_BULK_BEGIN: BulkBegin.from_payload,
    TYPE_BULK_CHUNK: BulkChunk.from_bytes,
    TYPE_BULK_END: BulkEnd.from_payload,
    TYPE_NET_REPORT: NetReport.from_payload,
    TYPE_NET_CMD: NetCmd.from_payload,
    TYPE_TIME_SYNC: TimeSync.from_bytes,
}


def decode_message(type_code: int, payload: bytes) -> Any:
    """Decode a frame payload given its type code. Codes 0x80-0xFF
    (experiment-defined, PROTOCOL.md section 4) and any other unknown code
    are returned as the raw payload bytes -- the gateway passes those
    through to the UI opaquely rather than failing."""
    decoder = _DECODERS.get(type_code)
    if decoder is None:
        return payload
    return decoder(payload)

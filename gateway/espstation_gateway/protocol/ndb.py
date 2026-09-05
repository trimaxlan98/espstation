# The Node Database (NDB): PROTOCOL.md section 4.1 is explicit that "the
# station never hard-codes channel ids; every chart, unit and limit is
# driven by what the node declares" in HELLO. This module holds that
# per-node channel table and the enc+scale -> semantic value conversion
# described in PROTOCOL.md section 4.4 and docs/EXPERIMENTS.md.
#
# Two independent things determine how a raw TELEMETRY sample becomes a
# semantic value:
#   1. `enc` (in the sample itself) is the *transport* type -- a node may
#      send i16 for a channel whose NDB-declared *semantic* type is f32, to
#      save bandwidth, without renegotiating (PROTOCOL.md 4.4).
#   2. `scale` (declared per-channel in an EXP_SET, EXPERIMENTS.md) is the
#      factor that reconstructs the real-world value from that downgraded
#      integer, e.g. raw i16 counts * 0.0001 -> volts. It is active only
#      while that experiment's channel config is installed, which is why
#      it is tracked separately from the (fairly static) NDB itself.
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from .messages import ExperimentChannel, ExperimentSpec, Hello, NdbChannel
from . import spec as protocol_spec


class UnknownChannelError(KeyError):
    pass


# Semantic NDB `type` string -> the Python type samples should be coerced to
# once `enc`+`scale` have produced a numeric value.
_SEMANTIC_PYTYPE: dict[str, type] = {
    "u8": int, "i8": int, "u16": int, "i16": int, "u32": int, "i32": int,
    "u64": int, "i64": int, "f32": float, "f64": float, "bool": bool,
}


def _coerce(value: Any, semantic_type: str) -> Any:
    pytype = _SEMANTIC_PYTYPE.get(semantic_type)
    if pytype is None:
        return value
    if pytype is bool:
        return bool(value)
    if pytype is int and isinstance(value, float) and value.is_integer():
        return int(value)
    return pytype(value) if pytype is float else value


@dataclass
class NodeDatabase:
    """Per-node channel table plus the currently-active experiment's
    per-channel scale overrides. One instance per known node id."""

    node_id: int
    label: str = ""
    channels_by_id: dict[int, NdbChannel] = field(default_factory=dict)
    channels_by_key: dict[str, NdbChannel] = field(default_factory=dict)
    _active_scale: dict[int, float] = field(default_factory=dict)  # channel id -> scale
    _active_enc: dict[int, str] = field(default_factory=dict)  # channel id -> declared transport enc name

    @classmethod
    def from_hello(cls, hello: Hello) -> "NodeDatabase":
        ndb = cls(node_id=hello.node_id, label=hello.label)
        ndb.merge_hello(hello)
        return ndb

    def merge_hello(self, hello: Hello) -> None:
        """A node may extend its NDB at runtime by re-sending HELLO (e.g. a
        sensor was hot-plugged). Merge rather than replace so channel ids
        already referenced by stored samples keep resolving."""
        self.label = hello.label
        for ch in hello.ndb:
            self.channels_by_id[ch.id] = ch
            self.channels_by_key[ch.key] = ch

    def by_id(self, channel_id: int) -> NdbChannel:
        try:
            return self.channels_by_id[channel_id]
        except KeyError:
            raise UnknownChannelError(f"node {self.node_id}: no NDB channel with id={channel_id}") from None

    def by_key(self, key: str) -> NdbChannel:
        try:
            return self.channels_by_key[key]
        except KeyError:
            raise UnknownChannelError(f"node {self.node_id}: no NDB channel with key={key!r}") from None

    # -- active experiment channel config (enc/scale overrides) -----------

    def apply_experiment_channels(self, channels: list[ExperimentChannel]) -> None:
        """Install the enc/scale overrides an EXP_SET declares per channel.
        Raises UnknownChannelError if a channel key isn't in this node's
        NDB -- this is exactly validation gate 1 from EXPERIMENTS.md
        ("does every channels[].key exist in that node's NDB?")."""
        for ch in channels:
            ndb_ch = self.by_key(ch.key)  # raises if unknown
            if ch.scale is not None:
                self._active_scale[ndb_ch.id] = ch.scale
            else:
                self._active_scale.pop(ndb_ch.id, None)
            if ch.enc is not None:
                self._active_enc[ndb_ch.id] = ch.enc

    def clear_active_channels(self) -> None:
        self._active_scale.clear()
        self._active_enc.clear()

    # -- conversion ---------------------------------------------------------

    def convert(self, channel_id: int, raw_value: Any) -> Any:
        """enc -> semantic value: reverse any active scale, then coerce to
        the NDB-declared semantic type. `raw_value` is what
        messages.unpack_sample_value() already decoded per the sample's
        `enc` byte (i.e. the transport-typed number/bool)."""
        ndb_ch = self.by_id(channel_id)
        scale = self._active_scale.get(channel_id)
        value = raw_value * scale if scale is not None else raw_value
        return _coerce(value, ndb_ch.type)

    # -- station-side validation (EXPERIMENTS.md gate 1) --------------------

    def validate_spec(self, spec: ExperimentSpec, *, rate_limit_hz: float | None = None) -> list[str]:
        """Semantic checks the node can't do cheaply, run before EXP_SET is
        ever sent: every channel key must exist in this node's NDB, and the
        aggregate declared sample rate must fit the node's budget. Returns a
        list of human-readable error strings; empty list == valid.

        `rate_limit_hz` defaults to the protocol's declared
        telemetry_rate_limit_hz (espstation.protocol.yaml `timing:` block);
        HELLO_ACK.policy.telemetry_rate_limit_hz should override it once a
        session is established, since that is the node's live, possibly
        lower, budget.
        """
        errors: list[str] = []
        if rate_limit_hz is None:
            rate_limit_hz = float(protocol_spec.timing().get("telemetry_rate_limit_hz", 200))

        total_rate = 0.0
        for ch in spec.channels:
            if ch.key not in self.channels_by_key:
                errors.append(f"channel {ch.key!r} not present in node {self.node_id}'s NDB")
                continue
            total_rate += ch.rate_hz
        if total_rate > rate_limit_hz:
            errors.append(
                f"aggregate channel rate {total_rate:g} Hz exceeds node budget {rate_limit_hz:g} Hz"
            )
        return errors


class NodeRegistry:
    """All known nodes' NDBs, keyed by node id. Owned by a Link (see
    transports/base.py) or the app-level store; kept separate from SQLite
    because it's live/in-memory session state, not durable history."""

    def __init__(self) -> None:
        self._nodes: dict[int, NodeDatabase] = {}

    def on_hello(self, hello: Hello) -> NodeDatabase:
        ndb = self._nodes.get(hello.node_id)
        if ndb is None:
            ndb = NodeDatabase.from_hello(hello)
            self._nodes[hello.node_id] = ndb
        else:
            ndb.merge_hello(hello)
        return ndb

    def get(self, node_id: int) -> NodeDatabase | None:
        return self._nodes.get(node_id)

    def require(self, node_id: int) -> NodeDatabase:
        ndb = self._nodes.get(node_id)
        if ndb is None:
            raise UnknownChannelError(f"no NDB registered for node {node_id} (no HELLO seen yet)")
        return ndb

    def __iter__(self):
        return iter(self._nodes.values())

    def __len__(self) -> int:
        return len(self._nodes)

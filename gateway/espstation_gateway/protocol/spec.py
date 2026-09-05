# Loads protocol/espstation.protocol.yaml at runtime and exposes it as
# indexed lookups (message type <-> code, encodings table, enums, ...).
#
# Why runtime loading instead of hand-copied constants: the gateway builder
# brief for this project is explicit that message-type codes and the
# encoding table must be sourced from the YAML rather than duplicated by
# hand, precisely because protocol/PROTOCOL.md is "law" and
# tools/check_protocol.py gates drift between the YAML and every
# implementation. Hand-copied constants are exactly the kind of drift that
# gate exists to catch -- so this module removes the opportunity for it.
#
# Search order for the YAML file:
#   1. ESPSTATION_PROTOCOL_YAML env var, if set (tests / packaging override).
#   2. Walk up from this file looking for a sibling `protocol/` directory
#      (the normal monorepo layout: gateway/ and protocol/ share a parent).
# If neither resolves, raise a clear error rather than silently hard-coding
# values that could drift from the spec.
from __future__ import annotations

import functools
import os
from pathlib import Path
from typing import Any

import yaml


class ProtocolSpecNotFoundError(RuntimeError):
    pass


def _locate_yaml() -> Path:
    env = os.environ.get("ESPSTATION_PROTOCOL_YAML")
    if env:
        p = Path(env)
        if not p.is_file():
            raise ProtocolSpecNotFoundError(f"ESPSTATION_PROTOCOL_YAML={env!r} does not exist")
        return p

    here = Path(__file__).resolve()
    for parent in here.parents:
        candidate = parent / "protocol" / "espstation.protocol.yaml"
        if candidate.is_file():
            return candidate
    raise ProtocolSpecNotFoundError(
        "Could not locate protocol/espstation.protocol.yaml by walking up from "
        f"{here}. Set ESPSTATION_PROTOCOL_YAML to override."
    )


@functools.lru_cache(maxsize=1)
def load_spec() -> dict[str, Any]:
    path = _locate_yaml()
    with path.open("r", encoding="utf-8") as fh:
        return yaml.safe_load(fh)


@functools.lru_cache(maxsize=1)
def message_types() -> dict[str, int]:
    """name -> code, e.g. {"HELLO": 0x01, ...}."""
    spec = load_spec()
    return {m["name"]: m["code"] for m in spec["messages"]}


@functools.lru_cache(maxsize=1)
def message_names() -> dict[int, str]:
    """code -> name, the inverse of message_types()."""
    return {code: name for name, code in message_types().items()}


@functools.lru_cache(maxsize=1)
def message_meta() -> dict[str, dict[str, Any]]:
    """name -> full message table row (dir, plane, schema/struct)."""
    spec = load_spec()
    return {m["name"]: m for m in spec["messages"]}


@functools.lru_cache(maxsize=1)
def encodings() -> dict[int, dict[str, Any]]:
    """The `enc` byte in TELEMETRY samples: code -> {name, size}."""
    spec = load_spec()
    return {int(code): info for code, info in spec["encodings"].items()}


@functools.lru_cache(maxsize=1)
def encoding_by_name() -> dict[str, int]:
    return {info["name"]: code for code, info in encodings().items()}


@functools.lru_cache(maxsize=1)
def enums() -> dict[str, Any]:
    return load_spec().get("enums", {})


@functools.lru_cache(maxsize=1)
def bitfields() -> dict[str, dict[int, str]]:
    spec = load_spec().get("bitfields", {})
    return {name: {int(bit): flag for bit, flag in bits.items()} for name, bits in spec.items()}


@functools.lru_cache(maxsize=1)
def commands() -> dict[str, dict[str, Any]]:
    return load_spec().get("commands", {})


@functools.lru_cache(maxsize=1)
def channel_id_ranges() -> list[dict[str, Any]]:
    return load_spec().get("channel_id_ranges", [])


@functools.lru_cache(maxsize=1)
def system_channels() -> list[dict[str, Any]]:
    return load_spec().get("system_channels", [])


@functools.lru_cache(maxsize=1)
def timing() -> dict[str, Any]:
    return load_spec().get("timing", {})


@functools.lru_cache(maxsize=1)
def protocol_meta() -> dict[str, Any]:
    return load_spec().get("protocol", {})

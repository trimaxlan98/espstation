#!/usr/bin/env python3
"""Protocol drift gate.

`protocol/espstation.protocol.yaml` is the single source of truth for the wire
format. Three implementations restate it: the firmware codec, the gateway
codec, and (for the JSON shapes only) the desktop's types. Restating something
three times is how it drifts, and wire-format drift is the worst kind of bug
here — it shows up as corrupted telemetry on hardware, far from the change that
caused it.

This does not attempt to parse C or Python properly. It checks that every
constant the YAML declares is *present with the right value* in each
implementation, which catches the realistic failure: someone adds a message or
renumbers one and updates two places out of three.

A component that does not exist yet is SKIPped, not failed, so the gate is
usable while a sprint is still building one of them.

Exit 0 = in sync (or skipped), 1 = drift, 2 = the YAML itself is broken.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    print("PyYAML required: pip install pyyaml (or use gateway/.venv)", file=sys.stderr)
    raise SystemExit(2)

ROOT = Path(__file__).resolve().parent.parent
SPEC = ROOT / "protocol" / "espstation.protocol.yaml"

FIRMWARE_CODEC = ROOT / "firmware" / "components" / "esps_proto"
GATEWAY_CODEC = ROOT / "gateway" / "espstation_gateway" / "protocol"


class Result:
    def __init__(self) -> None:
        self.failures: list[str] = []
        self.skips: list[str] = []
        self.checks = 0

    def check(self, ok: bool, message: str) -> None:
        self.checks += 1
        if not ok:
            self.failures.append(message)

    def skip(self, message: str) -> None:
        self.skips.append(message)


def read_tree(path: Path, suffixes: tuple[str, ...]) -> str:
    """All source in a directory as one blob. Crude on purpose: we are looking
    for the presence of a value, not analysing structure."""
    parts = []
    for p in sorted(path.rglob("*")):
        if p.is_file() and p.suffix in suffixes:
            parts.append(p.read_text(encoding="utf-8", errors="replace"))
    return "\n".join(parts)


def hex_variants(code: int) -> list[str]:
    """The same number as a C or Python author would plausibly write it."""
    return [f"0x{code:02X}", f"0x{code:02x}", str(code)]


def find_named_value(blob: str, name: str, code: int) -> bool:
    """True if `name` appears on a line that also carries its numeric value.

    Tolerant of the naming each language prefers (ESPS_MSG_HELLO, MSG_HELLO,
    HELLO = 0x01, "HELLO": 1) because forcing one convention across C, Python
    and TypeScript would be a worse tax than this looseness costs.
    """
    for line in blob.splitlines():
        if name not in line:
            continue
        if any(v in line for v in hex_variants(code)):
            return True
    return False


def find_spec_backed_name(blob: str, name: str) -> bool:
    """True when Python obtains a named constant from the YAML-backed table.

    The gateway intentionally loads message values from ``spec.message_types``
    instead of copying numeric literals.  In that case requiring the literal
    value beside every name would reintroduce the very duplication this gate is
    meant to prevent.  Still require an explicit lookup for every message so a
    missing gateway binding remains a drift failure.
    """
    if "spec.message_types()" not in blob:
        return False
    return re.search(
        rf"^\s*TYPE_{re.escape(name)}\s*=\s*_TYPES\[['\"]{re.escape(name)}['\"]\]\s*$",
        blob,
        re.MULTILINE,
    ) is not None


def check_impl(res: Result, label: str, path: Path, suffixes: tuple[str, ...], spec: dict) -> None:
    if not path.exists():
        res.skip(f"{label}: {path.relative_to(ROOT)} does not exist yet")
        return
    blob = read_tree(path, suffixes)
    if not blob.strip():
        res.skip(f"{label}: no source files under {path.relative_to(ROOT)}")
        return

    for msg in spec["messages"]:
        res.check(
            find_named_value(blob, msg["name"], msg["code"])
            or (label == "gateway" and find_spec_backed_name(blob, msg["name"])),
            f"{label}: message {msg['name']} (0x{msg['code']:02X}) not found with its value",
        )

    crc = spec["crc"]
    res.check(
        any(v in blob for v in ("0x1021", "0X1021", "4129")),
        f"{label}: CRC polynomial {crc['poly']:#06x} not found",
    )
    res.check(
        any(v in blob for v in ("0xFFFF", "0xffff", "65535")),
        f"{label}: CRC init value not found",
    )

    proto = spec["protocol"]
    res.check(
        str(proto["max_payload"]) in blob,
        f"{label}: max_payload {proto['max_payload']} not found",
    )
    res.check(
        str(proto["max_payload_espnow"]) in blob,
        f"{label}: max_payload_espnow {proto['max_payload_espnow']} not found",
    )
    res.check(
        re.search(r"\b" + str(spec["header"]["size"]) + r"\b", blob) is not None,
        f"{label}: header size {spec['header']['size']} not found",
    )


def check_spec_internals(res: Result, spec: dict) -> None:
    """The YAML can be self-inconsistent too; catch that before blaming code."""
    codes = [m["code"] for m in spec["messages"]]
    res.check(len(codes) == len(set(codes)), "spec: duplicate message codes")

    names = [m["name"] for m in spec["messages"]]
    res.check(len(names) == len(set(names)), "spec: duplicate message names")

    reserved = spec["reserved_ranges"][0]
    clashing = [f"{n}=0x{c:02X}" for n, c in zip(names, codes) if reserved["from"] <= c <= reserved["to"]]
    res.check(not clashing, f"spec: messages inside the reserved range: {clashing}")

    for name, struct in spec["structs"].items():
        if "size" not in struct:
            continue
        end = 0
        for f in struct["fields"]:
            if "offset" not in f:
                continue
            width = {"u8": 1, "i8": 1, "u16": 2, "i16": 2, "u32": 4, "i32": 4, "f32": 4, "u64": 8}.get(f["type"])
            if width is None:
                continue
            end = max(end, f["offset"] + width)
        res.check(
            end == struct["size"],
            f"spec: struct {name} declares size {struct['size']} but its fields end at {end}",
        )

    for ch in spec["system_channels"]:
        lo, hi = spec["channel_id_ranges"][0]["from"], spec["channel_id_ranges"][0]["to"]
        res.check(
            lo <= ch["id"] <= hi,
            f"spec: system channel {ch['key']} id {ch['id']} outside the system range {lo}-{hi}",
        )


def main() -> int:
    if not SPEC.exists():
        print(f"missing {SPEC}", file=sys.stderr)
        return 2
    spec = yaml.safe_load(SPEC.read_text(encoding="utf-8"))

    res = Result()
    check_spec_internals(res, spec)
    check_impl(res, "firmware", FIRMWARE_CODEC, (".c", ".h"), spec)
    check_impl(res, "gateway", GATEWAY_CODEC, (".py",), spec)

    for s in res.skips:
        print(f"SKIP  {s}")
    for f in res.failures:
        print(f"DRIFT {f}", file=sys.stderr)

    if res.failures:
        print(
            f"\n{len(res.failures)} of {res.checks} checks failed. "
            "The wire format and its implementations disagree — fix them in one commit "
            "(protocol/PROTOCOL.md, the YAML, the firmware codec, the gateway codec).",
            file=sys.stderr,
        )
        return 1

    print(f"protocol in sync — {res.checks} checks passed, {len(res.skips)} skipped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

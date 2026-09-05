# pyserial transport. pyserial is a blocking library, so every syscall runs
# in the default executor -- the asyncio event loop must never block on a
# read(). Device paths are validated against an allowlist before being
# handed to pyserial (never format a user string into a shell command or
# trust it unchecked into an OS call).
#
# On this dev machine /dev/ttyUSB0 exists but the user isn't in the
# `dialout` group, so pyserial raises a bare PermissionError. We catch that
# specifically and re-raise SerialPermissionError with the actual fix,
# because "PermissionError: [Errno 13]" with a stack trace is not an
# actionable error message for someone plugging in a board.
from __future__ import annotations

import asyncio
import re
from dataclasses import dataclass
from typing import AsyncIterator

import serial
import serial.tools.list_ports

from .base import Transport
from ..protocol.cobs import encode as cobs_encode

# Linux ttyUSB*/ttyACM*/ttyS*, macOS /dev/cu.*|tty.*, Windows COMn.
_ALLOWED_PORT_RE = re.compile(r"^(/dev/(ttyUSB|ttyACM|ttyS)\d+|/dev/(cu|tty)\.[\w.-]+|COM\d+)$")


class InvalidPortPathError(ValueError):
    pass


class SerialPermissionError(RuntimeError):
    pass


class SerialOpenError(RuntimeError):
    pass


@dataclass(frozen=True, slots=True)
class PortInfo:
    path: str
    vid: int | None
    pid: int | None
    description: str
    in_use: bool = False


def list_ports() -> list[PortInfo]:
    """Enumerate serial ports via pyserial's platform backend. `in_use` is
    filled in by the caller (app.py knows which paths already have a Link)."""
    return [
        PortInfo(path=p.device, vid=p.vid, pid=p.pid, description=p.description or "")
        for p in serial.tools.list_ports.comports()
    ]


def validate_port_path(path: str) -> str:
    if not _ALLOWED_PORT_RE.match(path):
        raise InvalidPortPathError(
            f"{path!r} does not match an allowed serial device pattern "
            f"(expected /dev/ttyUSB*, /dev/ttyACM*, /dev/ttyS*, /dev/cu.*, /dev/tty.*, or COMn)"
        )
    return path


class SerialTransport(Transport):
    def __init__(self, path: str, baudrate: int = 115200, *, poll_interval: float = 0.02, read_chunk: int = 4096) -> None:
        self.path = validate_port_path(path)
        self.baudrate = baudrate
        self._poll_interval = poll_interval
        self._read_chunk = read_chunk
        self._ser: serial.Serial | None = None
        self._closing = False

    async def open(self) -> None:
        loop = asyncio.get_running_loop()

        def _open() -> serial.Serial:
            try:
                # timeout=0: non-blocking reads, we do our own polling so the
                # event loop stays responsive between bytes.
                return serial.Serial(self.path, self.baudrate, timeout=0)
            except PermissionError as exc:
                raise SerialPermissionError(
                    f"Permission denied opening {self.path}. On Linux: "
                    f"`sudo usermod -aG dialout $USER` then log out and back in. "
                    f"(You can check membership with `groups`.)"
                ) from exc
            except serial.SerialException as exc:
                raise SerialOpenError(f"Could not open {self.path}: {exc}") from exc

        self._ser = await loop.run_in_executor(None, _open)
        self._closing = False

    async def close(self) -> None:
        self._closing = True
        ser, self._ser = self._ser, None
        if ser is not None:
            loop = asyncio.get_running_loop()
            await loop.run_in_executor(None, ser.close)

    async def send(self, frame_bytes: bytes) -> None:
        if self._ser is None:
            raise SerialOpenError("SerialTransport is not open")
        wire = cobs_encode(frame_bytes) + b"\x00"
        loop = asyncio.get_running_loop()
        await loop.run_in_executor(None, self._ser.write, wire)

    async def receive(self) -> AsyncIterator[bytes]:
        loop = asyncio.get_running_loop()
        while not self._closing:
            ser = self._ser
            if ser is None:
                return
            chunk = await loop.run_in_executor(None, ser.read, self._read_chunk)
            if chunk:
                yield chunk
            else:
                await asyncio.sleep(self._poll_interval)

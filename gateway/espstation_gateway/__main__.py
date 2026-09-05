# CLI entry point: `python -m espstation_gateway [--sim | --serial PORT] ...`
#
# access_log=False on the uvicorn server is deliberate, not an oversight:
# WS auth is a `?token=` query parameter (app.py's docstring explains why --
# browsers can't set a custom header on a WS handshake), and uvicorn's
# default access logger prints the full request line including the query
# string. That would put the bearer token in plaintext in every gateway
# log, which is exactly what "never log tokens or query strings" rules out.
from __future__ import annotations

import argparse
import re
from pathlib import Path

import uvicorn

from .app import DEFAULT_PORT, DEFAULT_TOKEN, Settings, create_app
from .store import DEFAULT_DB_PATH

_SERIAL_PATH_RE = re.compile(r"^(/dev/(ttyUSB|ttyACM|ttyS)\d+|/dev/(cu|tty)\.[\w.-]+|COM\d+)$")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="espstation_gateway", description="EspStation station-side gateway")
    parser.add_argument("--host", default="127.0.0.1", help="bind address (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"bind port (default: {DEFAULT_PORT})")
    parser.add_argument("--token", default=DEFAULT_TOKEN, help="bearer token (default: dev token, change for LAN exposure)")
    parser.add_argument("--db", default=str(DEFAULT_DB_PATH), help="path to the SQLite database file")
    parser.add_argument("--sim", nargs="?", const=3, type=int, metavar="N", help="preload N simulated nodes (default 3 if flag given with no value)")
    parser.add_argument("--serial", metavar="PATH", help="attach a real node over this serial port at startup (e.g. /dev/ttyUSB0)")
    return parser


def main(argv: list[str] | None = None) -> None:
    args = build_parser().parse_args(argv)

    if args.serial is not None and not _SERIAL_PATH_RE.match(args.serial):
        raise SystemExit(f"--serial {args.serial!r} does not look like a serial device path")

    settings = Settings(
        token=args.token,
        host=args.host,
        port=args.port,
        db_path=Path(args.db),
        sim_preload=args.sim or 0,
    )
    app = create_app(settings)

    if args.serial is not None:
        # Attached on the startup event so it shares the runtime's event
        # loop instead of trying to open the port before uvicorn is serving.
        @app.on_event("startup")
        async def _attach_serial() -> None:  # pragma: no cover - exercised only with real hardware
            await app.state.runtime.attach_serial(args.serial)

    uvicorn.run(app, host=settings.host, port=settings.port, access_log=False)


if __name__ == "__main__":
    main()

# Setup

Everything except flashing an actual board works with **zero hardware** — the
gateway's simulated nodes speak the real protocol through the real codec. Start
there; add the ESP32 when you need it.

Prerequisites: Python ≥ 3.11, Node ≥ 22.5, `gcc` and `make` (host firmware
tests), `git`.

## 1. Firmware host tests — 30 seconds, no toolchain

```bash
make -C firmware/test/host test
```

This gates the codec (framing, CRC, COBS, packed layouts), which is the code
most likely to break and the hardest to debug on target. Run it constantly.

## 2. Gateway

```bash
cd gateway
python3 -m venv .venv || ~/.local/bin/virtualenv .venv   # see note below
.venv/bin/pip install -e ".[dev]"
.venv/bin/python -m pytest tests/ -q
.venv/bin/python -m espstation_gateway --sim --port 8787
```

> **Note for this development machine:** `python3 -m venv` fails under Python
> 3.14 (no `ensurepip`). Use `~/.local/bin/virtualenv .venv`. Target Python
> **3.11** compatibility regardless of the local interpreter.

## 3. Desktop

```bash
cd desktop
npm install
npm run typecheck && npm test && npm run build
npm run dev        # needs a gateway running
```

The app starts pre-configured for `127.0.0.1:8787` with the dev token
`espstation-dev`.

## 4. ESP32 toolchain — only when you have a board

PlatformIO vendors its own compiler, CMake and Ninja, so there is no
system-wide ESP-IDF install to manage.

```bash
~/.local/bin/virtualenv .venv-tools
.venv-tools/bin/pip install platformio
.venv-tools/bin/pio pkg install -d firmware -e esp32dev   # ~1 GB, once
.venv-tools/bin/pio run -d firmware -e esp32dev
.venv-tools/bin/pio run -d firmware -e esp32dev -t upload
```

Targets other than `esp32dev` (`esp32s3`, `esp32c3`, `esp32c6`) have build
environments defined but **no hardware has validated them** — see D-13.

## 5. Serial port permissions — you will hit this

On Linux, `/dev/ttyUSB0` is owned by `root:dialout` with mode `660`. If your
user is not in `dialout`, every port open fails with `PermissionError` and the
symptom is a node that never appears.

```bash
groups                          # is 'dialout' listed?
sudo usermod -aG dialout $USER
# then LOG OUT AND BACK IN — a new shell is not enough, the group is attached
# to your login session. `newgrp dialout` works for one shell as a stopgap.
ls -l /dev/ttyUSB0              # confirm the device exists
```

A CP2102 or CH340 bridge (most DevKits) appears as `/dev/ttyUSB*`; boards with
native USB (ESP32-S3, C3) appear as `/dev/ttyACM*`.

## 6. Verify the whole path

```bash
tools/enlp_sniff.py /dev/ttyUSB0
```

You should see the boot-ROM banner as raw text, then a decoded `HELLO` with the
node's channel table, then heartbeats at 1 Hz. If you see raw text and no
frames, the firmware is running but the link is not — check the baud rate. If
you see nothing at all, it is permissions (§5) or the wrong port.

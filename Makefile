# One entry point for every gate in the repo.
#
# The point is that a contributor — or an agent — never has to remember which
# suite lives where, and CI runs exactly what a person runs locally. When those
# two drift, the local command is the one that gets trusted and CI becomes
# noise, so this file is the shared definition rather than a convenience.

PIO      := .venv-tools/bin/pio
GW_PY    := gateway/.venv/bin/python
FW_ENV   ?= esp32dev
PORT     ?= /dev/ttyUSB0

.PHONY: help check contracts fw-test bench-test fw-build fw-flash fw-monitor \
        gateway-test gateway-run desktop-test desktop-dev sniff clean

help:
	@echo "EspStation — targets"
	@echo ""
	@echo "  make check          all gates that need no hardware (CI also builds the pio envs)"
	@echo "  make contracts      protocol drift + agent-role sync"
	@echo "  make fw-test        pure-C11 component tests on the host: esps_proto codec,"
	@echo "                      esps_dio, esps_morse (gcc + -Werror + ASan/UBSan)"
	@echo "                      (sin make, p. ej. Windows: python3 firmware/test/host/run_tests.py)"
	@echo "  make bench-test     the bench practices' real .ino sketches on a host mock,"
	@echo "                      plus the serial bridge and the duplex viewer (needs g++)"
	@echo "  make fw-build       build firmware        [FW_ENV=$(FW_ENV)]"
	@echo "  make fw-flash       build and upload      [FW_ENV=$(FW_ENV)]"
	@echo "  make fw-monitor     serial monitor        [PORT=$(PORT)]"
	@echo "  make gateway-test   gateway pytest"
	@echo "  make gateway-run    gateway with simulated nodes + a virtual-cable pair on :8787"
	@echo "  make desktop-test   typecheck + vitest + build"
	@echo "  make desktop-dev    launch the app (needs a gateway running)"
	@echo "  make sniff          decoded ENLP frame dump [PORT=$(PORT)]  -- NO IMPLEMENTADO:"
	@echo "                      tools/enlp_sniff.py no existe todavia (S1, docs/plans/S1-link-is-real.md M4)"
	@echo ""
	@echo "First-time setup: docs/SETUP.md"

# Everything verifiable without an ESP32 attached. Ordered cheapest-first, so a
# protocol mistake surfaces in seconds rather than after a desktop build.
check: contracts fw-test bench-test gateway-test desktop-test
	@echo ""
	@echo "All hardware-free gates passed."

contracts:
	python3 tools/check_protocol.py
	python3 tools/sync_agents.py --check

fw-test:
	$(MAKE) -C firmware/test/host test

# The Arduino sketches of the bench practices are third implementations of
# their links: esps_dio / dio_link.py for the two-wire link, esps_morse /
# morse_link.py for the Morse one. Each of these runs the REAL .ino on a
# minimal host mock and checks it against its SPEC, so no pair can drift
# silently. The last two also cover the browser-facing tools: the serial
# bridge and the duplex viewer, replayed against recorded evidence.
# Needs g++ and the gateway venv (CI: `make bench-test GW_PY=python`).
# Proves logic only, never timing -- that is measured on the bench.
bench-test:
	$(GW_PY) bench/practicas/enlace-digital/tests/run_tests.py
	$(GW_PY) bench/practicas/clave-morse/tests/run_tests.py
	$(GW_PY) bench/practicas/morse-duplex/tests/run_tests.py
	$(GW_PY) bench/practicas/clave-morse/tests/test_puente.py
	$(GW_PY) bench/practicas/morse-duplex/tests/test_visor.py

fw-build:
	$(PIO) run -d firmware -e $(FW_ENV)

fw-flash:
	$(PIO) run -d firmware -e $(FW_ENV) -t upload --upload-port $(PORT)

fw-monitor:
	$(PIO) device monitor -p $(PORT) -b 115200

gateway-test:
	$(GW_PY) -m pytest gateway/tests/ -q

gateway-run:
	$(GW_PY) -m espstation_gateway --sim --sim-dio --port 8787

desktop-test:
	cd desktop && npm run typecheck && npm test && npm run build

desktop-dev:
	cd desktop && npm run dev

# tools/enlp_sniff.py DOES NOT EXIST in this repository. The target is kept
# so the name stays reserved and the failure says why, instead of an ENOENT
# from python3 that reads like a broken install. It is planned in
# docs/plans/S1-link-is-real.md (M4). AGENTS.md, the espstation skills and the
# bug issue template all still mention it as if it shipped.
sniff:
	@echo "make sniff: tools/enlp_sniff.py is not in this repository yet."
	@echo "            Planned in docs/plans/S1-link-is-real.md (M4)."
	@echo "            For raw bytes meanwhile: python3 -m serial.tools.miniterm $(PORT) 115200"
	@exit 1

clean:
	$(MAKE) -C firmware/test/host clean
	rm -rf firmware/.pio desktop/out desktop/dist

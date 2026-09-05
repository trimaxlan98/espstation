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

.PHONY: help check contracts fw-test fw-build fw-flash fw-monitor \
        gateway-test gateway-run desktop-test desktop-dev sniff clean

help:
	@echo "EspStation — targets"
	@echo ""
	@echo "  make check          all gates that need no hardware (what CI runs)"
	@echo "  make contracts      protocol drift + agent-role sync"
	@echo "  make fw-test        firmware codec tests on the host (gcc + sanitizers)"
	@echo "  make fw-build       build firmware        [FW_ENV=$(FW_ENV)]"
	@echo "  make fw-flash       build and upload      [FW_ENV=$(FW_ENV)]"
	@echo "  make fw-monitor     serial monitor        [PORT=$(PORT)]"
	@echo "  make gateway-test   gateway pytest"
	@echo "  make gateway-run    gateway with simulated nodes on :8787"
	@echo "  make desktop-test   typecheck + vitest + build"
	@echo "  make desktop-dev    launch the app (needs a gateway running)"
	@echo "  make sniff          decoded ENLP frame dump [PORT=$(PORT)]"
	@echo ""
	@echo "First-time setup: docs/SETUP.md"

# Everything verifiable without an ESP32 attached. Ordered cheapest-first, so a
# protocol mistake surfaces in seconds rather than after a desktop build.
check: contracts fw-test gateway-test desktop-test
	@echo ""
	@echo "All hardware-free gates passed."

contracts:
	python3 tools/check_protocol.py
	python3 tools/sync_agents.py --check

fw-test:
	$(MAKE) -C firmware/test/host test

fw-build:
	$(PIO) run -d firmware -e $(FW_ENV)

fw-flash:
	$(PIO) run -d firmware -e $(FW_ENV) -t upload --upload-port $(PORT)

fw-monitor:
	$(PIO) device monitor -p $(PORT) -b 115200

gateway-test:
	$(GW_PY) -m pytest gateway/tests/ -q

gateway-run:
	$(GW_PY) -m espstation_gateway --sim --port 8787

desktop-test:
	cd desktop && npm run typecheck && npm test && npm run build

desktop-dev:
	cd desktop && npm run dev

sniff:
	python3 tools/enlp_sniff.py $(PORT)

clean:
	$(MAKE) -C firmware/test/host clean
	rm -rf firmware/.pio desktop/out desktop/dist

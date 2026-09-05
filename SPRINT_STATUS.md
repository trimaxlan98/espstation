# SPRINT_STATUS — S0: Foundation

**ESTADO: EN CURSO** · Inicio 2026-09-04 (sesión de arranque, orquestador Opus).

Este archivo es el **punto de reanudación**. Si una sesión se corta, se empieza
por lo que aquí figure como no hecho. Registra lo que *no* está hecho con el
mismo cuidado que lo hecho — un estado optimista es peor que ninguno.

## Objetivo de S0

Un repositorio en el que otro agente pueda entrar y contribuir sin preguntar
nada: contratos escritos, andamiaje que compila y corre, CI que cierra las
puertas, y convenciones de agentes listas.

## Entregado por el orquestador (contratos y gobierno)

- [x] `protocol/PROTOCOL.md` — ENLP v0.1 completo: framing COBS/longitud, cabecera
      de 8 B + CRC-16/CCITT, 17 tipos de mensaje, plano de control JSON vs plano
      de datos empaquetado, autonomía/store-and-forward (§5), reglas de versionado.
- [x] `protocol/espstation.protocol.yaml` — gemelo legible por máquina.
- [x] `protocol/experiment.schema.json` — JSON Schema del spec de experimentos.
- [x] `docs/ARCHITECTURE.md` — principio rector, componentes, flujo, seguridad.
- [x] `docs/EXPERIMENTS.md` — spec declarativo, ciclo de vida, 3 puertas de validación.
- [x] `docs/ROADMAP.md` — S0..S10 con *definition of done* verificable por sprint.
- [x] `docs/DECISIONS.md` — D-1..D-15 con razón y consecuencia.
- [x] `docs/SETUP.md` — incluye el problema de `dialout` y el toolchain.
- [x] `AGENTS.md` (+ `CLAUDE.md` → symlink) y `CONTRIBUTING.md`.
- [x] `.claude/agents/` — orchestrator, builder, reviewer, firmware-specialist.
- [x] `.codex/agents/` — **generado** por `tools/sync_agents.py` (gate en CI).
- [x] `.claude/skills/espstation/SKILL.md` (+ espejo en `.agents/skills/`).
- [x] `tools/check_protocol.py` — gate de deriva del protocolo.
- [x] `tools/sync_agents.py` — roles Claude → Codex, con `--check`.
- [x] `.github/workflows/ci.yml` — contracts · firmware host · firmware build ·
      gateway (3.11/3.12) · desktop.
- [x] README, LICENSE (MIT), .gitignore.
- [x] Toolchain PlatformIO + ESP-IDF instalado localmente en `.venv-tools/`.

## Entregado por los builders

- [x] **firmware/** (builder Sonnet) — `esps_proto` puro C11 + tests de host,
      `esps_core`, `esps_link` (UART), `main.c`. Host verificado con ASan/UBSan
      y target `esp32dev` compilado con PlatformIO/ESP-IDF 5.5.
- [x] **gateway/** (builder Sonnet) — códec, transports (serial/tcp/sim),
      store SQLite, API FastAPI, `docs/API.md`, 37 tests. Contrato REST/WS
      verificado contra los tipos del desktop.
- [x] **desktop/** (builder Sonnet) — Electron+React, design system, secciones
      Nodes y Live, 63 tests, typecheck y build. Árbol npm sin vulnerabilidades.

## Definition of done de S0

- [x] `make -C firmware/test/host test` verde (ASan/UBSan; LeakSanitizer
      desactivado porque el códec tiene prohibido asignar memoria)
- [x] `gateway/.venv/bin/python -m pytest tests/ -q` verde — 37 passed
- [x] `cd desktop && npm run typecheck && npm test && npm run build` verde —
      63 passed
- [x] `pio run -d firmware -e esp32dev` compila — verificado con Python 3.13;
      14,848 B RAM (4.5%), 230,711 B flash de aplicación (22.0%)
- [x] `python -m espstation_gateway --sim` sirve `/api/nodes` con 3 nodos
      simulados (verificado por HTTP con token)
- [x] `tools/check_protocol.py` verde — 55 checks
- [ ] Revisión adversarial independiente (reviewer Opus). La auditoría del
      orquestador ya corrigió drift del gate, contrato REST/WS vs desktop,
      timestamps iniciales, precisión NDB y dependencias vulnerables
- [ ] Repo en GitHub con CI en verde

## Estrategia operativa

Ciclo por módulo: **spec → builder → el orquestador verifica contra la realidad
→ reviewer → arreglo → commit atómico**. El paso que se salta siempre es
"verifica contra la realidad": correr la cosa, no sólo los tests.

Commit y push atómicos por módulo. En sesiones autónomas largas los agentes se
caen por fallos de API o por límite de gasto; el commit frecuente es la red de
seguridad, no la excepción.

## Conocido y pendiente

- **El usuario no está en el grupo `dialout`** → `/dev/ttyUSB0` no se puede
  abrir. Requiere `sudo usermod -aG dialout $USER` y **cerrar sesión**. Bloquea
  la validación en hardware (S1), no S0.
- Hay un **ESP32 WROOM (CP2102) conectado** en `/dev/ttyUSB0`, sin flashear.
- Entornos `esp32s3`/`esp32c3`/`esp32c6` declarados pero **sin hardware que los
  valide** (D-13). No presentarlos como soportados.
- El `.venv-tools` original usa Python 3.14, incompatible con el conjunto de
  dependencias de ESP-IDF 5.5. El build quedó verificado con un entorno aislado
  Python 3.13; recrear `.venv-tools` con Python 3.11–3.13 para que `make fw-build`
  funcione directamente (ver `docs/SETUP.md`).
- El `uint32` de ms del nodo da la vuelta a los ~49.7 días; el manejo del wrap
  en el gateway está especificado (D-10) pero **no implementado**.

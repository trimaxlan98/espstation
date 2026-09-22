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
      store SQLite, API FastAPI, `docs/API.md`, 38 tests. Contrato REST/WS
      verificado contra los tipos del desktop.
- [x] **desktop/** (builder Sonnet) — Electron+React, design system, secciones
      Nodes y Live, 64 tests, typecheck y build. Árbol npm sin vulnerabilidades.

## Definition of done de S0

- [x] `make -C firmware/test/host test` verde (ASan/UBSan; LeakSanitizer
      desactivado porque el códec tiene prohibido asignar memoria)
- [x] `gateway/.venv/bin/python -m pytest tests/ -q` verde — 38 passed
- [x] `cd desktop && npm run typecheck && npm test && npm run build` verde —
      64 passed
- [x] `pio run -d firmware -e esp32dev` compila — verificado con Python 3.13;
      14,848 B RAM (4.5%), 230,711 B flash de aplicación (22.0%)
- [x] `python -m espstation_gateway --sim` sirve `/api/nodes` con 3 nodos
      simulados (verificado por HTTP con token)
- [x] `tools/check_protocol.py` verde — 55 checks
- [ ] Revisión adversarial independiente (reviewer Opus). La auditoría del
      orquestador ya corrigió drift del gate, contrato REST/WS vs desktop,
      timestamps iniciales, precisión NDB y dependencias vulnerables
- [x] Repo en GitHub con CI en verde — run `33944882088` sobre `0e5e454`

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

---

# Slice fuera de banda — enlace digital entre ESP32

**ESTADO: implementado y verificado SIN hardware; en placa sólo N1 (2026-09-21).**
Registrado en `docs/ROADMAP.md` como slice fuera de banda (no es S1). Decisiones:
D-16 … D-21. Contrato del enlace: `bench/practicas/enlace-digital/SPEC-LINK.md`.
Prompt para verificar en hardware: `docs/plans/PRACTICA-enlace-digital.hardware-verification.prompt.md`.
**Commiteado en la rama `feat/enlace-digital`** (3 commits: Fase 0 `bench/`, Fase 1
código + protocolo + gates, documentación), **sin push ni merge a `main`**.

## Hecho y verificado (salida real, esta máquina)

- [x] Fase 0: 4 sketches Arduino (`n1_nivel/{emisor,receptor}`, `n2_handshake`,
      `n3_bytes`) **compilan** con `arduino-cli` 1.5.1 + core `esp32:esp32` 3.3.11
      (variantes `ROL_B` incluidas; los `#error` de rol disparan).
- [x] `esps_dio` parte pura (CRC-8, trama, receptor bit a bit, BER, generador,
      pines, política) — `make -C firmware/test/host test` verde; barrido de
      bit-flips: 13 `FRAME_OK` espurios en 8000 volteos de LEN, 0 en 139 976 de
      payload/CRC.
- [x] `esps_dio` capa ESP-IDF + `main.c` (HELLO troceado): `esp32dev`,
      `esp32dev_dio_a`, `esp32dev_dio_b` **compilan** (RAM 14 848 / 15 024 /
      15 992 B; con `ROLE=0` RAM idéntica a S0 y ningún símbolo de `esps_dio`).
- [x] Simulador: cable virtual, `set_gpio` real, `--sim-dio`; `make gateway-run`
      muestra dos nodos hablando (RTT medio 80 µs con 40 µs por sentido, BER
      ~3–6e-4). `pytest gateway/tests` → 160 passed.
- [x] `make check` en verde (contracts, fw-test, gateway 160, desktop 64).
      `tools/check_protocol.py` → 55 checks. Protocolo: sólo una frase aclaratoria
      en §4.1 (D-20), sin cambio de trama ni de campos.
- [x] **Desktop sin cambios**: verificado con la UI real (renderer servido +
      Chromium headless) — los 6 canales aparecen como chips, `DIO RX` sigue a
      `DIO TX`, el rail muestra `dio.edge`/`link.crc_err`/`link.lost`/`link.frame_ok`.

## NO hecho / NO verificado

- [x] **N1 en hardware (2026-09-21, dos ESP32 reales, A=`ttyUSB0` emisor, B=`ttyUSB1`
      receptor):** 300 s con **0 `ANOMALIA`** (120 cambios/min, 1 Hz); con el cable de
      señal suelto y `INPUT_PULLDOWN`, nivel 0 estable ≈66,9 s; contraste con `INPUT`
      sin pull: ≈120 cambios/s (hipótesis: red a 60 Hz, sin medir). Cifras y
      limitaciones en `README.md` `## Resultados N1`; logs en
      `bench/practicas/enlace-digital/evidencia/` (**ignorados por `.gitignore:36`
      `*.log`**). **Límites:** la desconexión se hizo con la línea en 0 (la caída 1→0
      sólo la muestra el contraste); longitud del cable no medida; **hallazgo abierto:**
      bloques de 64 bytes `0xFF` y ráfagas `80 00 00` en la captura serie de ambas
      placas (causa no encontrada, pisan líneas de datos).
- [ ] **Sin hacer en hardware**: N2 (RTT 1000 intercambios), N3 (BER 1000 tramas +
      límite de velocidad) y el firmware `esps_dio` en placa. Las secciones
      `Resultados N2/N3` del README dicen PENDIENTE. **Ninguna cifra de latencia,
      BER o límite de velocidad existe aún.**
- [x] **Correcciones a la revisión (2026-09-20), verificadas por el orquestador:**
      **A1, A2, A3, M1, M2, B1, B2 corregidos.** `make check` verde (275 gateway,
      64 desktop, host firmware); `esp32dev`/`dio_a`/`dio_b` compilan;
      `ROLE=0` sin símbolos de `esps_dio`; B1/B2 comprobados en vivo (400 con
      `null`, `1e400`, `NaN`); eventos del simulador capturados por WS con las
      claves exactas de la tabla del SPEC. Cambios: pila 3072→**6144 B** (medida
      con objdump: ~3760 B + interrupción + ISR; **es análisis estático, no
      medición en placa**; +3 KB de heap, no aparece en el RAM de `pio`);
      `esps_dio_start()` ahora **antes** del bucle de apertura del UART (D-1);
      el nodo reanuncia hasta recibir tantos ACK como trozos de HELLO; el modo
      manual ya no libera 14/25; eventos alineados firmware↔sim (tabla en
      SPEC-LINK); log de muestras de canal desconocido en el gateway.
      **Notas honestas:** (a) A2 supone que el gateway responde un ACK por HELLO
      (lo hace, `runtime.py`); no se probó contra un nodo real. (b) Hay una
      carrera menor teórica: `hello_task` reinicia `g_hello_acks` mientras
      `on_frame` puede incrementarlo; sólo se manifestaría con un ACK tardío de la
      ronda anterior. (c) A3 hace que el log de `esps_dio` salga por UART0 antes
      de instalar el hook de logs: unas líneas sin trama al arrancar, que el
      gateway ya muestra como salida cruda. (d) Se **eliminó** el evento
      `dio.gpio_rejected` por pin mal configurado al arrancar (ya no podía
      entregarse, sin sink; sigue el `ESP_LOGE`). (e) `link.crc_err` con
      `reason:"len"` lleva `len: 0` siempre (contrato ajustado, el receptor
      descarta el byte).
- [x] **Segunda tanda de correcciones (M3, M4, B3–B10), 2026-09-20, verificadas
      por el orquestador:** `make check` verde en 29 s (55 contratos, host de
      firmware, **`bench-test` 91 comprobaciones**, 285 gateway, 64 desktop); los
      tres builds compilan; `ROLE=0` sin símbolos de `esps_dio`; el sketch N3
      compila con `arduino-cli`. **M3:** el sketch N3 ahora tiene test en el repo
      (`make bench-test`, en CI): compila el `.ino` real contra un mock de Arduino,
      vectores dorados, barrido 13/0, enlace TX→RX y 11 escenarios contra
      `LinkMonitor`; 11/11 mutaciones detectadas, `.ino` intacto (md5). **M4:**
      `PROTOCOL.md` §4.1 y D-20 dicen ahora que es un cambio de *comportamiento*
      de la estación, no sólo una aclaración. **B3:** en vez de poner a cero se
      escriben los 34 bytes (con `_Static_assert` de que no hay relleno); verificado
      en el desensamblado que ningún callee de las ISR está en flash (memcpy es
      ROM). **B4:** lectura volátil en el punto de uso, sin tocar la estructura
      pura. **B5:** comentarios corregidos (dos sitios). **B6:** `_Static_assert`
      de que 3+6 canales caben, más log si alguna vez se trunca. **B7:** N3 escribe
      el latch antes de `pinMode`. **B8–B10:** cola acotada sin bloquear,
      `enable_dio` tras `start` lanza `RuntimeError`, HELLOs del mismo nodo y
      enlace a <5 s comparten `session` (un ACK y un ancla de time-sync por trozo).
      **Límites:** el test del sketch prueba lógica, no temporización; N1 y N2 siguen
      sin test en el repo; el paso de CI (2 pythons) sólo se validó como YAML, no se
      ejecutó en un runner de GitHub.
- [x] **Revisión adversarial (reviewer Opus) completada el 2026-09-20** (segundo
      intento; el primero murió por límite de API). 0 críticos, **3 altos, 4
      medios, 10 bajos.** *Todos corregidos (ver el bloque siguiente).*
      Hallazgos originales: Los tres altos, con su
      verificación por el orquestador:
      - **A1 — pila de la tarea `esps_dio` (3072 B) insuficiente.** El reviewer
        midió con `objdump` sobre el ELF ~2496 B en el camino evento→JSON→ENLP
        (`esps_dio.c:115`, comentario `:962-967` describe otro camino, ~70 B).
        Overflow con canary = panic en la primera sesión de banco. *Constante y
        comentario verificados; la suma de frames es del reviewer, no recalculada.*
        Arreglo: 5120–6144 B o publicar eventos desde otra tarea.
      - **A2 — `g_hello_acked` con el primer `HELLO_ACK`.** El gateway responde un
        ACK por cada trozo; si el trozo 2 se pierde nunca se reenvía y el gateway
        descarta sus muestras en silencio (`main.c:734`, `runtime.py:225-256`).
        *Verificado.* Arreglo: exigir ACK posterior al último trozo, o reanunciar.
      - **A3 — D-1 roto si el UART no abre.** `esps_dio_start()` (`main.c:806`)
        va después del bucle infinito de apertura del enlace (`main.c:785`).
        *Verificado.* Arreglo: arrancar `esps_dio` antes del bucle.
      Medios: M1 el modo manual permite `set_gpio` en 14/25 (las entradas del enlace)
      y reintroduce contención; M2 deriva firmware↔sim en eventos de `len_err`;
      M3 el sketch N3 es la única implementación sin test en el repo (verificado
      a mano por el reviewer: hoy sin deriva); M4 la frase de §4.1 impone a la
      estación "nunca borrar canales", que es un requisito normativo y no sólo
      una aclaración. Bajos: B1 `POST /api/sim/fault` con `loss:null` → 500; B2
      `WireConfig` acepta `inf`/`nan`; B3–B10 (cola ISR sin inicializar, `state` sin
      `volatile`, comentario de afinidad falso, `collect_ndb` trunca sin log, orden
      `pinMode`/latch en N3, `put_nowait`, `enable_dio` tras `start`, N sesiones por
      HELLO troceado). **Sin deriva** entre las tres implementaciones del enlace
      (contra-pruebas independientes del reviewer; barrido 13/0 reproducido).
      *No pudo auditar:* nada de lo que se decide en placa; leyó por encima los
      tests del simulador.
- [ ] El primer flasheo de `esps_dio` es también la primera ejecución real del
      firmware base (S1 sigue sin validarse). `tools/enlp_sniff.py` sigue sin existir.
- [ ] `set_gpio` **no es alcanzable desde la estación en hardware real** hasta S3
      (no hay runtime de experimentos; añadir un CMD sería cambio de protocolo).
      Sí funciona en el simulador vía trigger sobre pin libre.
- [ ] Hipótesis de firmware sin medir (ver D-21): IRAM evita errores durante
      escrituras NVS, 10 kbit/s sostenido, ISR de RX al ritmo, stack de 6144 B,
      GPIO14 con PWM al arrancar (conocimiento general, no medido); y el stall de
      hasta ~370 ms que la espera activa (prio 11, core 1) puede causar a las
      tareas UART si el planificador las coloca en el mismo core: el RX del
      driver (2048 B) desbordaría con tráfico continuo de la estación durante
      una ráfaga. Hoy la estación sólo manda CMDs ocasionales.
- [ ] Extensión de bus con direccionamiento (4 placas): no implementada.
- [x] ~~`len_err` sin evento en el firmware~~ — resuelto (M2): ambos emiten
      `link.crc_err` con `reason:"len"`, misma tabla de eventos y mismos límites.
- [ ] Debilidad medida del formato: CRC-8 no protege `LEN` (0,16 % de falsos OK).
- [ ] Límites de presentación de Live (no del NDB): un solo eje Y y máx. 4 canales
      → mezclar `DIO TX` (0/1) con `Link RTT` (~80) aplasta el TX. Un panel de
      analizador lógico sería trabajo aparte.
- [ ] Bug de desktop **observado, no investigado**: en Live el trazo del gráfico
      se corta a los pocos segundos aunque la API sigue entregando muestras (la
      última muestra tenía 0,1 s). **Reproducido también en `sim-1001` (`adc.a0`)**,
      un nodo que no toca este slice, así que no lo introdujo. No se comparó
      contra el commit de S0 ni se buscó la causa; el eje de tiempo también
      superpone etiquetas a la derecha.
- [ ] El estado de `.venv-tools` cambió: ahora Python 3.13 (uv); el de 3.14 quedó
      en el scratchpad de la sesión. `desktop/build/icons/*` siguen borrados en el
      working tree desde antes (no son de este trabajo).

---

# Slice fuera de banda — clave Morse táctil entre dos ESP32

**ESTADO: implementado y verificado en hardware (2026-09-21) con un operador, con resultado PARCIAL y NO ROBUSTO: 2 de 3 `SOS` en la tanda 3, pero 0 de 3 en la tanda 4 con los mismos umbrales.**
Práctica independiente de `enlace-digital` (no toca `n1_nivel/`, `n2_handshake/` ni
`n3_bytes/`). Contrato: `bench/practicas/clave-morse/SPEC-MORSE.md`. **Sin commitear.**
Convención fija: **A = transmisora / llave (`ttyUSB0`), B = receptora / decodificadora
(`ttyUSB1`)**. Reutiliza el cable A.GPIO26→B.GPIO25 de N1; añade sólo `KEY_PIN` = GPIO13
en A. Subirla **sustituye el sketch de N1** en las dos placas.

## Hecho y verificado (salida real, esta máquina)

- [x] `transmisor` (273 256 B) y `receptor` (277 240 B) **compilan** con `arduino-cli`
      1.5.1 + core `esp32:esp32` 3.3.11 y `--warnings all`, sin errores ni advertencias.
- [x] Lógica en el host: `python3 bench/practicas/clave-morse/tests/run_tests.py` →
      receptor 18/18, transmisor 7/7, tabla A–Z/0–9 (36 entradas) 0 fallos contra un
      diccionario Morse independiente. Compila los `.ino` reales con ASan/UBSan sobre
      un mock del core. **Comprobado con 3 mutaciones** (quitar la bandera de
      `[palabra]`, romper la entrada `S`, quitar el relleno a 8 bits): las tres
      hacen fallar el test. No está enchufado a `make check`.

- [x] **En hardware (2026-09-21, dos ESP32 reales, A=`ttyUSB0` llave, B=`ttyUSB1` receptor,
      llave sin 330 Ω):** el enlace A→B no perdió flancos (`aceptados` de A = `flancos_crudos`
      de B en las tandas 1, 2 y 4) y B decodifica. **4 tandas de 3 `SOS`:** 0/3 con los umbrales de
      arranque (300/700/1800/15 ms), 0/3 tras cambiar `p` a 140, **2/3 (letras) tras calibrar
      con el operador a `p`=170, `l`=930, `w`=3000, `d`(A)=25 ms** y **0/3 en la tanda 4 con esos
      mismos umbrales congelados** (2 de 6 en total con umbrales congelados). Fallos: pausa entre rayas
      > `l` (1037 y 1001 ms), hueco entre SOS < `w` (2467 ms) y 2 errores de tecleo del operador
      (a un SOS le falta la primera S; otro salió `O O U`).
      **Antirrebote:** con `d`=15 un toque dio 2 pulsos (hueco de 19 ms; 27 toques → 28
      pulsos); con `d`=25, 27 toques → 27 pulsos (una sola tanda). Cifras, límites y
      logs en `bench/practicas/clave-morse/README.md` `## Resultados` y `evidencia/`
      (**ignorados por `.gitignore:36` `*.log`**).

## NO hecho / NO verificado

- [ ] **No es robusto:** un solo operador y 4 tandas; el ritmo del operador cambió ~2× entre
      tandas y los márgenes de `letra_ms` son de ~80 ms. **2/3 no es una tasa de acierto.**
      Los umbrales finales se aplicaron por serie y **no están en el firmware** (tras un
      reset vuelven a 300/700/1800/15).
- [ ] **Tandas 1 y 4: toques no contados** (el operador no sabe cuántos dio): su prueba de
      rebote es indeterminable. **Anomalía sin explicar** entre las tandas 3 y 4: A +8 flancos aceptados y B +2 sin captura. **`d`=25 validado en una sola tanda.**
- [ ] **Sin medir:** latencia de la ISR, longitud del cable, cualquier cosa con
      instrumento. **Hallazgo abierto:** en la tanda 3, B contó 55 activaciones de ISR
      frente a 54 flancos aceptados por A (`niveles_repetidos=1`); causa no determinada.
      **Hallazgo abierto (mismo que N1):** bloques de 64 bytes `0xFF` en la captura serie; en la tanda 4 se añadió una ráfaga
      de ≈781 KB en ≈3 s (≈260 KB/s, imposible por una UART a 115200) al abrir el puerto de A. Hipótesis: driver/capa USB del PC
      (64 B = paquete USB full-speed); **no comprobada**.
- [ ] Extensiones mías sobre el prompt, a revisar: comandos `d<ms>` (A y B), `v`
      (verbose) y `c` (contadores a cero); en el formato, `[letra: X] [bin: …]` (el
      `SOS` del ejemplo del prompt lo interpreté como nombre del caso de prueba, no
      como campo) y `[palabra]` como separador.

## Entrega para el evaluador (2026-09-21)

- [x] `INFORME.md` / `INFORME.pdf` (circuito, justificación de cada cable/resistencia/pin, **énfasis en el GND común**,
      pasos para replicar, resultados y límites), `docs/circuito.svg|png`, `visor/index.html` (**interfaz gráfica autónoma**:
      traducción a puntos/rayas/binario, teclear en vivo, las 4 tandas con la salida real de la placa, circuito y GND).
- [x] `tests/replay_evidencia.py`: el `receptor.ino` compilado en el PC reproduce **exactamente** los 163 eventos que imprimió la placa
      real en las 4 tandas, y el port JS del visor coincide (400 secuencias aleatorias con valores en los límites: 0 diferencias;
      2 mutaciones hacen fallar la prueba). **No prueba la temporización real del microcontrolador.**
- [x] **Pestaña «En vivo» del visor + `herramientas/puente_serie.py`** (2026-09-21): muestra el circuito REAL en tiempo real (llave de A, señal, símbolo
      y letra+byte que imprime B, umbrales/contadores, consolas). Probado en el banco: conecta limpio y A aceptó 128 flancos = B contó 128.
      `tests/test_puente.py` (24 pruebas): en demo, lo que llega por SSE == lo que imprimió la placa real (39/36 eventos) y los flancos de A
      == contadores (54/48). **Hallazgo:** al abrir un puerto ya usado llega una ráfaga de líneas viejas repetidas (≈40 `[palabra]` falsos);
      el puente vacía hasta silencio + guarda de tasa. Causa **no comprobada** (driver/USB del PC).
      **No verificado:** el visor en Firefox con las placas reales (solo Chromium headless con la demo); la latencia A→B (no se mide).
- [ ] **El visor no está integrado en la app de escritorio** a propósito: estas placas no hablan ENLP, así que la app no recibe sus datos.
- [ ] **Sin commitear**, y el `.zip` de entrega no está versionado. `.gitignore:36` (`*.log`) ignora la evidencia.

---

# Slice Morse dúplex en la app (2026-09-22)

Continúa la práctica `bench/practicas/morse-duplex/`, que hasta ahora vivía sólo
en el banco. Guía completa: [`docs/PRACTICA-MORSE.md`](docs/PRACTICA-MORSE.md).
Decisión de diseño y sus consecuencias: [`docs/DECISIONS.md`](docs/DECISIONS.md) D-22.

## Hecho y verificado (salida real, esta máquina)

- [x] `gateway/.../transports/sim/morse_link.py` — contrato Morse en Python:
      tabla, decodificador, filtro de llave, generador y **canales NDB 22-29**.
- [x] `gateway/.../transports/morse_sketch.py` — adaptador `FrameDecoder`: el
      texto del sketch entra y salen **frames ENLP reales**, más
      `MorseLogReplayTransport` para reproducir capturas grabadas.
- [x] `attach_morse_sketch` / `attach_morse_replay` en el runtime, y los
      `kind: "morse"` y `kind: "morse-replay"` en `POST /api/links`.
- [x] Sondeo del adaptador: `r` cada 5 s y un único `v` si hace falta. Sin esto
      los canales quedaban vacíos, porque abrir el puerto reinicia la placa.
- [x] `desktop`: sección **Morse** (texto recibido, tiras de nivel, contadores,
      cruce de integridad) + entrada en la barra lateral.
- [x] Las prácticas Morse entran en `make check` (objetivo `bench-test`).
- [x] **Verificado contra las dos placas reales**: aparecen como nodos
      `online`, declaran los nueve canales y la telemetría fluye al teclear
      (`morse.symbols=9`, `pulse_ms`, `gap_ms`, `tx`/`rx`, `bounces`).

Salidas reales:

```
gateway:  321 passed                      (eran 285; +36)
desktop:  9 files, 72 tests passed        (eran 64; +8); typecheck y build limpios
bench:    enlace-digital 91 comprobaciones · clave-morse TODO OK ·
          morse-duplex TODO OK (35) · test_puente 24/0 · test_visor 27/0
protocolo: protocol in sync — 55 checks passed, 0 skipped
```

Dos fallos reales que cazaron las pruebas y el hardware, ya corregidos:
el HELLO de 1241 B no cabía en un frame (ahora va troceado, como el simulador),
y el transporte rechazaba **todo** envío, así que el `HELLO_ACK` de la estación
tumbaba el enlace y el nodo salía `offline` para siempre.

## NO hecho / NO verificado

- [ ] **Las tandas NO se han repetido con el firmware real.** Corre en las dos
      placas y publica sus canales, pero todas las medidas del informe se
      tomaron con el sketch Arduino. La coincidencia de ±1 ms entre extremos
      depende ahora de que la tarea de 1 ms no se retrase, y eso no está medido.
- [ ] **Nodo simulado Morse** dentro del simulador del gateway. La demo sin
      hardware existe por reproducción de capturas (camino real), pero no hay
      un par de nodos sintéticos tecleándose entre ellos.
- [ ] **Comandos desde la app** (`morse.*` en la tabla de comandos). Exige el
      cambio de protocolo atómico en los cuatro sitios. Hoy un `CMD` falla
      ruidosamente a propósito.
- [ ] **Etapa de dos ordenadores**: el procedimiento de medida de masas está
      escrito; nadie lo ha ejecutado.
- [ ] `make check` **entero** no se ha corrido en esta máquina: falta `make` y el
      toolchain de ESP-IDF, así que `fw-test` y `fw-build` no se han ejecutado.
      Sí se han corrido, uno a uno, los cinco comandos de `bench-test`, la suite
      del gateway, la del desktop y las dos puertas de `contracts`.
- [ ] La rama POSIX de `captura_serie.py` sigue sin reprobarse en Linux.

## Anadido despues: esps_morse en C11 (2026-09-22)

- [x] `firmware/components/esps_morse/` — mitad pura en C11: tabla Morse,
      maquina de estados de pulso/silencio y antirrebote de la llave. Sin
      asignacion, sin globales, sin ESP-IDF.
- [x] `firmware/test/host/test_morse_{table,decode,key}.c` — vectores dorados,
      incluidos los dos que ninguna sesion de banco puede alcanzar: el
      envolvimiento de `micros()` (~71,6 min) y el de `millis()` (~49,7 dias).
      Tambien los numeros reales de la tanda 2 (`EEE` por una `S`, y una `S` y
      una `O` fundidas), que es el caso que prueba que no hay `letra_ms` valido.
- [x] `firmware/test/host/run_tests.py` — la misma puerta sin necesitar `make`,
      para que se pueda correr en Windows. El Makefile sigue siendo la
      referencia y el runner avisa si las dos listas de ficheros se separan.
- [x] Ejecutado en esta maquina con LLVM-MinGW: `ALL TESTS PASSED`, con
      `-Werror` y ASan+UBSan.

Decision y consecuencias: `docs/DECISIONS.md` D-23.

**NO hecho:** la mitad ESP-IDF (`esps_morse.c`), que es GPIO, ISR de flanco,
tarea y publicacion de canales. Y `make -C firmware/test/host test` no se ha
podido ejecutar aqui por no haber `make`; lo verificado es el runner de Python,
que compila los mismos ficheros con las mismas banderas.

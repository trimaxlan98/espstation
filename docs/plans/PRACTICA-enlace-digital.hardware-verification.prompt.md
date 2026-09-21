# Prompt — verificación en hardware del enlace digital (sesión con acceso a los puertos)

> Pégalo completo como primer mensaje de Claude Code, dentro de
> `~/Documentos/github/espstation`, **en la sesión que ya tiene el grupo
> `dialout` activo**. Rol: **orchestrator**, pero en esta sesión **no se hace
> commit** salvo que el usuario lo pida al final.

---

## 0. Qué es esto y qué NO es

Otra sesión escribió los sketches Arduino de la Fase 0 (`bench/practicas/enlace-digital/`)
y la parte sin hardware de la Fase 1. **Ninguno está verificado en hardware.**
Tu trabajo aquí es ser quien lo verifica contra la realidad y traer **cifras
reales**. Lee primero `AGENTS.md`, `bench/practicas/enlace-digital/SPEC-LINK.md`
(el contrato), `bench/practicas/enlace-digital/README.md` y `SPRINT_STATUS.md`.

Reglas de oro, sin excepción:

1. **No inventes ni redondees mediciones.** Cada cifra que escribas en un
   README viene de una salida de serie que pegaste en tu reporte. Lo que no
   corriste se queda marcado **PENDIENTE / NO VERIFICADO**.
2. **Un fallo es un resultado.** Si algo no funciona, repórtalo con la salida
   real y diagnostica; no lo maquilles ni cambies el criterio para que pase.
3. **No modifiques los sketches para que "salga bien".** Si hay un bug real,
   arréglalo con el cambio mínimo, dilo explícitamente en el reporte (qué,
   por qué, cómo lo comprobaste) y vuelve a correr la prueba completa.
4. **No uses GPIO12. No sustituyas el cable por WiFi/BLE/ESP-NOW.** Nunca unas
   dos salidas entre sí.
5. Antes de cablear o cambiar cableado, dime qué vas a pedirme que mueva: el
   cableado físico lo hago yo. Pídeme cada configuración de cables una vez, con
   un diagrama ASCII corto, y espera mi confirmación.

## 1. Precondiciones (verifícalas, no las supongas)

```bash
id                       # debe listar "dialout"; si no, PARA y dímelo
ls -l /dev/ttyUSB*       # se esperan dos: ttyUSB0 y ttyUSB1
~/.local/bin/arduino-cli version
~/.local/bin/arduino-cli core list          # debe listar esp32:esp32
~/.local/bin/arduino-cli board list         # ambos puertos (CP2102)
```

Si `id` no muestra `dialout` en esta sesión, no intentes rodearlo con `sudo`:
para y dime que hay que cerrar sesión y volver a entrar. Si el core `esp32:esp32`
no está instalado, instálalo (`arduino-cli core install esp32:esp32`, ~1 GB).

Las dos placas son CP2102 idénticas; no hay forma de saber cuál es cuál por el
USB. Convención: **A = `/dev/ttyUSB0`, B = `/dev/ttyUSB1`**. Pídeme que ponga
una etiqueta física (cinta) a cada una y confirma con `arduino-cli board list`
que las dos aparecen antes de seguir. Si al reconectar los números se
intercambian, dilo y reasigna.

Compilar todo primero (esto sí lo puedes hacer sin cablear nada):

```bash
cd bench/practicas/enlace-digital
for d in n1_nivel/emisor n1_nivel/receptor n2_handshake n3_bytes; do
  ~/.local/bin/arduino-cli compile --fqbn esp32:esp32:esp32 "$d" || echo "FALLO $d"
done
```

Pega la salida real. Para N2 y N3 hay que compilar **dos veces**, una con
`#define ROL_A` y otra con `ROL_B` (usa la mecánica que documente el README, o
`--build-property compiler.cpp.extra_flags=-DROL_A` si los sketches lo
permiten; si no, edita el `#define` y **revierte el cambio al terminar**).

## 2. Cómo capturar el serie (IMPORTANTE: abrir el puerto reinicia la placa)

Con CP2102 + auto-reset, **abrir el puerto pulsa EN**. Por eso: abre el
capturador **antes** de empezar cada prueba y no lo cierres hasta terminar; un
"reinicio inesperado" a mitad de prueba casi siempre es que reabriste el puerto.

Usa un capturador que escriba a archivo con marca de tiempo del host y que
puedas dejar corriendo en segundo plano para las dos placas a la vez, p. ej.:

```bash
cat > /tmp/serial_log.py <<'EOF'
import sys, time, serial
port, out = sys.argv[1], sys.argv[2]
with serial.Serial(port, 115200, timeout=0.2) as s, open(out, "wb", buffering=0) as f:
    t0 = time.time()
    while True:
        line = s.readline()
        if line:
            f.write(f"{time.time()-t0:9.3f} ".encode() + line)
EOF
gateway/.venv/bin/python /tmp/serial_log.py /dev/ttyUSB0 /tmp/A.log &   # en background
gateway/.venv/bin/python /tmp/serial_log.py /dev/ttyUSB1 /tmp/B.log &
```

(Comprueba que `pyserial` esté en `gateway/.venv`; si no, dilo.) Guarda los
logs completos de cada nivel en `bench/practicas/enlace-digital/evidencia/`
(crea la carpeta: `n1_receptor.log`, `n1_desconexion.log`, `n2_A.log`, …) y
cita en el README el nombre del archivo, no sólo el resumen.

Para subir un sketch cuando el capturador tiene el puerto abierto: para ese
capturador, sube, y vuelve a abrirlo.

## 3. Nivel 1 — espejo de estado (1 bit)

Cableado (te lo pido a mí): B→A `GPIO26 → GPIO25`, `GND ↔ GND`, LED en GPIO4
con 330 Ω en cada placa. Emisor en B, receptor en A.

1. Sube `n1_nivel/emisor` a B y `n1_nivel/receptor` a A. Abre los capturadores.
2. **Prueba de 5 minutos**: deja correr 300 s. Criterio: el LED de A sigue al
   de B sin fallos; en `A.log` **cero** líneas `ANOMALIA`. Pega el primer y el
   último resumen periódico y el conteo de `ANOMALIA` (`grep -c`).
3. **Prueba de desconexión (la demostración de por qué `INPUT` estaba mal)**:
   con todo corriendo, pídeme que **desconecte el jumper de señal** (dejando
   GND). Deja 30 s. Criterio: A queda en **0 estable** (gracias al pull-down),
   sin cambios espurios. Guarda el log.
4. **Contraste (evidencia)**: sube al receptor una variante temporal con
   `pinMode(RX_DATA, INPUT)` (sin pull) en una copia en el scratchpad, **no en
   el repo**, y repite 30 s con el cable de señal desconectado. Documenta lo
   que se ve realmente (ruido, cambios espurios, o quizá nada: si no se ve nada
   en tu placa/entorno, dilo — no es un fallo, es un dato). Vuelve a subir el
   sketch correcto.

## 4. Nivel 2 — handshake y latencia

Cableado cruzado: `A.26→B.25`, `B.26→A.25`, GND común, 330 Ω serie por línea.
Sube `n2_handshake` con `ROL_A` a A y `ROL_B` a B. Arranca primero B
(respondedor), luego A. Registra: las 1000 líneas `seq,rtt_us,timeouts`, las
estadísticas finales (n, min, máx, media, desv. est., timeouts, histograma).
Repite la corrida **3 veces** y reporta las tres (¿es estable la media entre
corridas?). Después **invierte los roles** (A responde, B inicia) una vez, para
detectar si una placa es sistemáticamente más lenta. Reporta cualquier outlier
grande y qué lo explica (o que no lo sabes).

## 5. Nivel 3 — trama síncrona de dos hilos

Cableado: `A.26→B.25` (datos), `A.27→B.14` (reloj), GND, 330 Ω en las líneas.
A transmisor, B receptor.

1. **Humo** a 1 kbit/s: 1000 tramas; el receptor debe reportar
   `frames_ok`, `frames_crc_err`, `frames_len_err`, `bits_rx`, `bit_errors`,
   `BER`, `frames_perdidas`. Si a 1 kbit/s no sale limpio (BER≈0, 1000 ok),
   **para y diagnostica** antes de barrer: cableado, GND, reloj, sincronía.
2. **Barrido de velocidad**: 1 k, 5 k, 10 k, 50 k, y luego sigue subiendo
   (100 k, 200 k, … lo que permita el sketch) **hasta que se degrade**.
   Por cada velocidad: 1000 tramas, tabla con las cifras. Repite el punto donde
   empieza a degradarse **3 veces** para ver que no es azar.
3. **El límite es un hallazgo.** Reporta la velocidad más alta con BER = 0 en
   1000 tramas, y a partir de cuál se degrada, con la causa probable (latencia
   de la ISR, tiempo de subida con cable largo, contención, etc.) marcada como
   **hipótesis** salvo que la hayas comprobado (p. ej. acortando el cable o
   quitando la resistencia serie y viendo si el límite se mueve).
4. Prueba extra de robustez: con la prueba corriendo a una velocidad que sí
   funciona, desconecta y reconecta el reloj un instante; el receptor debe
   perder tramas sin quedarse colgado, y recuperar el sincronismo. Reporta
   cuántas tramas se perdieron y si se recuperó solo.
5. Extensión con 4 placas (bus con direccionamiento): **sólo** si el sketch
   la implementa (`EXT_DIRECCION`) y todo lo anterior quedó sólido. Si no
   existe, no la inventes.

Comprobación de seguridad eléctrica (rápida, con las placas alimentadas por
USB): tras un reset con todo cableado, mide/observa que ninguna placa se
calienta y que ninguna línea queda en un nivel absurdo. Si tienes forma de ver
GPIO14 durante el arranque (osciloscopio/analizador del usuario), dilo; si no,
anota "no medido".

## 6. Documentar (sólo con datos reales)

Rellena en `bench/practicas/enlace-digital/README.md` las secciones
`## Resultados N1/N2/N3` (hoy marcadas **PENDIENTE DE MEDIR EN HARDWARE**):
tablas con las cifras, referencias a los archivos de `evidencia/`, fecha, placas
usadas (etiquetas A/B), longitud aproximada de los cables y si llevaban
resistencia serie. Quita la marca PENDIENTE **sólo** de lo que efectivamente
mediste. Añade a `SPRINT_STATUS.md` (sección de esta práctica) qué quedó
verificado en hardware y qué no.

## 7. Parte B — firmware EspStation (`esps_dio`) en hardware

Esta parte SÍ se puede hacer ya: el firmware compila y está en el working tree
(`esp32dev_dio_a` y `esp32dev_dio_b`; ver `docs/DECISIONS.md` D-21 y
`SPRINT_STATUS.md`). **Nada de esto ha corrido nunca en una placa**, y es además
la primera ejecución real del firmware base (S1 nunca se validó). Haz primero la
Parte A (Fase 0): si N1–N3 no funcionan con el cableado, esta parte no tiene
sentido.

Flashea una placa con cada rol (**dos placas con el mismo rol no dan enlace**:
dos B parecen inertes, dos A reportan `link.lost`):

```bash
make fw-flash FW_ENV=esp32dev_dio_a PORT=/dev/ttyUSB0
make fw-flash FW_ENV=esp32dev_dio_b PORT=/dev/ttyUSB1
gateway/.venv/bin/python -m espstation_gateway --serial /dev/ttyUSB0 --port 8787
```

(`tools/enlp_sniff.py` no existe; observa con el gateway y `curl -H 'Authorization: Bearer espstation-dev' localhost:8787/api/nodes/<id>`; para observar las dos placas a la vez, adjunta la segunda desde el desktop (Nodes → Links → Attach) o mira `gateway/docs/API.md` para el endpoint; no asumas uno.)
Cableado idéntico al de N3, con 330 Ω en cada línea.

Verifica, con salida real cada punto (y para cada uno lo que NO se pudo comprobar):

1. **Arranque y HELLO troceado**: el nodo aparece; su NDB tiene **9 canales**
   (3 `sys.*`: `heap_free`, `rssi`, `uptime`; más `dio.tx`, `dio.rx`,
   `link.rtt_us`, `link.frames_ok`, `link.frames_err`, `link.ber`; los 11 del
   simulador incluyen además `sys.vbat` y `sys.temp`); en el log del nodo salen
   los tamaños de cada trozo de HELLO (esperado: 2 trozos de ~799 y ~721 B) y
   una línea `HELLO round: 2 chunks sent, 2 acked` (si dice `— retrying` de forma
   persistente, el gateway no está enviando un ACK por trozo: repórtalo). Es
   la primera vez que el gateway real fusiona dos HELLO parciales de un nodo real:
   comprueba que no duplica el nodo ni pierde canales, y que `node.ping` recibe
   `CMD_ACK` en < 100 ms.
2. **`dio.rx` sigue a `dio.tx`** del otro nodo y **`link.rtt_us`** es del orden del
   RTT que mediste en N2 (no se ha predicho ninguna cifra: repórtala).
3. **Ráfagas N3 (10 kbit/s por defecto)**: espera `link.frames_ok` == 20 por
   ráfaga y BER 0 en el receptor. Después barre 1 k/5 k/10 k/50 k
   (`ESPS_DIO_BITRATE`) y reporta dónde se degrada: **es un hallazgo**.
4. **Desconectar el cable** (datos y reloj) a mitad de una ráfaga: el nodo NO se
   cuelga; `link.lost` aparece tras 3 timeouts; al reconectar, `link.up` y las
   ráfagas siguientes llegan sin comerse tramas (D-1: el nodo sigue con o sin
   enlace, y heartbeat/telemetría hacia la estación no se interrumpen).
5. **La decisión de IRAM (D-21) es una hipótesis**: con las ráfagas corriendo,
   emite repetidamente `node.set_label` (fuerza escrituras NVS con la caché
   apagada) y comprueba que `link.frames_err` NO sube. Si tienes tiempo,
   compara con un build sin `ESP_INTR_FLAG_IRAM`: esa comparación es la
   justificación de los ~750–920 B de IRAM.
6. **Stack (es la primera lectura que debes hacer)**: la tarea `esps_dio` tiene
   6144 B por análisis estático (cadena evento→JSON→ENLP medida con objdump en
   ~3760 B + 256 de interrupción + la ISR propia, con ~125 llamadas indirectas
   sin resolver): es una hipótesis. El nodo imprime en un LOG, justo tras emitir
   su primer evento, `phase task stack after first event: N B never used of
   6144 B allocated`, y otra vez a los ~15 s. Anota ambos números; si el primero
   es < ~1000 B, repórtalo como hallazgo. Un panic *"stack overflow in task
   esps_dio"* al arrancar sería exactamente el fallo que se quiso evitar.
7. **GPIO14 al arrancar**: si el usuario tiene osciloscopio o analizador lógico,
   mide GPIO14 durante un reset con todo cableado; si no, anota "no medido".
   Es una afirmación de referencias de pines del ESP32 que no está verificada.
8. **Tráfico continuo de la estación durante una ráfaga**: la espera activa del
   transmisor (prio 11, core 1) puede dejar sin CPU hasta ~370 ms por ráfaga a
   las tareas UART si el planificador las coloca en el core 1. Con la estación
   mandando `node.ping` en bucle rápido durante ráfagas de N3, comprueba que no
   se pierden CMD_ACK ni aparecen bytes crudos de más en el gateway, y si el
   firmware expone contadores de descartes del enlace, léelos. Es aritmética, no
   una medición.
9. **Reloj TX**: con osciloscopio/analizador, periodo mínimo y máximo de
   `TX_CLK` a lo largo de una ráfaga a 10 kbit/s. Si no hay instrumento, "no medido".
10. Nunca marques como verificado lo que dependa de un instrumento que no hay.

## 8. Reporte final

Archivos creados o modificados · comandos exactos y salida real · tablas de
N1/N2/N3 con cifras y referencia a su archivo de evidencia · el límite de
velocidad de N3 con su nivel de certeza · bugs encontrados en los sketches y
cómo se corrigieron · qué quedó **sin verificar** y por qué · qué necesita
saber la sesión de desarrollo.

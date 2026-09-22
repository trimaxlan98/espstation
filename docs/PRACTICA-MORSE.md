# Práctica Morse dúplex — guía completa

> Esta guía está en español porque todo el corpus de la práctica lo está
> (`bench/practicas/clave-morse/`, `bench/practicas/morse-duplex/`, sus SPEC,
> informes y evidencia). El resto de `docs/` está en inglés; el contrato de
> integración con la app se resume además en inglés en
> [`DECISIONS.md` D-22](DECISIONS.md).

Dos ESP32, dos llaves, dos operadores. Cada uno teclea Morse y ve decodificado
lo que teclea el otro. Esta guía cubre la práctica entera: desde el cable hasta
verla dentro de EspStation.

| Quiero… | Ve a |
|---|---|
| Montar el circuito y teclear | [1. El montaje](#1-el-montaje) |
| Verla en la app, con placas | [3. Monitorizar desde EspStation](#3-monitorizar-desde-espstation) |
| Verla **sin hardware** | [4. Sin hardware](#4-sin-hardware) |
| Entender qué se mide y qué no | [5. Qué se puede medir](#5-qué-se-puede-medir-y-qué-no) |
| Los números medidos en el banco | [6. Resultados](#6-resultados-medidos) |
| Qué falta | [8. Lo que no está hecho](#8-lo-que-no-está-hecho) |

Contratos: [`SPEC-DUPLEX.md`](../bench/practicas/morse-duplex/SPEC-DUPLEX.md) es
la ley de la práctica; [`PROTOCOL.md`](../protocol/PROTOCOL.md) lo es de lo que
viaja entre nodo y estación. **No se solapan**, y esa separación es el punto
principal de esta guía.

---

## 1. El montaje

Un solo sketch, `transceptor/transceptor.ino`, **idéntico en las dos placas**.
El cable va cruzado, un hilo por sentido, como un null-modem serie:

```
   P1                                        P2
   GPIO26 (TX_DATA) ----- 330R --------->    GPIO25 (RX_DATA)
   GPIO25 (RX_DATA) <---- 330R ----------    GPIO26 (TX_DATA)
   GND ---------------------------------     GND        <-- OBLIGATORIO

   en CADA placa:
   3V3 --- punta pelada ][ punta pelada --- GPIO13   (la llave)
   GPIO4  --- 330R --- LED --- GND   (testigo: lo que envío)
   GPIO16 --- 330R --- LED --- GND   (testigo: lo que recibo)
```

```bash
cd bench/practicas/morse-duplex
arduino-cli compile --fqbn esp32:esp32:esp32 transceptor
arduino-cli upload  --fqbn esp32:esp32:esp32 -p COM5 transceptor
arduino-cli upload  --fqbn esp32:esp32:esp32 -p COM7 transceptor
```

### ⚠ Si las dos placas van en ordenadores distintos

El hilo de GND une las masas de los **dos ordenadores**, y un cargador de
portátil de dos clavijas deja pasar del orden de la mitad de la tensión de red
hasta el chasis. Sobra para matar un puerto USB. **Mide con multímetro entre
las dos masas, en alterna y en continua, antes de unir nada**, y conecta GND
primero y quítalo el último. El procedimiento completo está en el
[README de la práctica](../bench/practicas/morse-duplex/README.md#-enlazar-dos-ordenadores-medir-antes-de-unir-las-masas).

Una protoboard con carril de masa común **no arregla** esto: es la conexión,
no un aislamiento.

---

## 2. Cómo encaja en EspStation

Aquí está la decisión que hay que entender, porque determina todo lo demás.

Las placas de esta práctica **no hablan ENLP**: imprimen texto plano. Había dos
formas de meterlas en la app y sólo una es correcta:

| | Qué implicaría |
|---|---|
| ❌ Enseñar al gateway un segundo protocolo | Dos códecs en la estación. Rompe [D-8] y multiplica los caminos que pueden derivar. |
| ✅ **Adaptarlas a ENLP en el borde** | Un decodificador convierte el texto en frames reales. Río abajo no hay ningún caso especial. |

`transports/morse_sketch.py` es lo segundo: se coloca en la ranura de
`FrameDecoder`, entra texto y salen **frames ENLP de verdad**, construidos con
el códec real y re-parseados en el sitio. Si alguno saliera malformado,
reventaría ahí y no llegaría al registro.

La visibilidad se consigue con **canales NDB 22-29**, dentro del rango
16-127 que `PROTOCOL.md` reserva a canales definidos por el nodo. Por eso
**integrar esta práctica no toca el protocolo** y `tools/check_protocol.py`
sigue verde sin cambiarlo.

```
   sketch Arduino          morse_sketch.py            todo lo demás
  ┌───────────────┐      ┌─────────────────┐      ┌──────────────────┐
  │ "RX ."        │      │ FrameDecoder    │      │ NodeRegistry     │
  │ "RX [letra: S]│ ───► │ texto → ENLP    │ ───► │ store · REST · WS│
  │ "# RX pulso_ms│      │ (códec REAL)    │      │ desktop          │
  └───────────────┘      └─────────────────┘      └──────────────────┘
```

### Los canales

| id | clave | tipo | qué es |
|---|---|---|---|
| 3 | `sys.uptime` | u32 | el único canal de sistema que el adaptador puede saber |
| 22 | `morse.tx` | u8 | nivel de mi llave |
| 23 | `morse.rx` | u8 | nivel que llega del otro |
| 24 | `morse.pulse_ms` | u32 | duración del último pulso recibido |
| 25 | `morse.gap_ms` | u32 | duración del último silencio |
| 26 | `morse.symbols` | u32 | puntos + rayas decodificados |
| 27 | `morse.letters` | u32 | letras decodificadas |
| 28 | `morse.unknown` | u32 | códigos que no están en la tabla |
| 29 | `morse.bounces` | u32 | rebotes del contacto que el filtro suprimió |

Y los eventos: `morse.symbol`, `morse.letter`, `morse.word`, `morse.unknown`,
`morse.filtered`, `morse.thresholds`, `morse.note`. Cada uno lleva `dir`, que
vale `RX` (la mano del otro) o `TX` (el eco de la propia).

### Implicaciones que hay que tener presentes

1. **No son nodos de EspStation.** El HELLO lo dice: `fw.build` es
   `arduino-sketch` y `caps` lleva `read_only`. No tienen runtime de
   experimentos, ni NVS, ni store-and-forward.
2. **La app no puede mandarles comandos.** Un `CMD` o un `EXP_SET` **falla
   ruidosamente** en vez de desaparecer en silencio, para que nadie crea que un
   comando llegó. Poder ajustar `p/l/w/d/k` desde la app exige una op `morse.*`
   en el protocolo, o sea el cambio atómico en los cuatro sitios.
3. **El adaptador sí escribe dos cosas.** Manda `r` (lectura pura de umbrales y
   contadores) cada 5 s, y **un único** `v` si la placa dice que el verbose está
   apagado. No es opcional: abrir el puerto reinicia la placa, lo que apaga el
   verbose y pone los contadores a cero, así que sin esto los ocho canales se
   quedarían vacíos para siempre. Es un driver configurando su fuente.
4. **Las marcas de tiempo son una reconstrucción.** El sketch sólo sella
   `# TX flanco` y `# resumen`; entre medias el adaptador extrapola con el reloj
   de la estación. No son el reloj de la placa y no deben leerse como tal.

---

## 3. Monitorizar desde EspStation

```bash
# 1. el gateway
cd gateway && .venv/bin/python -m espstation_gateway --port 8787

# 2. engancha cada placa (el token por defecto es el de desarrollo)
curl -s -X POST http://127.0.0.1:8787/api/links \
  -H "Authorization: Bearer espstation-dev" -H "Content-Type: application/json" \
  -d '{"kind":"morse","path":"COM5","label":"P1"}'
curl -s -X POST http://127.0.0.1:8787/api/links \
  -H "Authorization: Bearer espstation-dev" -H "Content-Type: application/json" \
  -d '{"kind":"morse","path":"COM7","label":"P2"}'

# 3. la app
cd ../desktop && npm run dev      # sección «Morse» en la barra lateral
```

En Linux el `path` es `/dev/ttyUSB0`, `/dev/ttyUSB1`.

La sección **Morse** muestra, por estación: el texto que **está recibiendo**
(decodificado por el otro extremo, que es lo único que prueba que hubo
comunicación), las tiras de nivel de su llave y de lo entrante, y los
contadores. Debajo, el **cruce de integridad**: si los dos sentidos no
transportaron el mismo número de pulsos, lo dice.

### Cómo leer lo que sale

| Lo que ves | Qué significa |
|---|---|
| Los pulsos cuadran, las letras no | **El cable está bien y un umbral está mal.** Es el caso más común. |
| `filtered` > 0 | Ruido o rebote pasando el filtro: sube `d` y `k`. |
| `desbordes_buffer` > 0 | El `loop()` no drenó el buffer de 64 flancos. |
| `unknown` sube | Letras que se funden o se parten: es `letra_ms`, no el enlace. |
| `bounces` muy alto | El contacto metal-metal chisporrotea; normal, el filtro lo absorbe. |

### Recomendaciones de operación

- **Congela los umbrales antes de evaluar.** Una tanda con un umbral cambiado a
  mitad no es una tanda, es una anécdota.
- **Cambia una variable por tanda.** La sesión del banco que cambió dos a la vez
  no pudo atribuir el resultado a ninguna.
- **Empieza por `p`,** que es el umbral fácil: en el banco la banda vacía entre
  el punto más largo y la raya más corta fue de 131 y 138 ms. Si esa banda baja
  de 40 ms, el umbral es frágil y hay que arreglar la mano, no el número.
- **`d=40` y `k=40`, no 15.** Con 15 se colaron un pulso espurio de 33 ms y un
  hueco de 16 ms. Con 40, `filtrados=0` en toda la tanda.
- **El umbral describe la mano del otro.** `punto_raya_ms` de *mi* placa ajusta
  el pulso de *mi compañero*. Al calibrar hay que decir siempre de quién es la
  mano y en qué placa está el número.

---

## 4. Sin hardware

Cualquier captura grabada se vuelve a pasar por el **mismo adaptador**, así que
la app no distingue una reproducción de una placa:

```bash
curl -s -X POST http://127.0.0.1:8787/api/links \
  -H "Authorization: Bearer espstation-dev" -H "Content-Type: application/json" \
  -d '{"kind":"morse-replay",
       "path":"bench/practicas/morse-duplex/evidencia/tanda_k40_l600_A.log",
       "speed":4,"label":"replay"}'
```

Y el visor propio de la práctica, fuera de la app, con los dos sentidos a la vez:

```bash
python bench/practicas/clave-morse/herramientas/puente_serie.py --duplex \
    --velocidad 4 \
    --reproducir-a bench/practicas/morse-duplex/evidencia/tanda_k40_l600_A.log \
    --reproducir-b bench/practicas/morse-duplex/evidencia/tanda_k40_l600_B.log
# http://127.0.0.1:8765/
```

---

## 5. Qué se puede medir y qué no

**Sí se puede**, comparando los dos logs con `verbose`: para el pulso n-ésimo,
el `# TX pulso_ms=` del que teclea contra el `# RX pulso_ms=` del que escucha.
La correspondencia es **por orden**, nunca por reloj, y los contadores avisan si
se perdió algún pulso.

**No se puede** medir la latencia absoluta de un sentido. Con las placas en
ordenadores distintos, los `micros()` de cada una son independientes, sin origen
común, y no hay camino de ida y vuelta que recorra un solo reloj. Cualquier
número de latencia que apareciera sin un montaje nuevo sería inventado.

**Tampoco cubre la práctica**: detección de vida (un cable suelto da 0 estable,
igual que un compañero callado), colisiones (imposibles con un hilo por
sentido) ni corrección de errores.

---

## 6. Resultados medidos

Tres sesiones en el banco, un operador con las dos llaves. Evidencia completa en
[`morse-duplex/evidencia/`](../bench/practicas/morse-duplex/evidencia/).

**El enlace es transparente.** En las tres tandas, en los dos sentidos:

| | Pulsos enviados / recibidos | Error máximo |
|---|---|---|
| Tanda 1 (A→B) | 13 / 13 | 1 ms |
| Tanda 1 (B→A) | 15 / 15 | 1 ms |
| Tanda 2 (A→B) | 49 / 49 | 1 ms |
| Tanda 2 (B→A) | 30 / 30 | 1 ms |

Siempre con `filtrados=0`, `desbordes_buffer=0` y `niveles_repetidos=0`. El
error de ±1 ms es el truncado de microsegundos a milisegundos en cada extremo,
no una pérdida del cable.

**Con umbrales iguales en las dos placas, el eco predice al receptor carácter
por carácter.** Las dos cadenas salieron idénticas, sin una sola diferencia.

**El hallazgo principal: para `letra_ms` no existe ningún valor válido** con la
cadencia del operador probado. Lo demuestran dos errores de signo contrario en
la misma tanda: una `S` partida en tres `E` (huecos ≥600 ms *dentro* de una
letra) y una `S` y una `O` fundidas (hueco <600 ms *entre* dos letras). Las dos
poblaciones de huecos se solapan y ningún corte las separa.

La causa es la mano, no el número: los huecos dentro de la letra midieron de 233
a 570 ms cuando el punto mediano mide **123 ms**. El Morse estándar pide 1
unidad de hueco de elemento, 3 entre letras y 7 entre palabras — para 123 ms eso
es **123 / 369 / 861**. La corrección es **acortar la pausa entre los puntos de
una misma letra**; si baja a ~150 ms, `l≈300` y `w≈700` quedan con bandas
anchas.

---

## 7. Verificación

| Puerta | Qué cubre | Estado |
|---|---|---|
| `bench/practicas/morse-duplex/tests/run_tests.py` | el `.ino` real sobre un mock del core | 35 pruebas |
| `bench/practicas/morse-duplex/tests/test_visor.py` | puente `--duplex` y visor, reproduciendo evidencia real | 27 pruebas |
| `gateway/tests/test_morse_link.py` | vectores dorados; **re-decodifica la evidencia real** y exige las mismas letras que imprimió la placa | 20 pruebas |
| `gateway/tests/test_morse_sketch.py` | el adaptador; replica **un log entero** en frames válidos | 13 pruebas |
| `gateway/tests/test_morse_replay_link.py` | reproducción por REST | 3 pruebas |
| `desktop` | sección Morse | 8 pruebas |
| `firmware/test/host` | **`esps_morse` en C11**: tabla, decodificador y llave, con `-Werror` + ASan/UBSan | 3 suites |
| Hardware | las dos placas, en la app, con telemetría real | verificado |

Las tres primeras entran en `make check` (objetivo `bench-test`); las de
`firmware/test/host` entran por `fw-test`.

### Ejecutar el componente C11, en Linux y en Windows

`esps_morse` es la tercera implementación del enlace (las otras dos son el
sketch Arduino y el espejo en Python). Se compila con `-Werror` y
ASan+UBSan, y se ejecuta de dos formas equivalentes:

```bash
# Linux / macOS — la referencia, y lo que corre CI
make -C firmware/test/host test

# cualquier sistema, incluido Windows, sin necesitar make
python3 firmware/test/host/run_tests.py
```

Salida esperada, con las tres suites nuevas al final:

```
== morse_table ==
== morse_decode ==
== morse_key ==
ALL TESTS PASSED
```

En Windows hace falta un compilador C11 con sanitizers que **no sea** el clang
de `LLVM.LLVM` (ése apunta a MSVC y pide las cabeceras de Visual Studio):

```powershell
winget install MartinStorsjo.LLVM-MinGW.UCRT
$env:PATH = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW.UCRT_*\llvm-mingw-*\bin;$env:PATH"
python firmware\test\host\run_tests.py
```

Ese mismo `bin` lleva la DLL de ASan, así que con ponerlo en el `PATH` basta.

> **Si sale `OSError: [WinError 4551]`**, es **Smart App Control** de Windows
> bloqueando un ejecutable recién compilado; no es un fallo de los tests.
> Es intermitente: en esta máquina hizo falta reintentar hasta cuatro veces
> para una de las suites. Vuelve a lanzarlo, o desactiva Smart App Control si
> vas a compilar a menudo.

Dos de los vectores sólo pueden existir aquí: el envolvimiento de `micros()` a
los ~71,6 min y el de `millis()` a los ~49,7 días. Ninguna sesión de banco dura
lo suficiente para encontrarlos.

**La defensa contra la deriva** son los vectores dorados: la misma lógica existe
en el sketch Arduino, en `morse_link.py` y —cuando exista— en `esps_morse`. Si
cambias una, cambia las otras y el SPEC en el mismo commit.

---

## 8. Lo que no está hecho

Dicho explícitamente, porque es tan parte de la guía como lo demás:

- **`esps_morse` no tiene todavía mitad ESP-IDF.** La lógica pura en C11 (tabla,
  decodificador, antirrebote) **sí existe y pasa en el host**, pero falta lo que
  `esps_dio.c` es para el enlace digital: configurar los GPIO, la ISR de flanco,
  la tarea y la publicación de canales. Hasta que exista, las placas del banco
  siguen con el sketch Arduino y llegan a la estación por el adaptador.
- **No hay nodo simulado Morse** dentro del simulador del gateway. La demo sin
  hardware existe por reproducción de capturas, que usa el camino real, pero no
  es lo mismo que un par de nodos sintéticos tecleándose entre ellos.
- **La etapa de dos ordenadores no se ha ejecutado.** El procedimiento de masas
  está escrito; nadie lo ha hecho.
- **No hay comandos desde la app** (ver sección 2, implicación 2).
- **Un solo operador**, con las dos llaves. La pregunta de si dos manos distintas
  necesitan umbrales distintos sigue abierta.
- **Ninguna tanda se repitió** con los umbrales congelados, así que no hay tasa
  de acierto que dar.

[D-8]: DECISIONS.md

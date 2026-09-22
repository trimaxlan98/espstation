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
| Los `.ino` para el IDE de Arduino | [4bis. Llevarse los sketches](#4bis-llevarse-los-sketches-al-ide-de-arduino) |
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

Una placa con el **sketch Arduino** de la práctica no habla ENLP: imprime texto
plano. (Una placa con el firmware `esps_morse` sí lo habla, y entra en la app
como un nodo normal — ver la nota al final de la sección 3.) Para las del
sketch había dos formas de meterlas en la app y sólo una es correcta:

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

1. **Una placa con el sketch no es un nodo de EspStation.** El HELLO lo dice:
   `fw.build` es `arduino-sketch` y `caps` lleva `read_only`
   (`transports/morse_sketch.py:128`). No tiene runtime de experimentos, ni
   NVS, ni store-and-forward. Las tres implicaciones de abajo son del
   **adaptador**, no de la práctica: con el firmware `esps_morse` las tres
   desaparecen.
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

> **Esto es para placas con el sketch Arduino.** Una placa flasheada con
> `pio run -e esp32dev_morse` **habla ENLP**: se engancha con
> `{"kind":"serial","path":"COM5"}` como cualquier otro nodo, sin adaptador, y
> las tres limitaciones de la sección 2 no la afectan.

La sección **Morse** muestra, por estación: el texto que **está recibiendo**
(decodificado por el otro extremo, que es lo único que prueba que hubo
comunicación), **la señal en el cable** y los contadores. Debajo, el **cruce
de integridad**: si los dos sentidos no transportaron el mismo número de
pulsos, lo dice.

### La onda cuadrada

Es el dibujo del visor de `clave-morse` (`senalSVG`), ahora en vivo dentro de
la app: **1 mientras la llave está cerrada, 0 en reposo**, con dos carriles
sobre el mismo eje de tiempo — arriba **TX**, tu propia mano tal como te la
devuelve tu placa; abajo **RX**, lo que llega del otro operador. El borde
derecho es *ahora* y la onda se desliza por debajo. Cada pulso lleva su color
y su forma (círculo = punto, barra = raya), su duración en ms, y debajo el
corchete de la letra que cerró con su código.

> **De dónde salen los datos, y por qué no de `morse.tx`/`morse.rx`.** Esos
> dos canales llevan el nivel de la línea, que parece justo lo que hace falta
> y no lo es: `telemetry_task` publica **una muestra por segundo** y un punto
> dura ~100 ms. El propio comentario de la NDB en el firmware lo deja medido:
> *un SOS entero pasó sin que `morse.tx` se muestreara alto ni una sola vez*.
> La onda se reconstruye de los **eventos**: `morse.symbol` se dispara en el
> flanco que cerró el pulso y trae su duración, así que el pulso ocupó
> `[ts - ms, ts]`. Eso es exacto al milisegundo que midió la placa.
>
> **El adaptador del sketch también los manda.** El sketch imprime el símbolo
> y su duración en dos líneas seguidas, el símbolo primero, así que el
> adaptador retiene el símbolo —por sentido— hasta la línea siguiente de ese
> sentido. Si llega su duración, la lleva; si llega otra cosa, el evento sale
> **sin** `ms` antes que perderse o heredar el número de otro pulso. Hasta el
> 2026-09-22 no retenía nada y el `ms` se perdía siempre: con el sketch o con
> una reproducción, la onda no tenía nada que dibujar.
>
> **Lo que no puede enseñar:** un pulso sólo existe cuando **ha terminado**,
> porque hasta entonces la placa no sabe cuánto duró. Mientras mantienes la
> llave pulsada no hay nada que dibujar; lo que crece es el silencio, y ese
> número («your silence») sí está en vivo abajo a la derecha — es con el que
> se cronometran los huecos de letra y de palabra contra `l` y `w`.

Las tiras de nivel que había antes en su lugar se quitaron por lo mismo: a
1 Hz no pueden ver un punto, y una tira que casi siempre está vacía se lee
como «no llega nada».

### La cadencia

Debajo de la onda, por estación: **de dónde a dónde llegan tus puntos, de
dónde a dónde tus rayas, y cuánto vacío hay en medio**. Se mide del carril
**TX** — tu propia mano tal como la devolvió tu placa —, nunca del entrante,
que es la mano del otro. Todo sale de los pulsos que la placa ya reportó: no
hay que teclear ningún ajuste ni se lee nada del firmware.

| Lo que dice | Qué hacer |
|---|---|
| `Separation N ms — comfortable` | Nada. Da además el punto medio de la banda **para esa muestra**; no es un valor para copiar a otra sesión. |
| `Separation N ms — fragile` (N < 40) | Alarga las rayas. El banco fija 40 ms como mínimo cómodo: por debajo, un pulso cerca de la línea cambia de clase solo. |
| `Dots and dashes overlap` | Tu punto más largo no es más corto que tu raya más corta: **ningún** `punto_raya_ms` clasifica bien todos. Es mano, no número. |
| `Your gaps overlap too` | El silencio más largo dentro de una letra llega al más corto entre letras: **ningún** `letra_ms` los separa. |

El `wpm` es la estimación PARIS desde la **mediana del punto** (el punto es la
unidad, la palabra son 50 unidades → `1200 / punto_ms`). Es una estimación de
tu ritmo reciente, no una medida del enlace.

> **Por qué no se sugiere un `letra_ms`.** Porque la sesión de banco que lo
> intentó descubrió que las dos poblaciones de huecos **se solapan**, y cuando
> eso pasa no hay umbral que valga: sacar un número de un ritmo concreto es
> justamente cómo se escribe mal un hallazgo. Se ofrece punto medio sólo para
> `punto_raya_ms`, sólo cuando hay banda vacía de verdad, y siempre dicho
> como «en estos N símbolos».

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
la app no distingue una reproducción de una placa. El `path` lo abre el proceso
del gateway, así que una ruta relativa se resuelve desde **su** directorio de
trabajo, que siguiendo la sección 3 es `gateway/`:

```bash
curl -s -X POST http://127.0.0.1:8787/api/links \
  -H "Authorization: Bearer espstation-dev" -H "Content-Type: application/json" \
  -d '{"kind":"morse-replay",
       "path":"../bench/practicas/morse-duplex/evidencia/tanda_k40_l600_A.log",
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

## 4bis. Llevarse los sketches al IDE de Arduino

La app trae la sección **Sketches** en la barra lateral. No habla con el
gateway ni con ninguna placa: es la lista de los `.ino` de las prácticas, con
**Copy source** y **Save sketch folder…** para cada uno, y sirve estando la app
recién instalada, sin placas y sin red — que es exactamente la situación de
quien viene a buscar un sketch.

| Sketch | Va en | Fichero en el repo |
|---|---|---|
| Transceptor full duplex | **las dos placas**, binario idéntico | `bench/practicas/morse-duplex/transceptor/transceptor.ino` |
| Transmisor (placa A) | placa A, práctica de un sentido | `bench/practicas/clave-morse/transmisor/transmisor.ino` |
| Receptor (placa B) | placa B, práctica de un sentido | `bench/practicas/clave-morse/receptor/receptor.ino` |

**Librerías necesarias: ninguna.** Los tres sketches incluyen `<Arduino.h>` y
dos de ellos `"soc/gpio_struct.h"`, que es parte de ESP-IDF y por tanto del
core — no hay que abrir el Library Manager ni fijar ninguna versión. Lo que sí
hace falta:

- Core **`esp32` de Espressif** por Boards Manager
  (`https://espressif.github.io/arduino-esp32/package_esp32_index.json`).
  Compila con 2.x y con 3.x.
- Placa **ESP32 Dev Module** (o la que realmente sea la tuya).
- Monitor Serie a **115200**.
- **GND común** entre las dos placas y **330 Ω** en serie en cada línea de señal.
  **GPIO12 está prohibido**: un nivel alto en él al arrancar selecciona flash de
  1,8 V.

«Save sketch folder…» pide una **carpeta**, no un fichero, y crea dentro
`transceptor/transceptor.ino`: el IDE de Arduino sólo abre un sketch cuya
carpeta se llama igual que el fichero, y ese es el tropiezo típico justo en el
paso en que el usuario ya ha salido de la app.

Los ficheros **no están duplicados** en `desktop/`: se empaquetan en el bundle
desde `bench/` en tiempo de compilación (`?raw`), así que lo que reparte la app
es el mismo fichero con el que se flashean las placas del banco. Una placa con
el sketch necesita el adaptador `kind: "morse"` (sección 3); una con
`esp32dev_morse` no.

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

### 2026-09-22 — las dos placas con el firmware `esps_morse` y la app abierta

Primera tanda con **las dos placas llevando el mismo build post-auditoría** y
todo pasando por el gateway y la app. Un `SOS` por llave. Cruzando el evento
`morse.symbol` de la que **envía** contra el de la que **recibe**, pulso a
pulso:

| Sentido | Secuencia | Duraciones | Peor discrepancia |
|---|---|---|---|
| B → A | `... --- ...` idéntica | 127 100 99 · 312 346 343 · 123 112 101 ms | **0 ms** |
| A → B | `... --- ...` idéntica | 191 188 207 · 351 346 500 · 205 168 195 ms | **1 ms** |

Las dos placas miden la misma señal con **relojes distintos y filtros
independientes**, así que esa columna es el error del enlace entero. Las dos
decodificaron `SOS`, con `unknown=0` y `filtrados=0` en la tanda.

> **El cruce de integridad de la app dijo `Mismatch` en esta misma tanda, y
> tenía razón sobre lo que compara.** `morse.symbols` es acumulativo desde el
> arranque de **cada** placa y nada lo pone a cero: la B acababa de
> reflashearse (9) y la A llevaba ocho minutos oyendo tecleo anterior (21).
> Verdadero sobre los contadores y falso sobre la tanda. El panel lo advierte
> ahora; para que el cruce signifique algo, las dos placas tienen que haber
> arrancado a la vez.

**El enlace es transparente.** Las dos tandas con umbrales anotados, en los dos
sentidos (la tercera —la etapa 1, sin calibrar— está en el
[README de la práctica](../bench/practicas/morse-duplex/README.md)):

| Tanda | Evidencia | Pulsos enviados / recibidos | Error máximo |
|---|---|---|---|
| congelada (A→B) | `tanda_congelada_*.log` | 13 / 13 | 1 ms |
| congelada (B→A) | `tanda_congelada_*.log` | 15 / 15 | 1 ms |
| k40/l600 (A→B) | `tanda_k40_l600_*.log` | 49 / 49 | 1 ms |
| k40/l600 (B→A) | `tanda_k40_l600_*.log` | 30 / 30 | 1 ms |

Siempre con `filtrados=0`, `desbordes_buffer=0` y `niveles_repetidos=0`, en las
tres sesiones. El error de ±1 ms es el truncado de microsegundos a milisegundos
en cada extremo, no una pérdida del cable.

**Con umbrales iguales en las dos placas, el eco predice al receptor carácter
por carácter.** Las dos cadenas salieron idénticas, sin una sola diferencia.

**El hallazgo principal: para `letra_ms` no existe ningún valor válido** con la
cadencia del operador probado. Lo demuestran dos errores de signo contrario en
la misma tanda: una `S` partida en tres `E` (huecos ≥600 ms *dentro* de una
letra) y una `S` y una `O` fundidas (hueco <600 ms *entre* dos letras). Las dos
poblaciones de huecos se solapan y ningún corte las separa.

La causa es la mano, no el número: los huecos dentro de la letra midieron de 233
a **627 ms** —los dos más largos, 621 y 627, son precisamente los que partieron
la `S`— cuando el punto mediano de esa mano mide **123 ms**. El Morse estándar
pide 1 unidad de hueco de elemento, 3 entre letras y 7 entre palabras — para
123 ms eso es **123 / 369 / 861**. La corrección es **acortar la pausa entre los puntos de
una misma letra**; si baja a ~150 ms, `l≈300` y `w≈700` quedan con bandas
anchas.

---

## 7. Verificación

| Puerta | Qué cubre | Estado |
|---|---|---|
| `bench/practicas/morse-duplex/tests/run_tests.py` | el `.ino` real sobre un mock del core | 44 pruebas |
| `bench/practicas/morse-duplex/tests/test_visor.py` | puente `--duplex` y visor, reproduciendo evidencia real | 33 pruebas |
| `gateway/tests/test_morse_link.py` | vectores dorados; **re-decodifica la evidencia real** y exige las mismas letras que imprimió la placa | 26 pruebas |
| `gateway/tests/test_morse_sketch.py` | el adaptador; replica **un log entero** en frames válidos y exige que el símbolo lleve su duración | 24 pruebas |
| `gateway/tests/test_morse_replay_link.py` | reproducción por REST | 5 pruebas |
| `desktop` | sección Morse, la onda (`morseWave`), la **cadencia** (`morseCadence`), la sección Sketches y la escritura del `.ino` | 74 pruebas |
| `firmware/test/host` | **`esps_morse` en C11**: tabla, decodificador y llave, con `-Werror` + ASan/UBSan | 3 suites |
| `pio run -e esp32dev_morse` | el firmware completo, con la mitad ESP-IDF | compila y **corre en las dos placas** |
| Hardware | las dos placas, en la app, con telemetría real | verificado |

Todas entran en `make check`, pero por objetivos distintos: las **dos primeras**
por `bench-test`, las **tres del gateway** por `gateway-test`, la del desktop por
`desktop-test` y las de `firmware/test/host` por `fw-test`. `pio run -e
esp32dev_morse` **no** entra en ninguno: `.github/workflows/ci.yml` construye
sólo `esp32dev`, `esp32c3`, `esp32dev_dio_a` y `esp32dev_dio_b`, así que el
firmware Morse hay que compilarlo a mano.

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

**La defensa contra la deriva** son los vectores dorados. La misma lógica existe
hoy en las tres: el sketch Arduino, `morse_link.py` y `esps_morse` (las dos
mitades). Conviene saber exactamente cuánto cubren:

- El vector `SOS` es **idéntico** en `firmware/test/host/test_morse_decode.c` y
  en `gateway/tests/test_morse_link.py`: mismos tiempos (punto 100, raya 400,
  hueco 150, hueco de letra 900 ms), misma lista de eventos esperada y mismos
  contadores.
- El banco de pruebas del sketch (`bench/.../tests/host_duplex.cpp`) cubre los
  **mismos casos** pero con sus propios tiempos (punto 100, raya 500, hueco 100)
  y comparando el texto impreso, así que no es el mismo vector numérico.
- El ancla común más fuerte es la evidencia grabada: `test_morse_link.py`
  re-decodifica `tanda_k40_l600_*.log` entera y `test_morse_decode.c` replica dos
  extractos de ese mismo log.
- Los vectores **no** están en `SPEC-DUPLEX.md` — ese documento fija pines,
  cableado, semántica, formato de salida, comandos y contadores. Los vectores
  viven en las suites de arriba.

Si cambias una implementación, cambia las otras y el SPEC en el mismo commit.

---

## 8. Lo que no está hecho

Dicho explícitamente, porque es tan parte de la guía como lo demás:

- **El firmware real no está medido en el banco.** `esps_morse` completo —las dos
  mitades— corre en las dos placas y publica sus canales, pero **nadie ha repetido
  con él las tandas que se hicieron con el sketch**. En particular, la afirmación de
  que los dos extremos coinciden a ±1 ms se midió con el sketch, y con el firmware
  depende de que la tarea de 1 ms no se retrase; eso no se ha comprobado.
- **El adaptador sigue siendo necesario** para una placa con el sketch Arduino. Las
  dos formas conviven a propósito: la práctica se puede dar sin ESP-IDF.
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

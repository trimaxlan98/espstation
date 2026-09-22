# Práctica: Morse full dúplex entre dos ESP32, una por persona

Dos placas, dos llaves, dos operadores. Cada uno teclea Morse con su llave y ve
decodificado en su pantalla **lo que teclea el otro**. No hay emisor ni receptor:
las dos placas llevan **el mismo sketch** y el cable va cruzado, un hilo por
sentido, como un null-modem serie.

Continuación de [`../clave-morse/`](../clave-morse/README.md), que tenía una placa
llave y una placa receptora. La decodificación es la misma; lo que cambia es que
ahora las dos mitades conviven en cada placa y hay una segunda mano en juego.

- Contrato: [`SPEC-DUPLEX.md`](SPEC-DUPLEX.md) — pines, formato, comandos, y lo
  que la práctica deliberadamente no cubre.
- Sketch único: [`transceptor/transceptor.ino`](transceptor/transceptor.ino).

## Qué se aprende que no enseñaba la práctica anterior

1. **Un enlace simétrico necesita un hilo por sentido.** Con dos extremos
   idénticos que hablan a la vez, el cruce TX→RX no es un detalle de montaje: es
   la única forma de que no haya turnos.
2. **El umbral pertenece a la mano que teclea, pero vive en la placa que
   escucha.** Al calibrar hay que decir siempre *de quién* es la mano y *en qué
   placa* está el número. Con una sola mano, como antes, esto no se veía.
3. **Unir dos ordenadores por un cable de datos une sus masas**, y eso no es
   gratis. Ver el aviso de abajo: es la parte de esta práctica que puede romper
   hardware.
4. **La comprobación cruzada de contadores**: lo que una placa cree haber enviado
   tiene que ser lo que la otra cuenta haber recibido, en los dos sentidos.

## Material

Por pareja:

- 2 ESP32 WROOM (`esp32:esp32:esp32`) con su cable USB.
- 2 jumpers pelados por sus dos puntas **por placa** (la llave) → 4 en total.
- 2 cables de datos suficientemente largos para llegar de una placa a la otra,
  más 1 para la masa → 3 hilos entre las dos placas.
- 2 resistencias de 330 Ω (una por hilo de datos). Recomendables, no exigidas.
- Opcional, 2 LEDs + 2 resistencias de 330 Ω **por placa**: uno para lo que
  envío, otro para lo que recibo.
- **Un multímetro**, si las placas van en ordenadores distintos. No es opcional
  en ese caso: ver el aviso.

## Cableado

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

La salida de cada placa entra en la entrada de la otra. Si al montarlo los dos
extremos quedan en el mismo pin (26→26), no pasa nada malo gracias a las 330 Ω,
pero no llega nada: `RX_DATA` se queda en 0 y parece que el compañero no teclea.

**La llave es metal contra metal, no el dedo.** Las dos puntas peladas se
presionan una contra otra: los dedos sólo dan la presión y el contacto es
metal-metal (~0 Ω). Cerrando con la piel, el dedo es una resistencia variable
(1 kΩ mojado a >500 kΩ seco) en un divisor contra el pull-down interno (~45 kΩ) y
el nivel cae en la zona indefinida, cambiando con la humedad de la mano.

## ⚠ Enlazar dos ordenadores: medir antes de unir las masas

El hilo de GND une las masas de los **dos ordenadores**. Las fuentes conmutadas
con la toma de tierra flotante (un cargador de portátil de dos clavijas) dejan
pasar tensión de red a través de sus condensadores Y: con un multímetro se mide
del orden de **la mitad de la tensión de red** entre el chasis y tierra. La
corriente está muy limitada —por eso cosquillea y no salta el diferencial— pero
sobra para matar un puerto USB o un GPIO. El jumper de GND es un cortocircuito
para esa diferencia y toda esa corriente pasa por él.

**Antes de conectar ningún hilo entre las dos placas:**

1. Enciende los dos ordenadores con sus placas enchufadas por USB, **sin ningún
   cable entre las placas**.
2. Multímetro en **tensión alterna (V~)**, escala alta: mide entre un pin GND de
   P1 y un pin GND de P2. Repite en **continua (V⎓)**.
3. Interpreta:
   - Las dos lecturas prácticamente **0** (menos de ~1 V): se puede unir.
   - **Decenas de voltios** en alterna: fuga de una fuente flotante. **No unas las
     masas todavía.**
4. Si hay tensión, en este orden de preferencia:
   - **Los dos portátiles con la batería y el cargador desenchufado.** Quita la
     referencia a la red de un lado, y suele bastar. Vuelve a medir.
   - Los dos ordenadores **en la misma regleta**, compartiendo la misma tierra.
   - Aislamiento galvánico de verdad: un **optoacoplador por sentido**. Es la
     solución correcta de ingeniería; añade piezas y no está montada en esta
     práctica.
5. Conecta **primero la masa** y sólo después los dos hilos de datos.
   Desconéctala **la última**. Si un hilo de datos entra antes que la masa, el
   único camino de vuelta para la corriente es el diodo de protección del pin, o
   sea justo lo que se intenta evitar.

Con las **dos placas en el mismo ordenador** nada de esto aplica: la masa ya es
común a través del USB. Por eso la etapa 1 se hace así.

### «¿Y si uso una protoboard con la masa común?»

La protoboard **no aísla: conduce**. Unir las dos masas en un carril común es
eléctricamente lo mismo que unirlas con un jumper; sólo cambia el trozo de cobre
por el que pasa la corriente de fuga. No es una alternativa a medir.

Lo que decide si hay riesgo es **de dónde salen los dos cables USB**:

| Montaje | Riesgo |
|---|---|
| Dos placas en una protoboard, los dos USB **al mismo ordenador** | **Ninguno**: la masa ya era común por el USB. Es la etapa 1. |
| Dos placas en una protoboard, cada USB **a un ordenador distinto** | **El mismo**: el carril de GND es el puente entre las dos masas. Hay que medir. |

Para el **cableado** sí conviene: el cruce y las dos 330 Ω quedan mucho más
limpios en protoboard que punta a punta. Y si las dos placas comparten
protoboard, los dos ordenadores están pegados, así que enchufarlos a la misma
regleta sale gratis.

## Compilar y subir

El **mismo** sketch en las dos placas, sin cambiar nada:

```bash
cd bench/practicas/morse-duplex
arduino-cli compile --fqbn esp32:esp32:esp32 transceptor

# Linux/macOS
arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 transceptor
arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/ttyUSB1 transceptor

# Windows
arduino-cli upload --fqbn esp32:esp32:esp32 -p COM5 transceptor
arduino-cli upload --fqbn esp32:esp32:esp32 -p COM7 transceptor
```

Si una placa contesta `Wrong boot mode detected (0x13)`, mantén pulsado **BOOT**
durante la subida: es una placa con la línea DTR→GPIO0 muerta, no un fallo del
sketch. Está documentado en `../clave-morse/INFORME.md` §9.

## Etapa 1 — ensayo en un solo ordenador (sin riesgo, y prueba todo el firmware)

Antes de juntar a las dos personas, monta las **dos placas en el mismo
ordenador** con el cable cruzado. La masa ya es común, así que no hay nada que
medir, y el montaje ejercita el firmware completo: los dos sentidos, los dos
decodificadores y el cruce. Teclea con una llave y mira la otra placa.

```bash
# una terminal por placa (Windows; en Linux, /dev/ttyUSB0 y /dev/ttyUSB1)
python ../clave-morse/herramientas/captura_serie.py COM5 evidencia/p1.log cmd_p1.txt
python ../clave-morse/herramientas/captura_serie.py COM7 evidencia/p2.log cmd_p2.txt
```

Lo que tiene que pasar:

- Al cerrar la llave de P1, se enciende `LED_TX` de P1 **y** `LED_RX` de P2.
- P2 imprime `RX .` / `RX -`; P1 imprime `TX .` / `TX -` de lo mismo.
- `r` en las dos placas: `tx.puntos + tx.rayas` de P1 = `rx.puntos + rx.rayas` de
  P2, y al revés.

Cuando esto sale, el firmware está validado y lo único que añade la etapa 2 es la
logística de los dos ordenadores.

## Etapa 2 — sesión en equipo

1. Haz la medida de masas de arriba. **Sin excepción.**
2. Conecta GND, luego los dos hilos de datos.
3. Cada uno abre su placa (monitor serie del IDE de Arduino, o la captura de
   abajo) y manda `r` para dejar constancia de los umbrales de partida.
4. Calibrad **por turnos**: uno teclea `SOS` varias veces mientras el otro ajusta
   el `p<ms>` de **su** placa hasta que le salgan `S` y `O`. Luego al revés.
   El número que ajusta cada uno describe la mano del otro.
5. Cuando los dos umbrales están fijos, **no se tocan más** y empieza la tanda de
   evaluación. Un umbral cambiado a mitad de tanda invalida la tanda.

### Calibrar: de quién es la mano, en qué placa está el número

| Quiero corregir | Comando | En qué placa |
|---|---|---|
| Las rayas del otro me salen como puntos | `p<más bajo>` | **la mía** |
| Mis rayas le salen como puntos al otro | `p<más bajo>` | **la del otro** |
| El otro junta dos letras en una | `l<más bajo>` | la mía |
| El otro parte una letra en dos | `l<más alto>` | la mía |

Con `v` (verbose) cada placa imprime `# RX pulso_ms=…`: ésa es la medida real de
la mano del otro, y de ahí sale el umbral. La banda vacía entre el punto más
largo y la raya más corta es donde hay que poner `p`.

## Leer la salida

```
RX .
RX .
RX .
RX [letra: S] [bin: 01010011]
TX -
TX [letra: T] [bin: 01010100]
RX [palabra]
```

`RX` es del otro, `TX` es el eco de mi propia llave. Detrás del prefijo va
exactamente el formato de `../clave-morse`. Las líneas con `#` son diagnóstico.

Si sólo quieres leer el mensaje del otro, apaga el eco con `e` y filtra. En el
monitor serie las líneas empiezan por `RX`, pero en un log de `captura_serie.py`
va delante la marca de tiempo del host, así que el patrón lleva el hueco:

```bash
grep -E '^ *[0-9]+\.[0-9]+ RX ' evidencia/p1.log      # del log de evidencia
grep '^RX '                      # de la salida cruda del monitor serie
```

## Herramientas

Se reutilizan **las de `../clave-morse/herramientas/`** sin copiarlas, porque
funcionan por puerto y no saben nada de roles:

```bash
# En CADA ordenador, una placa, un log:
python ../clave-morse/herramientas/captura_serie.py COM5 evidencia/p1.log cmd_p1.txt
python3 ../clave-morse/herramientas/captura_serie.py /dev/ttyUSB0 evidencia/p1.log /tmp/cmd_p1
```

y desde otra terminal del mismo ordenador, para mandar comandos sin cerrar el
puerto (en Windows el puerto lo abre un solo proceso):

```bash
echo "p170"                 > /tmp/cmd_p1       # Linux/macOS
Add-Content cmd_p1.txt "p170"                   # Windows
Add-Content cmd_p1.txt "#MARK INICIO TANDA 1"
```

En POSIX esa ruta tiene que ser una **FIFO** (la crea el propio capturador). Si
ahí ya hay un fichero normal, el capturador **para** en vez de abrirlo: lo que
hubiera dentro se le habría mandado entero a la placa en la primera lectura, y
`echo "r" > ruta` (que trunca) habría dejado el canal mudo sin decirlo. Bórralo o
usa otra ruta.

### Dashboard en vivo

`../clave-morse/herramientas/puente_serie.py` aprendió el formato de esta
práctica. Con `--duplex` usa el intérprete de las líneas `RX `/`TX `, acepta los
comandos del transceptor (`k`, `e` y los del eco con `t` delante) y sirve el visor
de aquí, [`visor/index.html`](visor/index.html):

```bash
python ../clave-morse/herramientas/puente_serie.py --duplex --a COM5 --b COM7 \
    --log-a evidencia/tanda_A.log --log-b evidencia/tanda_B.log --marcas marcas.txt
# y abrir http://127.0.0.1:8765/
```

Un solo proceso abre los dos puertos, sirve **una sola página con las dos
estaciones** y escribe la evidencia canónica al mismo tiempo. Con las placas en
ordenadores distintos, cada máquina arranca su propio puente con un solo puerto
(`--duplex --a COM5`), pero entonces cada pantalla ve sólo su mitad y se pierde el
cruce de integridad.

#### La señal, que es lo que la práctica tiene que enseñar

Lo primero y más grande de la página es un **lienzo animado** que se desplaza en
tiempo real, con **un carril por sentido**: `A → B` y `B → A`. Cada carril dibuja
el nivel de esa llave como onda cuadrada, y encima de cada pulso aparece el
símbolo (`.` o `-`) en cuanto se decide; la letra sale en la línea de reposo con
su marca vertical, y `/` en el corte de palabra.

Dos detalles que lo hacen útil y no decorativo:

- **El símbolo y la letra que se dibujan son los que decodificó quien escucha**,
  no quien envía. Lo que ves en el carril `A → B` es lo que B entendió. Si el otro
  extremo no está conectado, se usa el eco local como respaldo.
- A la derecha de cada carril hay una **regla con el ancho de `punto_raya_ms`** a
  la misma escala del trazo: enseña físicamente cuánto tiene que durar un pulso
  para pasar de punto a raya.

Ventana seleccionable (8/15/30/60 s) y rejilla de segundos.

**Con las dos placas en el mismo ordenador**, el puente ve los flancos de las dos
(`# TX flanco`), así que los dos carriles se dibujan *mientras* ocurre el pulso.
**Con un ordenador por placa**, cada pantalla dibuja así sólo su propia llave; el
sentido entrante se reconstruye a partir de la duración medida y por tanto aparece
**al terminar cada pulso**, no mientras dura. El trazo es igual de exacto, pero
llega con el retardo de un pulso.

#### Lo demás, en secciones plegables

- **Tira de huecos** en escala logarítmica con `letra_ms` y `palabra_ms`
  marcados. Cada barra se colorea por lo que causó: nada, cerró letra, cerró
  palabra. Debajo, en texto: *«último hueco 412 ms — no cerró letra: faltaron 188
  ms para l=600»*.
- **Tira de pulsos** con `punto_raya_ms`, y el cálculo de la **banda vacía** entre
  el punto más largo y la raya más corta (avisa si baja de 40 ms).
- **Cruce de integridad** en vivo: pulsos enviados por una contra recibidos por la
  otra, en los dos sentidos.
- Umbrales de las dos placas, y un botón *«igualar cada eco al RX del otro»*:
  pone el eco de A con los umbrales con los que B escucha a A, que es la misma
  mano, para que el eco prediga lo que el otro va a decodificar.

### Reproducir una tanda grabada

Cualquier par de logs de `captura_serie.py` se puede volver a ver en el visor sin
placas, que es también como se prueba el visor:

```bash
python ../clave-morse/herramientas/puente_serie.py --duplex --velocidad 4 \
    --reproducir-a evidencia/tanda_k40_l600_A.log \
    --reproducir-b evidencia/tanda_k40_l600_B.log
```

En reproducción no hay placas, así que `/comando` responde 409 y los umbrales del
visor son de sólo lectura.

## Comprobar que el enlace no perdió nada

Al final de una tanda, `r` en las dos placas y cruza los números:

| De P1 | tiene que ser igual a | De P2 |
|---|---|---|
| `contadores TX puntos+rayas` | = | `contadores RX puntos+rayas` |
| `contadores RX puntos+rayas` | = | `contadores TX puntos+rayas` |

Y en las dos: `filtrados=0`, `desbordes_buffer=0`, `niveles_repetidos=0`. Si
`desbordes_buffer` no es 0, el `loop()` se quedó sin drenar el buffer de 64
flancos; si `filtrados` no es 0, hubo rebote o ruido en la línea.

Ojo: esto compara **pulsos**, no letras. Los pulsos pueden coincidir y las letras
no, y eso es exactamente lo interesante: significa que el enlace está bien y el
umbral está mal.

## Pruebas de host (sin hardware)

```bash
python3 tests/run_tests.py
```

Compila el `.ino` real con g++ (ASan/UBSan) sobre un mock del core Arduino y
comprueba, además de la tabla A–Z/0–9 contra un diccionario independiente, lo
propio del dúplex: que los dos sentidos **no se mezclan** cuando funcionan a la
vez, que los umbrales y contadores son independientes, y que el antirrebote de la
llave no altera la duración que ve el eco.

Prueba la **lógica**, no la temporización real ni nada del enlace entre dos
ordenadores.

En Windows hace falta un g++ que no sea el de MSVC; con LLVM-MinGW:

```powershell
$env:PATH = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW.UCRT_*\llvm-mingw-*\bin;$env:PATH"
python tests\run_tests.py
```

## Resultados de la etapa 1 (2026-09-21, dos placas en un ordenador)

Un operador, las dos llaves, umbrales **sin calibrar** (los de arranque).
Evidencia: [`evidencia/etapa1_a_hacia_b.log`](evidencia/etapa1_a_hacia_b.log) y
[`evidencia/etapa1_bidireccional.log`](evidencia/etapa1_bidireccional.log).

### El enlace conserva las duraciones dentro de ±1 ms

Comparando, pulso a pulso y por orden, el `# TX pulso_ms=` del que teclea contra
el `# RX pulso_ms=` del que escucha:

| Sentido | Pulsos comparados | Error máximo |
|---|---|---|
| A → B | 13 | **1 ms** |
| B → A | 10 | **1 ms** |

Ejemplos de los dos sentidos: 49/49, 77/**76**, 396/396, 452/452, 100/100 (A→B);
122/**123**, 97/97, 457/**458**, 485/485, 462/462 (B→A). Los silencios igual
(352/351, 872/872, 1156/1155). El error va **en los dos sentidos** (±1 ms), lo
que encaja con el truncado de microsegundos a milisegundos en cada extremo, y no
con una pérdida del cable.

### Integridad: los contadores cruzan exacto en los dos sentidos a la vez

Al final de la tanda bidireccional:

| | Placa A | Placa B |
|---|---|---|
| `llave_aceptados` (envié) | 12 | 20 |
| `rx_flancos_crudos` (recibí) | 20 | 12 |

12 = 12 y 20 = 20, en la misma tanda y sin tocar nada. `desbordes_buffer=0` y
`niveles_repetidos=0` en las dos placas.

### Los dos sentidos a la vez, de verdad

Con las **dos llaves pulsadas simultáneamente**, cada placa decodificó su propio
eco y el del otro sin mezclarlos: A recibió un pulso de 6754 ms (B decía 6753) y
B recibió a la vez uno de 4236 ms (A decía 4237). Es la comprobación de que el
dúplex no es un turno rápido.

### Un `SOS` salió `S T T T S`: el enlace bien, el umbral mal

Tecleando en B, las tres rayas de la `O` llegaron perfectas (457, 485, 462 ms)
pero los **silencios entre ellas fueron 872, 817 y 814 ms**, por encima de
`letra_ms=700`. El receptor cerró letra en cada hueco y la `O` se partió en tres
`T`. Los pulsos cuadran al milisegundo y las letras no: exactamente el caso que
el README describe en "Comprobar que el enlace no perdió nada".

Con la llave de A, en la misma sesión, los huecos entre rayas fueron 572 y 551 ms
y la `O` salió bien. **Mismo operador, distinta llave, distinto ritmo:** por eso
`letra_ms` hay que medirlo por pareja y no heredarlo.

### Rebote del contacto, medido

| Placa | `llave_crudos` | `llave_aceptados` | Rebotes suprimidos |
|---|---|---|---|
| A | 130 | 12 | 118 |
| B | 76 | 20 | 56 |

El antirrebote de 15 ms está haciendo trabajo de verdad; ninguno de esos rebotes
llegó al cable (los contadores del otro extremo lo confirman).

### Hallazgo que desmintió el contrato

Un pulso de ~15 ms de la llave pasó el filtro de la llave y lo descartó el
decodificador (`# TX pulso_ms=14 filtrado` en A y `# RX pulso_ms=14 filtrado` en
B, la misma pulsación). La sección "El filtro doble del camino TX" de
`SPEC-DUPLEX.md` decía que eso no podía pasar; **era falso** y está corregida con
la explicación (los dos filtros usan bases de tiempo distintas, milisegundos
enteros contra microsegundos truncados, así que la frontera es borrosa en ±1 ms).

### Lo que esta tanda NO demuestra

- **Una sola tanda por sentido, sin repetir.** No es una tasa de acierto.
- **Un solo operador con las dos llaves**, no dos personas. La pregunta de si dos
  manos distintas necesitan umbrales distintos sigue abierta.
- **Umbrales sin calibrar**: se dejaron los de arranque a propósito, para ver qué
  fallaba. `letra_ms` quedó identificado como el que hay que subir, pero no se
  subió ni se volvió a medir.
- **Nada del enlace entre dos ordenadores**: las dos placas estaban en el mismo
  PC, con la masa común por USB. El riesgo de masas no se ha tocado.

## Resultados de la tanda 2 (2026-09-21, con el dashboard en vivo)

Umbrales **congelados e idénticos en las dos placas** durante toda la tanda:
`p=300 l=600 w=1800 d=40` en RX y en el eco, `k=40` en la llave, `verbose=1`.
Evidencia: [`evidencia/tanda_k40_l600_A.log`](evidencia/tanda_k40_l600_A.log) y
[`evidencia/tanda_k40_l600_B.log`](evidencia/tanda_k40_l600_B.log).
Un solo operador, las dos llaves, mirando el visor mientras tecleaba.

### El eco y la decodificación remota coinciden carácter por carácter

Con los mismos umbrales en las dos placas, lo que el emisor ve en su eco es
**exactamente** lo que el receptor decodifica. Las dos cadenas salieron idénticas:

```
mano en A (49 pulsos), eco de A  == RX de B:
  .E .E .E / ...S / .--W ...-V -T / --.G / .E .E .E -T -T -T ...S / ...---? ...S / ---O ...S / ...S ---O ..I /
mano en B (30 pulsos), eco de B  == RX de A:
  ...S ---O ...S ---O ...S / ...---? ...S / -..---? /
```

No es trivial: valida que los dos sentidos corren el mismo código con el mismo
estado, y convierte el eco local en un predictor fiable de lo que recibe el otro.

### Integridad: exacta en los dos sentidos, también en letras

| | A → B | B → A |
|---|---|---|
| Pulsos (eco del emisor / RX del receptor) | 49 / **49** | 30 / **30** |
| Flancos (`llave_aceptados` / `rx_flancos_crudos`) | 98 / **98** | 60 / **60** |
| Letras (`letras` + `desconocidas`) | 21+1 / **21+1** | 6+2 / **6+2** |
| Error máximo de duración | **1 ms** | **1 ms** |

`filtrados=0`, `desbordes_buffer=0` y `niveles_repetidos=0` en los cuatro
decodificadores.

### `d40`/`k40` resolvió el ruido de contacto

Cero `filtrados` en toda la tanda, contra el pulso espurio de 33 ms y el hueco de
16 ms de la tanda anterior. El `H` por `S` no se repitió. Los rebotes siguen ahí
(A: 318 crudos para 98 flancos; B: 198 para 60) pero ninguno pasa al cable.

### `punto_raya_ms` es sólido; el problema es el hueco entre letras

| Mano | Puntos | Rayas | Punto más largo | Raya más corta | Banda vacía |
|---|---|---|---|---|---|
| en A | 31 | 18 | 249 ms | 380 ms | **131 ms** |
| en B | 17 | 13 | 220 ms | 358 ms | **138 ms** |

`p=300` cae casi en el centro de las dos. Ese umbral está resuelto.

### El hallazgo: para `letra_ms` no existe ningún umbral válido

No es que 600 esté mal elegido: es que **no hay ningún valor que funcione**, y la
tanda lo demuestra con dos errores de signo contrario **a la vez**:

- `.E .E .E` — tres puntos de una `S` cerraron como tres letras `E`. Hubo huecos
  **≥600 ms dentro** de una letra que se quería junta.
- `...---?` — una `S` y una `O` se fundieron en un solo código de 6 símbolos.
  Hubo un hueco **<600 ms entre** dos letras que se querían separadas.

Las dos poblaciones de huecos —los de dentro de la letra y los de entre letras—
**se solapan**. Ningún corte las separa. En el histograma aparece un claro entre
570 y 621 ms (mano en A) y entre 548 y 624 (mano en B), pero es escasez de
muestras, no separación: los errores de arriba prueban que las distribuciones se
pisan.

**La causa está en la mano, no en el número.** Los huecos *dentro* de la letra
fueron de 233 a 570 ms, cuando el punto mediano de esa mano mide **123 ms**. El
Morse estándar pide hueco de elemento = 1 unidad, entre letras = 3, entre
palabras = 7. Para una unidad de 123 ms eso es **123 / 369 / 861 ms**; los huecos
de elemento medidos son de 2 a 4,6 veces demasiado largos, así que invaden la
zona de "fin de letra" por arriba.

O sea, la corrección no es bajar ni subir `letra_ms`: es **acortar la pausa entre
los puntos de una misma letra**. Si el hueco de elemento baja a ~150 ms, entonces
`l≈300` y `w≈700` quedan con bandas anchas. Con el estilo lento actual, cualquier
`letra_ms` falla.

### Lo que el dashboard sí consiguió

Con el visor delante, la mano en B produjo **`S O S O S`: cinco letras seguidas
correctas**, el mejor tramo de las tres sesiones. La realimentación funciona; lo
que no arregla es un hueco de elemento estructuralmente largo.

### Lo que esta tanda NO demuestra

- Un solo operador con las dos llaves, una sola tanda, sin repetir.
- Los tres `SOS` limpios por llave **no se consiguieron** en ninguno de los dos
  sentidos: no hay tasa de acierto que dar.
- La hipótesis del hueco de elemento (~150 ms) **no se ha probado**: haría falta
  una tanda tecleando más rápido dentro de cada letra.

## Estado de verificación

| Qué | Estado |
|---|---|
| Contrato (`SPEC-DUPLEX.md`) | escrito |
| Pruebas de host (`tests/run_tests.py`) | **35 pruebas, 0 fallos**, + tabla de 36 códigos contra diccionario independiente |
| Compilación para ESP32 (`arduino-cli`, `--warnings all`) | **limpia, sin avisos**: 278 852 B de flash (21 %), 23 340 B de RAM (7 %) |
| Pruebas del puente y del visor (`tests/test_visor.py`) | **27 pruebas, 0 fallos**, reproduciendo la evidencia real |
| Subida a las dos placas | hecha, mismo binario (279 008 B) en las dos |
| Etapa 1 (dos placas, un ordenador) | **hecha**: los dos sentidos, ver "Resultados" |
| Dashboard · tiras de cadencia | **hecho y usado** en la tanda 2 |
| Dashboard · animación de la señal | **hecho y probado con la evidencia**, pero **sin usar todavía en una tanda con las manos** |
| Etapa 2 (dos ordenadores, dos operadores) | **sin hacer** |
| `punto_raya_ms` | **medido**: `p=300`, banda vacía de 131 y 138 ms en las dos manos |
| `letra_ms` | **sin solución**: las poblaciones de huecos se solapan (ver tanda 2) |
| `debounce`/`llave` | **medido**: `d=40`/`k=40` dejan `filtrados=0` |

O sea: el enlace dúplex está demostrado en hardware, mide ±1 ms y el eco predice
carácter por carácter lo que recibe el otro. Lo que **no** está resuelto es la
cadencia de la mano, y lo que **no** está probado es nada del enlace entre dos
ordenadores ni la práctica con dos personas distintas.

No hay resultados de hardware en esta práctica todavía. Los valores de
`SPEC-DUPLEX.md` son puntos de partida heredados de `clave-morse`, y aquella
práctica midió ~170 ms en una mano y ~173 en otra; **eso no autoriza a darlos por
buenos aquí**, con dos operadores nuevos.

## Problemas

| Síntoma | Causa probable |
|---|---|
| No llega nada, `RX` siempre callado | cable sin cruzar (26→26 en vez de 26→25), o falta el puente de GND |
| Llegan símbolos sin tocar nada | ruido por masa mal unida o hilo largo sin las 330 Ω |
| `Wrong boot mode detected (0x13)` al subir | mantener pulsado BOOT; línea DTR→GPIO0 muerta en esa placa |
| Un puerto USB deja de dar COM | falta el driver CP210x; ver `../clave-morse/INFORME.md` §9 |
| El puerto está ocupado | en Windows lo abre un solo proceso: cierra el monitor serie del IDE |
| `# rechazado: comando desconocido` | dedazo; los umbrales del eco llevan `t` delante (`tp170`) |
| `# rechazado: k<ms> admite 0..200` tras escribir `k` | falta el número: `<ms>` es obligatorio y todo cifras. **No** se interpreta como `k0` |
| El eco `TX` no decodifica el primer toque tras un `e` | se apagó el eco con la llave cerrada: al encenderlo se resincroniza y descarta lo que quedaba a medias |
| Las letras salen mal pero los pulsos cuadran | umbral mal, enlace bien: `v` y mira `# RX pulso_ms=` |

# SPEC-MORSE — contrato de la clave Morse táctil entre dos ESP32

Contrato único de los dos sketches (`transmisor/transmisor.ino`,
`receptor/receptor.ino`). Si algo de aquí cambia, cambia en los dos sketches y en
el README, en el mismo commit.

> Esto **no** es el protocolo ENLP ni el contrato del enlace de N1–N3
> (`../enlace-digital/SPEC-LINK.md`, solo en el repositorio completo; **no hace falta para replicar esta práctica**).
> Sólo reutiliza sus reglas eléctricas.

## Roles — fijos, sin excepción

| Placa | Rol | Puerto habitual | Sketch |
|---|---|---|---|
| **A** | **transmisora / llave** | `/dev/ttyUSB0` | `transmisor/transmisor.ino` |
| **B** | **receptora / decodificadora** | `/dev/ttyUSB1` | `receptor/receptor.ino` |

Es la misma convención que N1 (`A` emite, `B` recibe). En esta práctica **A nunca
imprime texto Morse y B nunca lee la llave**. Los puertos son una convención de
banco: se confirman con etiquetas físicas, no por USB (las dos placas son CP2102
idénticas).

## Pines (ESP32 WROOM, todas las placas iguales)

| Señal | Placa | GPIO | Modo | Nota |
|---|---|---|---|---|
| `KEY_PIN` (llave táctil) | A | 13 | `INPUT_PULLDOWN` | una punta de jumper pelada aquí, la otra a **3V3** |
| `TX_DATA` | A | 26 | `OUTPUT` | espeja `KEY_PIN` (con antirrebote) hacia B |
| `RX_DATA` | B | 25 | `INPUT_PULLDOWN` | mismo pin que el receptor de N1 |
| LED testigo | A y B | 4 | `OUTPUT` | A: estado de la llave ya filtrado; B: nivel crudo recibido |
| GND | A↔B | — | — | puente **obligatorio** |

- GPIO13 no es pin de strapping y es seguro con `INPUT_PULLDOWN`.
- **GPIO12 prohibido**, como en el resto del banco.
- 25/26 son los mismos pines y **el mismo cable físico que N1**. 27 y 14 (reloj de
  N3) **no se usan**. La llave añade sólo GPIO13 en A.
- Serie a **115200** baudios en las dos placas.

### La llave: metal contra metal, no el dedo

El nivel lógico sale del **contacto metálico** entre dos puntas de jumper peladas:
los dedos sólo aplican presión. Tocar con el dedo una punta a 3V3 pone la piel
(1 kΩ mojada a >500 kΩ seca) en un divisor contra el pull-down interno (~45 kΩ) y
el nivel cae en la zona indefinida entre 0 y 1, según la humedad. Con metal-metal
el contacto es ~0 Ω y la llave se comporta como un pulsador.

Opcional y recomendable: una resistencia de 330 Ω en serie con la llave
(3V3 → 330 Ω → punta → punta → GPIO13). El nivel con el pulsador cerrado sigue
siendo ≈3,27 V y una configuración accidental de GPIO13 como salida ya no hace un
cortocircuito 3V3–GND por el pin. **La práctica no la exige.**

## Semántica de la señal

- Reposo: llave abierta → `KEY_PIN`=0 → `TX_DATA`=0 → `RX_DATA`=0.
- Llave cerrada = nivel **1**. La **duración a 1** es el símbolo; la duración a 0
  es el silencio.
- A espeja el nivel **filtrado**: cada flanco sale retrasado por el tiempo de
  antirrebote (`debounce_ms`), los dos con el mismo retardo, así que las
  duraciones que ve B no cambian.

## Transmisor (A)

- `loop()` sin `delay()` y **sin interrupciones**. Cada vuelta lee `KEY_PIN`.
- **Antirrebote:** un cambio de lectura sólo se acepta cuando la lectura se ha
  mantenido estable `debounce_ms` (por defecto **15 ms**, con `millis()`). Un
  rebote que revierte antes de ese tiempo no llega al enlace.
- Al aceptar un cambio: primero escribe `TX_DATA` y el LED, después imprime.
- Salida serie de A (sólo diagnóstico, no es Morse):
  - cabecera `t_ms,nivel_tx`;
  - una línea `t_ms,nivel_tx` por cada flanco **aceptado**;
  - cada 5 s, sólo si hubo actividad: `# resumen t_ms=… crudos=… aceptados=… nivel=…`.
- Comandos de una letra (línea terminada en `\n`):

| Comando | Efecto |
|---|---|
| `d<ms>` | fija `debounce_ms` (0..200; `d0` desactiva el filtro: sirve de contraste) |
| `c` | pone a cero los contadores `crudos` y `aceptados` |
| `r` | imprime `# umbrales debounce_ms=…` y los contadores |

- `crudos` = cambios de lectura del pin **antes** del filtro; `aceptados` = flancos
  que pasaron el filtro. Un toque completo (pulsar y soltar) da 2 aceptados.

## Receptor (B)

- `attachInterrupt(RX_DATA, …, CHANGE)`. La ISR (`IRAM_ATTR`) **sólo** sella
  `micros()`, lee el nivel de `GPIO.in`, cuenta y escribe en un buffer circular de
  64 flancos. **No imprime, no decodifica, no reserva memoria.** Si el buffer se
  llena, cuenta `desbordes_buffer` y descarta el flanco.
- `loop()` drena el buffer y corre la máquina de estados. Un flanco cuyo nivel es
  igual al anterior (la ISR lo leyó cuando la línea ya había vuelto) se ignora y se
  cuenta en `niveles_repetidos`.
- **Flanco de subida:** guarda `t_subida`.
- **Flanco de bajada** (sólo si hubo subida): `dur = t_bajada − t_subida`.
  - `dur < debounce_ms` → pulso filtrado, se ignora (`filtrados++`).
  - `dur < punto_raya_ms` → `.`; si no → `-`. El símbolo se imprime **en cuanto se
    decide**, en su propia línea, y se añade al símbolo en construcción (máx. 8).
- **Silencio** (línea en 0 desde la última bajada válida), medido en cada `loop()`:
  - `≥ letra_ms` con símbolo pendiente → decodifica, imprime la letra y su byte
    ASCII, limpia el símbolo;
  - `≥ palabra_ms` tras haber impreso al menos una letra desde el último
    separador → imprime **una sola vez** `[palabra]` (bandera de "ya reportado").
- La tabla Morse es una tabla de datos `{código, carácter}`, sin cadena de `if`.

### Valores de arranque (a calibrar en el banco, no definitivos)

| Constante | Valor | Significado |
|---|---|---|
| `punto_raya_ms` | 300 | `< ` esto = punto, `>=` = raya |
| `letra_ms` | 700 | silencio = fin de letra |
| `palabra_ms` | 1800 | silencio = fin de palabra |
| `debounce_ms` | 15 | pulsos a 1 más cortos se ignoran |

Comandos de B (línea terminada en `\n`, valores en ms, 1..60000; `d` 0..200):

| Comando | Efecto |
|---|---|
| `p<ms>` | fija `punto_raya_ms` |
| `l<ms>` | fija `letra_ms` (se rechaza si no es menor que `palabra_ms`) |
| `w<ms>` | fija `palabra_ms` (se rechaza si no es mayor que `letra_ms`) |
| `d<ms>` | fija `debounce_ms` del filtro de B |
| `v` | alterna verbose (por defecto **apagado**): añade líneas `# pulso_ms=…` y `# silencio_ms=…` para calibrar con datos reales |
| `c` | pone a cero los contadores |
| `r` | imprime umbrales y contadores |

## Formato de salida de B (una línea por evento, sin espacios finales)

```
.
-
.
[letra: S] [bin: 01010011]
-
-
-
[letra: O] [bin: 01001111]
[palabra]
```

- `[letra: X] [bin: bbbbbbbb]`: `X` es el carácter decodificado y `bbbbbbbb` su
  byte ASCII en **8 bits siempre**, con ceros a la izquierda (`A`=`01000001`,
  `E`=`01000101`, `S`=`01010011`, `O`=`01001111`). No se usa `Serial.print(c, BIN)`
  porque no rellena.
- Símbolo no presente en la tabla o de más de 8 pulsos: `[letra: ?] [morse: <código>]`
  (con `+` al final si se desbordó), sin byte.
- `[palabra]`: separador de palabra, una vez por hueco.
- Las líneas que empiezan por `#` son diagnóstico: umbrales, contadores, verbose.
- Cabecera al arrancar: `# receptor morse listo` seguida de los umbrales.

`SOS` = `... --- ...` es el caso de prueba obligatorio. Vectores de la tabla:

| Letra | Morse | ASCII | Letra | Morse | ASCII |
|---|---|---|---|---|---|
| S | `...` | `01010011` | O | `---` | `01001111` |
| A | `.-` | `01000001` | E | `.` | `01000101` |
| T | `-` | `01010100` | N | `-.` | `01001110` |

## Contadores del receptor (`r`)

`flancos_crudos` (entradas de la ISR), `filtrados`, `puntos`, `rayas`, `letras`,
`desconocidas`, `desbordes_buffer`, `niveles_repetidos`.

Relación esperada sin ruido: `flancos_crudos = 2 × (pulsos válidos + filtrados)`.
La **prueba de rebote** compara `flancos_crudos` con los toques reales que el
operador cuenta en voz alta (cada toque = 2 flancos).

## Lo que esta práctica NO cubre

- No hay corrección de errores ni CRC: un flanco espurio es un símbolo espurio, y
  eso es justo lo que se mide.
- No se toca `n1_nivel/`, `n2_handshake/` ni `n3_bytes/`.
- Los umbrales de arranque **no son medidos**: hasta que un operador real teclee
  "SOS" en el banco y se pegue el log, el README los marca como pendientes.

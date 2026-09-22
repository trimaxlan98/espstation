# SPEC-DUPLEX — contrato del transceptor Morse full dúplex

Contrato único de `transceptor/transceptor.ino`. Si algo de aquí cambia, cambia en
el sketch y en el README, en el mismo commit.

Práctica hermana de [`../clave-morse/`](../clave-morse/SPEC-MORSE.md), de la que
reutiliza la decodificación entera. Lo que cambia no es cómo se decodifica, es
**quién decodifica**: allí había una placa llave y una placa receptora; aquí las
dos placas hacen las dos cosas, cada una en manos de una persona distinta.

> Esto **no** es el protocolo ENLP ni el contrato de N1–N3
> (`../enlace-digital/SPEC-LINK.md`). Sólo reutiliza sus reglas eléctricas.

## Sin roles: el mismo sketch en las dos placas

| | clave-morse | morse-duplex |
|---|---|---|
| Sketches | 2 (`transmisor`, `receptor`) | **1** (`transceptor`), idéntico en las dos |
| Roles | A llave, B receptora, fijos | ninguno: las dos son iguales |
| Cable de datos | 1 hilo (A→B) | **2 hilos cruzados** (A→B y B→A) |
| Operadores | 1 | **2**, uno por placa |
| Ordenadores | 1 | 1 ó 2 (ver el aviso de masas) |

No hay comando ni puente que elija el rol: **las dos placas llevan el mismo
binario**. Si hiciera falta distinguirlas, se distinguen por el puerto serie y por
la etiqueta física, nunca por el firmware.

Llamamos **P1** y **P2** a las placas para poder hablar del cableado. No son
roles: son nombres de sitio, y son intercambiables.

## Pines (ESP32 WROOM, las dos placas iguales)

| Señal | GPIO | Modo | Nota |
|---|---|---|---|
| `KEY_PIN` (mi llave) | 13 | `INPUT_PULLDOWN` | una punta de jumper pelada aquí, la otra a **3V3** |
| `TX_DATA` (mi salida) | 26 | `OUTPUT` | espeja `KEY_PIN` ya filtrado, hacia `RX_DATA` del otro |
| `RX_DATA` (su salida) | 25 | `INPUT_PULLDOWN` | interrupción `CHANGE`; viene del `TX_DATA` del otro |
| `LED_TX` (testigo) | 4 | `OUTPUT` | mi llave, ya filtrada |
| `LED_RX` (testigo) | 16 | `OUTPUT` | nivel crudo que recibo del otro |
| GND | — | — | puente **obligatorio** entre las dos placas |

- Mismos pines que `clave-morse`: 13 llave, 26 salida, 25 entrada, 4 testigo. Lo
  único nuevo es `LED_RX` en **GPIO16**, para poder ver los dos sentidos a la vez.
- **GPIO12 prohibido**, como en el resto del banco. GPIO16/17 están libres en
  WROOM; en módulos **WROVER** los usa la PSRAM y habría que mover `LED_RX`.
- 27 y 14 (reloj de N3) no se usan.
- Serie a **115200** baudios en las dos placas.
- Los dos LEDs son opcionales: el sketch escribe los pines de todos modos.

## Cableado: cruzado, como un null-modem

```
   P1                                   P2
   GPIO26 (TX_DATA) ---- 330R ------->  GPIO25 (RX_DATA)
   GPIO25 (RX_DATA) <--- 330R --------  GPIO26 (TX_DATA)
   GND -------------------------------- GND          <-- OBLIGATORIO
```

La salida de cada una entra en la entrada de la otra. Es exactamente el cruce de
un cable serie null-modem, y por el mismo motivo: dos extremos idénticos que
hablan a la vez necesitan un hilo por sentido.

Las 330 Ω en serie no hacen de divisor (la entrada es de alta impedancia): limitan
la corriente si por error las dos puntas quedan como salidas enfrentadas.

### Es full dúplex de verdad, y eso quita un problema

Con **un hilo por sentido** los dos operadores pueden teclear al mismo tiempo sin
estorbarse: no hay bus compartido, no hay colisiones, no hace falta turno ni
detección de portadora. Esta práctica **no puede** enseñar nada sobre colisiones
justamente porque las ha eliminado por construcción. Un solo hilo compartido en
colector abierto (wired-OR) sí las tendría, y sería otra práctica.

### ⚠ Masas comunes entre dos ordenadores

El puente de GND une las masas de los **dos ordenadores**. Si están en enchufes
distintos puede haber tensión entre ellas: las fuentes conmutadas dejan tensión de
fuga a través de sus condensadores Y, con poca corriente pero suficiente para
matar un puerto USB o un GPIO. El jumper de GND es un cortocircuito para esa
diferencia y toda la corriente pasa por él.

El procedimiento de medida **antes** de unir las masas está en el README,
"Enlazar dos ordenadores". Con las dos placas en el **mismo** ordenador el
problema no existe, porque la masa ya es común por el USB.

## Semántica de la señal (igual que en clave-morse)

- Reposo: llave abierta → `KEY_PIN`=0 → `TX_DATA`=0 → el `RX_DATA` del otro = 0.
- Llave cerrada = nivel **1**. La duración a 1 es el símbolo; la duración a 0 es
  el silencio.
- Cada placa espeja al cable el nivel **ya filtrado**: los dos flancos salen
  retrasados el mismo `llave_debounce_ms`, así que las duraciones no cambian.

## Los dos decodificadores

El mismo código (`struct Dec` + `procesarFlanco`) corre **dos veces** por placa:

| Instancia | Qué mide | De quién es la mano |
|---|---|---|
| `RX` | los flancos que entran por `RX_DATA` (ISR) | del **otro** operador |
| `TX` | mi propia llave, ya filtrada (eco local) | **mía** |

El eco `TX` es opcional (`e` lo apaga) y no toca el cable: sirve para ver
decodificada mi propia mano y poder comparar *lo que quise mandar* con *lo que el
otro recibió*.

### El umbral es de la mano que teclea, pero vive en la placa que escucha

Consecuencia del reparto de arriba, y el punto que hay que entender de esta
práctica: `punto_raya_ms` de **mi** `RX` tiene que ajustarse al pulso **del
otro**, no al mío. Si mi compañero hace los puntos más largos, el umbral que hay
que subir es el de **mi** placa. Por eso son **dos juegos independientes** de
umbrales y no uno, y por eso al calibrar hay que decir siempre de quién es la
mano y en qué placa está el número.

En `clave-morse` esto no se veía: había una sola mano y un solo decodificador.

### El filtro doble del camino TX

En el camino de mi llave hay dos filtros en serie: `llave_debounce_ms` (antes de
escribir el cable) y `tx.debounce_ms` (dentro del decodificador). Con los valores
por defecto (15 y 15) casi nunca actúa el segundo, pero **sí actúa justo en la
frontera**, y está medido: un pulso de la llave de ~15 ms pasó el filtro de la
llave y el decodificador lo descartó como `# TX pulso_ms=14 filtrado` (evidencia
`etapa1_bidireccional.log`, t≈39 s).

El motivo es que los dos filtros **no usan la misma base de tiempo**:

- `llave_debounce_ms` compara `millis()` enteros, así que acepta el flanco en el
  milisegundo `t_candidato + 15`;
- `tx.debounce_ms` mide con `micros()` la distancia entre las dos aceptaciones y
  la trunca a milisegundos, y ese truncado puede dar 14 donde el otro contó 15.

Así que la frontera es borrosa en ±1 ms y un pulso de 15 ms puede caer a un lado
o al otro. No es un fallo: es el resultado de mezclar un filtro en milisegundos
con una medida en microsegundos, y conviene saberlo antes de interpretar un
`filtrados=1` como ruido. Con `tx.debounce_ms` > `llave_debounce_ms` el segundo
filtro ya descarta de forma franca.

Las pruebas de host **no pueden** cazar esto: el mock adelanta `micros()` en
múltiplos exactos de milisegundo, así que el truncado nunca discrepa y los dos
filtros coinciden siempre. Es una diferencia real entre el mock y la placa, y hay
que tenerla presente al confiar en `tests/run_tests.py`.

*(Antes esta sección afirmaba que el segundo filtro «nunca descarta nada» con los
valores por defecto. Era falso, y lo desmintió la primera tanda en hardware.)*

### Valores de arranque (no medidos)

| Constante | Valor | Significado |
|---|---|---|
| `punto_raya_ms` | 300 | `<` esto = punto, `>=` = raya |
| `letra_ms` | 700 | silencio = fin de letra |
| `palabra_ms` | 1800 | silencio = fin de palabra |
| `debounce_ms` | 15 | pulsos a 1 más cortos se ignoran |
| `llave_debounce_ms` | 15 | antirrebote de la llave propia |

Son los mismos que arrancaba `clave-morse`, **a propósito**: la práctica anterior
midió `punto_raya_ms ≈ 170` en una mano y `≈ 173` en otra, pero con dos operadores
nuevos hay que volver a medirlo y no dar por buenos los números de otra sesión.
Arrancar desde 300 hace visible el ajuste.

## Formato de salida

Cada línea de símbolo o letra va prefijada por el sentido, **`RX ` o `TX `**, y
detrás va exactamente la línea que imprimía `clave-morse`. Así una herramienta que
ya sabía leer aquel formato sirve quitando tres caracteres.

```
RX .
RX .
RX .
RX [letra: S] [bin: 01010011]
TX -
TX [letra: T] [bin: 01010100]
RX [palabra]
```

- `[letra: X] [bin: bbbbbbbb]`: byte ASCII en **8 bits siempre**, con ceros a la
  izquierda. No se usa `Serial.print(c, BIN)` porque no rellena.
- Código no presente en la tabla o de más de 8 pulsos:
  `[letra: ?] [morse: <código>]` (con `+` al final si se desbordó), sin byte.
- `[palabra]`: separador, una vez por hueco y **por sentido**.
- Las líneas que empiezan por `#` son diagnóstico y se pueden filtrar sin perder
  nada del mensaje.

### Cabecera al arrancar

```
# transceptor morse listo (el mismo sketch en las dos placas)
# umbrales RX punto_raya_ms=300 letra_ms=700 palabra_ms=1800 debounce_ms=15
# umbrales TX punto_raya_ms=300 letra_ms=700 palabra_ms=1800 debounce_ms=15
# contadores RX puntos=0 rayas=0 letras=0 desconocidas=0 filtrados=0 niveles_repetidos=0 flancos_crudos=0 desbordes_buffer=0
# contadores TX puntos=0 rayas=0 letras=0 desconocidas=0 filtrados=0 niveles_repetidos=0
# llave debounce_ms=15 crudos=0 aceptados=0
# modo eco_local=1 verbose=0
```

### Verbose (`v`, por defecto apagado)

Añade, etiquetado por sentido, lo necesario para calibrar y para comparar los dos
extremos:

```
# TX flanco t_ms=12345 nivel=1
# TX pulso_ms=180
# RX pulso_ms=181
# RX silencio_ms=420
```

### Resumen periódico

Cada 5 s, sólo si hubo actividad:

```
# resumen t_ms=… llave_crudos=… llave_aceptados=… rx_flancos_crudos=… nivel_tx=… nivel_rx=…
```

## Comandos (línea terminada en `\n`)

Sin prefijo, los umbrales van al decodificador **RX** (la mano del otro), que es
el que casi siempre hay que ajustar. Con **`t`** delante van al eco local.

| Comando | Efecto |
|---|---|
| `p<ms>` | `punto_raya_ms` de RX (1..60000) |
| `l<ms>` | `letra_ms` de RX (se rechaza si no es menor que `palabra_ms`) |
| `w<ms>` | `palabra_ms` de RX (se rechaza si no es mayor que `letra_ms`) |
| `d<ms>` | `debounce_ms` de RX (0..200) |
| `tp/tl/tw/td<ms>` | lo mismo para el eco local TX, con las mismas validaciones |
| `k<ms>` | `llave_debounce_ms`, antirrebote de mi llave (0..200; `k0` lo desactiva) |
| `e` | eco local on/off (no afecta al cable) |
| `v` | verbose on/off |
| `c` | pone a cero **todos** los contadores (los dos sentidos y la llave) |
| `r` | imprime umbrales, contadores y modo |

Un comando que no encaje responde `# rechazado: comando desconocido <cmd>`, para
que un dedazo en el monitor serie no pase por silencio.

## Contadores

Por sentido (`RX` y `TX`): `puntos`, `rayas`, `letras`, `desconocidas`,
`filtrados`, `niveles_repetidos`. Sólo `RX` añade `flancos_crudos` (entradas de la
ISR) y `desbordes_buffer`. Aparte, de la llave propia: `crudos` y `aceptados`.

Relaciones esperadas sin ruido:

- `llave_aceptados = 2 × (mis pulsos)` — un toque es pulsar y soltar.
- `rx_flancos_crudos = 2 × (pulsos del otro + filtrados de RX)`.
- **`tx.puntos + tx.rayas` de una placa = `rx.puntos + rx.rayas` de la otra.**
  Ésta es la comprobación propia del dúplex: lo que yo creo haber enviado tiene
  que ser lo que el otro cuenta haber recibido, en los **dos** sentidos.

## Lo que sí se puede medir y lo que no

**Sí**, comparando los dos logs con `verbose`: para el pulso n-ésimo, el
`# TX pulso_ms=` del que teclea contra el `# RX pulso_ms=` del que escucha. Si
coinciden dentro de unos ms, el enlace conserva las duraciones, que es lo único
que Morse necesita. La correspondencia se hace **por orden**, no por reloj, y los
contadores avisan si se perdió algún pulso.

**No**, la latencia absoluta de un sentido: con las placas en ordenadores
distintos los `micros()` de cada una son independientes, sin origen común ni
sincronía, y no hay camino de ida y vuelta que recorra un solo reloj. Ningún par
de logs permite deducirla. Cualquier número de latencia que apareciera en este
informe sin un montaje nuevo sería inventado.

## Lo que esta práctica NO cubre

- **No hay detección de vida.** Con `INPUT_PULLDOWN`, un cable suelto da 0 estable
  — exactamente igual que un compañero que no teclea. El silencio es ambiguo y no
  se puede distinguir "no dice nada" de "se rompió el hilo". Haría falta un latido
  periódico, y eso ya es un protocolo.
- **No hay colisiones** (ver arriba): son imposibles con un hilo por sentido.
- **No hay corrección de errores ni CRC**: un flanco espurio es un símbolo
  espurio, y eso es justo lo que se mide.
- **No hay reloj común**: nada de latencia absoluta ni de marcas de tiempo
  comparables entre los dos ordenadores.
- No se toca `../clave-morse/` ni `../enlace-digital/`.

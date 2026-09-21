# SPEC-LINK — contrato del enlace digital de dos hilos

Contrato único que comparten **tres implementaciones independientes**:
los sketches Arduino de la Fase 0 (`n3_bytes`), el componente de firmware
`esps_dio` (parte pura, C11) y el simulador del gateway (Python). Si algo de
aquí cambia, cambia en las tres y en sus vectores de prueba, en el mismo commit.

> Esto **no** es parte del protocolo ENLP. ENLP es lo que el nodo le dice a la
> estación; esto es lo que dos nodos se dicen por un cable. La estación sólo ve
> el resultado como canales NDB.

## Pines (ESP32 WROOM, todas las placas iguales)

| Señal | GPIO | Modo | Nota |
|---|---|---|---|
| `TX_DATA` | 26 | `OUTPUT` | |
| `RX_DATA` | 25 | `INPUT_PULLDOWN` | |
| `TX_CLK` | 27 | `OUTPUT` | sólo N3 |
| `RX_CLK` | 14 | `INPUT_PULLDOWN` | sólo N3 |
| LED testigo | 4 | `OUTPUT` | LED externo + 330 Ω. GPIO2 (on-board) es alternativa |
| GND | GND | — | puente **obligatorio** |

Prohibidos: GPIO6–11 (flash), **GPIO12 (strapping/MTDI, nunca)**. Evitar 0, 2,
5, 15. GPIO34–39 son sólo entrada. Salidas permitidas para `set_gpio`:
`4, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33`.

Cableado N2/N3 entre placas A y B: `A.26→B.25`, `B.26→A.25`, y en N3
`A.27→B.14`. Resistencia serie 330 Ω en cada línea. Nunca dos salidas unidas.

## Trama (N3)

```
0xAA | LEN (1 B, 1..32) | PAYLOAD (LEN B) | CRC8
```

- **CRC-8/SMBUS**: poly `0x07`, init `0x00`, sin reflexión, xorout `0x00`.
  Check value: `crc8("123456789") == 0xF4`.
- El CRC cubre `LEN || PAYLOAD` (no el `0xAA`).
- **Orden de bits: MSB primero.** Bytes en orden de trama.
- **Reloj**: en reposo `CLK=0` y `DATA=0`. El transmisor pone el bit en `DATA`
  con `CLK` bajo, espera medio periodo, sube `CLK` (el receptor muestrea en el
  flanco de subida), espera medio periodo, baja `CLK`. Tras el último bit
  `DATA` vuelve a 0.
- **Receptor** (una máquina de estados alimentada bit a bit): `HUNT` desplaza
  bits a un registro de 8 hasta ver `0xAA` → `LEN` (si `LEN==0` o `LEN>32`,
  descarta y vuelve a `HUNT`, cuenta `frames_len_err`) → `PAYLOAD` → `CRC`. Al
  completar entrega `{ok|crc_err, len, payload}` y vuelve a `HUNT`.
- Hueco mínimo entre tramas: 16 periodos de reloj.

### Detalles del receptor fijados por la implementación C (M1a) — las demás deben coincidir

- Al completar **cada byte** el registro de desplazamiento se pone a cero. Sin
  eso, una trama pegada a otra hace falso sincronismo cuando el CRC termina en
  `1010` (`0xA` en el nibble bajo → 4 bits antes de un `0xAA` real).
- Tras `LEN_ERR`, `len` vale 0. Tras `OK`/`CRC_ERR`, `len` y `payload` se
  conservan hasta que llega el siguiente payload.
- Un bit distinto de cero cuenta como 1.
- El generador de payload de prueba **rellena con ceros** `out[len..32)`;
  `stats_account` compara `len` bytes contra ese relleno cuando la longitud
  recibida difiere de la esperada.

### Debilidad conocida del formato (medida, no supuesta)

CRC-8 detecta todo error de 1 bit en `PAYLOAD`/`CRC`, pero **no protege el
campo `LEN` de sí mismo**: voltear un bit de `LEN` mueve la frontera de la
trama y el receptor compara un byte cualquiera con un CRC cualquiera, que
coincide por azar ~1/256 de las veces. Medido con el barrido de bit-flips sobre
las tramas `seq 0..999` (cada bit del cuerpo, uno a uno, con 36 bytes de ceros
de línea inactiva detrás):

| Zona volteada | Volteos | `FRAME_OK` espurios |
|---|---|---|
| `PAYLOAD` + `CRC` | 139 976 | 0 |
| `LEN` | 8 000 | **13** (0,16 %) |

Consecuencia: un `frames_ok` con `bit_errors > 0` puede ser una colisión por
`LEN` corrupto, y la contabilidad de abajo lo hace visible. Las implementaciones
deben reproducir **exactamente** estas cifras (13 y 0) con el mismo barrido.

## Contabilidad

- **`bit_errors` con longitud distinta** (decisión del orquestador, tras
  detectar tres reglas distintas en tres implementaciones): se comparan
  exactamente los `len` bytes **recibidos** contra el payload esperado rellenado
  con ceros hasta 32 B. Los bytes que faltan no se cuentan. Motivo: así
  `bit_errors ⊆ bits_rx` y `BER ≤ 1` siempre; una regla que cuente bytes
  faltantes metería errores que no están en el denominador.
- `bits_rx`: bits recibidos **después** del sincronismo (LEN+PAYLOAD+CRC) de
  cada trama completada (ok o crc_err).
- `frames_ok`, `frames_crc_err`, `frames_len_err`.
- `bit_errors`: bits distintos (popcount del XOR) entre el payload recibido y el
  esperado, sólo en tramas completadas. `BER = bit_errors / bits_rx`
  (0.0 si `bits_rx == 0`).
- Trama con CRC correcto pero payload distinto del esperado: contarla en
  `bit_errors` igual (es una colisión de CRC, y hay que verla).

## Payload de prueba (semilla conocida)

xorshift32, `SEED = 0xC0FFEE`. Para la trama número `seq` (uint32):

```
st  = SEED ^ (seq * 2654435761)        # aritmética uint32
if st == 0: st = 1
st  = xorshift32(st);  len = 1 + (st % 32)
payload[0] = seq & 0xFF
for i in 1..len-1:  st = xorshift32(st);  payload[i] = (st >> 8) & 0xFF

xorshift32(x): x ^= x << 13; x ^= x >> 17; x ^= x << 5      # uint32
```

El byte 0 lleva `seq & 0xFF` para que el receptor pueda **resincronizar**
el índice esperado tras tramas perdidas: si la trama recibida es válida
(`CRC ok`), el índice esperado pasa a `k + ((byte0 - k) & 0xFF)`. Las tramas con
CRC erróneo se comparan contra el índice esperado actual y luego éste avanza.

## Vectores dorados (obligatorios en los tests de las tres implementaciones)

CRC-8:

| Entrada | CRC-8 |
|---|---|
| `"123456789"` (ASCII) | `0xF4` |
| `00` | `0x00` |
| `01 00` | `0x15` |

Tramas completas (hex):

| Caso | Payload | Trama |
|---|---|---|
| `[0x00]` | `00` | `aa010015` |
| `"ESP"` | `455350` | `aa03455350f8` |
| `seq=1` | `012f` | `aa02012f0e` |
| `seq=999` | `e741` | `aa02e7413e` |
| `seq=0` | `004a9d11b4cb431957fd2d5e40a7affdd0` (17 B) | `aa11004a9d11b4cb431957fd2d5e40a7affdd017` |
| `seq=2` | `022464f86b91dc51d3fac2fb8ca4561b5197dee1` (20 B) | `aa14022464f86b91dc51d3fac2fb8ca4561b5197dee1a0` |

## Canales NDB (los publica el nodo en su `HELLO`; ids en el rango 16–127)

| id | key | tipo | unidad | grupo |
|---|---|---|---|---|
| 16 | `dio.tx` | u8 | — | `digital` |
| 17 | `dio.rx` | u8 | — | `digital` |
| 18 | `link.rtt_us` | u32 | µs | `link` |
| 19 | `link.frames_ok` | u32 | — | `link` |
| 20 | `link.frames_err` | u32 | — | `link` |
| 21 | `link.ber` | f32 | — | `link` |

`link.frames_err` = `frames_crc_err + frames_len_err`.

Eventos (`EVENT`, JSON, sin tocar el protocolo): `dio.edge`, `link.frame_ok`,
`link.crc_err`, `link.lost`, `link.up`, `dio.gpio_rejected`. `sys.*`, `exp.*`,
`net.*` y `fdir.*` están reservados; `dio.*` y `link.*` no.

## Semántica de eventos y de `set_gpio` (común a firmware y simulador)

Firmware y simulador emiten **los mismos códigos, severidades, claves de `data`
y límites de tasa**. La forma la marca el firmware (dos claves numéricas más un
`reason` de texto, sin más campos): el simulador se ajusta a ella, no al revés.

| `code` | severity | `data` | límite de tasa |
|---|---|---|---|
| `dio.edge` | debug | `level`, `count` | 5 por 1000 ms |
| `link.up` | info | `rtt_us`, `timeouts` | ninguno |
| `link.lost` (handshake) | warning | `timeouts`, `samples`, `reason:"handshake_timeout"` | ninguno |
| `link.lost` (tramas) | warning | `missed`, `seq`, `reason:"frames_missed"` | 1 por 5000 ms |
| `link.frame_ok` | info | `frames_ok`, `len` | 1 por 5000 ms |
| `link.crc_err` | warning | `frames_err`, `len`, `reason:"crc"` o `"len"` | 2 por 5000 ms |
| `dio.gpio` | info | `gpio`, `level` | ninguno |
| `dio.gpio_rejected` | warning | `gpio`, `level`, `reason` | ninguno |

- Un `len_err` **sí** emite `link.crc_err` con `reason:"len"` y **`len` = 0
  siempre**: el receptor descarta el byte LEN inválido (tras `LEN_ERR`, `len`
  vale 0, ver "Detalles del receptor"), así que ya no existe cuando se emite el
  evento. Cuenta en `frames_err` igual que un CRC malo.
- `data.suppressed` (eventos omitidos por el límite desde el último emitido)
  aparece **sólo cuando es > 0**, y sólo en los eventos con límite de tasa.
- Límite "N por W ms": ventana de W ms que admite N eventos; los demás se
  cuentan como `suppressed` y se suman al siguiente que sí salga. La semántica
  exacta es la de `esps_dio_ratelimit_*` en `firmware/components/esps_dio/`.
- Una trama con `len_err` **no** mueve el índice esperado (no cuenta como completada).
- `link.lost` por tramas: se emite cuando una trama OK resincroniza con un salto
  de índice (`missed` = tramas saltadas).
- `link.rtt_us` **no** se publica en un timeout. `link.lost` por handshake se
  emite tras **3 timeouts consecutivos**; `link.up` en el primer handshake
  exitoso desde el arranque o desde `lost`.
- **`set_gpio`** valida contra la lista de salidas permitidas. Un pin no
  permitido no actúa y emite `dio.gpio_rejected` (`reason` = el motivo). Un pin
  libre permitido se configura como salida y se escribe. En el firmware real la
  acción existe pero no es alcanzable desde la estación hasta S3 (`TODO(S3)`).
- **Pines del enlace y modo manual** (corrige el diseño anterior, hallazgo M1 de
  la revisión): `14` y `25` son las **entradas** del enlace: un nodo no puede
  saber si el otro extremo de ese cable es una salida, así que se rechazan
  **siempre** con `reason:"owned_by_link"`, también en modo manual. `26` y `27`
  son las salidas del enlace: se rechazan con `owned_by_link` salvo en **modo
  manual** (el nodo no programa fases del enlace y `dio.tx` sólo lo maneja
  `set_gpio`), donde se liberan. Nodos sin `esps_dio` rechazan los cuatro.
- **HELLO troceado**: un NDB que no cabe en `MAX_PAYLOAD` (1024 B; en el
  firmware el techo real es 885 B) se anuncia como varios `HELLO` completos,
  cada uno con un trozo de `ndb`; la estación los fusiona y responde un
  `HELLO_ACK` por cada uno. Motivo: 3 canales `sys.*` + los 6 de `dio` ya no
  caben en un frame. **El nodo deja de reanunciar sólo cuando ha recibido tantos
  `HELLO_ACK` aceptados como trozos envió en la ronda actual** (hallazgo A2: con
  el primer ACK se perdía el resto para siempre); cada ronda reenvía todos los
  trozos y reinicia la cuenta.

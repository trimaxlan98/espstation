/*
 * N3 - ENLACE SINCRONO DE DOS HILOS CON RELOJ EXPLICITO (ESP32 WROOM)
 *
 * Transmite tramas con verificacion (CRC-8) de una placa a otra y mide la
 * tasa de error de bit (BER). Contrato completo: SPEC-LINK.md (misma carpeta
 * padre). Este sketch lo implementa al byte; si algo aqui contradice el SPEC,
 * el que manda es el SPEC.
 *
 * ---------------------------------------------------------------------------
 *  POR QUE UN ENLACE SINCRONO (con reloj) Y NO UNA UART "A MANO"
 * ---------------------------------------------------------------------------
 *  Una UART (asincrona) manda solo datos y confia en que emisor y receptor
 *  hayan acordado la MISMA velocidad: cada uno cuenta el tiempo con su propio
 *  reloj y, si difieren en unos pocos %, los bits se van corriendo hasta leer
 *  mal. Aqui el emisor manda ademas una linea de RELOJ y el receptor lee el
 *  dato EN CADA FLANCO DE SUBIDA de ese reloj:
 *    - No depende de que los relojes de las dos placas coincidan: el receptor
 *      ni siquiera necesita saber la velocidad (solo la usa el emisor).
 *    - Es robusto: si el emisor se retrasa (una interrupcion, un delay) el
 *      reloj se estira y el receptor sigue sincronizado, porque el reloj SON
 *      los propios flancos. Solo hay un requisito temporal: que el dato este
 *      estable alrededor del flanco (aqui, medio periodo antes y despues).
 *    - Se explica mejor: "el dato vale lo que hay cuando sube el reloj".
 *  El precio es un cable mas (dos hilos de senal + GND).
 *
 * ---------------------------------------------------------------------------
 *  ROL Y CABLEADO (un solo sentido basta para medir BER)
 * ---------------------------------------------------------------------------
 *     Placa A (ROL_A, transmisor)        Placa B (ROL_B, receptor)
 *     GPIO26 (TX_DATA) --330R-->         GPIO25 (RX_DATA)
 *     GPIO27 (TX_CLK)  --330R-->         GPIO14 (RX_CLK)
 *     GND ------------------------------ GND           <-- OBLIGATORIO
 *
 *  Nunca unir dos salidas. GPIO12 PROHIBIDO (strapping/MTDI). Las entradas
 *  usan INPUT_PULLDOWN (una entrada flotante NO es un 0). GPIO14 emite un
 *  pulso PWM durante el arranque del ESP32: por eso la resistencia de 330 ohm.
 *
 * ---------------------------------------------------------------------------
 *  TRAMA:   0xAA | LEN (1..32) | PAYLOAD (LEN bytes) | CRC8
 *  CRC-8/SMBUS (poly 0x07, init 0, sin reflexion) sobre LEN||PAYLOAD.
 *  Bits MSB primero. Reloj y dato en reposo a 0. El transmisor pone el bit,
 *  espera medio periodo, sube CLK (ahi muestrea el receptor), espera medio
 *  periodo y baja CLK. Entre tramas, 16 periodos de reloj de hueco.
 * ---------------------------------------------------------------------------
 *
 *  USO (serie a 115200 en AMBAS placas)
 *    t      transmisor: inicia la prueba (1000 tramas, seq 0..999).
 *           receptor: pone a cero contadores. Pulsa 't' primero en el
 *           RECEPTOR y despues en el transmisor.
 *    r      receptor: imprime el reporte (tambien sale solo al llegar la
 *           trama 999 o tras 5 s sin tramas). transmisor: imprime su estado.
 *    +  -   sube/baja la velocidad por la lista 1000, 5000, 10000, 50000,
 *           100000, 200000 bit/s.
 *    b<N>   fija la velocidad directamente, p. ej. b50000 (100..500000).
 *  Solo el TRANSMISOR usa la velocidad para generar el reloj; el receptor la
 *  guarda solo para etiquetar el reporte. Aun asi, manda el mismo comando a
 *  las dos placas para que el reporte diga a que velocidad se midio.
 *
 *  Espera errores a partir de ~100-200 kbit/s: la latencia de la ISR del
 *  receptor (unos microsegundos) se acerca al medio periodo del reloj. Ese
 *  es justamente el limite que la practica quiere encontrar.
 *
 *  Salida: las lineas '#' son comentarios/progreso; el reporte es CSV.
 */

// ---- ELIGE EL ROL (exactamente uno) ---------------------------------------
#define ROL_A     // transmisor
// #define ROL_B  // receptor
// ---------------------------------------------------------------------------

#if defined(ROL_A) && defined(ROL_B)
#error "Define SOLO UNO: ROL_A o ROL_B, no los dos."
#endif
#if !defined(ROL_A) && !defined(ROL_B)
#error "Falta el rol: define ROL_A (transmisor) o ROL_B (receptor)."
#endif

#include <Arduino.h>
#include "soc/gpio_struct.h"

#define BIT_RATE_HZ 1000          // velocidad inicial (bit/s); cambiable por serie
#define TOTAL_TRAMAS 1000         // tramas de una prueba
#define HUECO_PERIODOS 16         // hueco minimo entre tramas (SPEC)

#define TX_DATA 26
#define TX_CLK 27
#define RX_DATA 25
#define RX_CLK 14

#ifdef ROL_A
static const bool SOY_TX = true;
#else
static const bool SOY_TX = false;
#endif

static_assert(TX_DATA < 32 && TX_CLK < 32 && RX_DATA < 32 && RX_CLK < 32,
              "el acceso directo a registro exige pines < 32");
static const uint32_t MASK_TX_DATA = 1UL << TX_DATA;
static const uint32_t MASK_TX_CLK = 1UL << TX_CLK;
static const uint32_t MASK_RX_DATA = 1UL << RX_DATA;

// --------------------------------------------------------------------------
//  Trama, CRC y payload de prueba (SPEC-LINK.md). Compartido por TX y RX:
//  un solo codigo, no dos versiones "por si acaso".
// --------------------------------------------------------------------------
#define SYNC_BYTE 0xAA
#define MAX_LEN 32
#define SEED 0xC0FFEEu

// Tipos del receptor. Van ANTES de cualquier funcion: el IDE de Arduino genera
// prototipos automaticos al inicio del archivo y, si un tipo se define despues,
// una funcion que lo use como parametro no compila.
enum : uint8_t { ST_HUNT = 0, ST_LEN, ST_PAYLOAD, ST_CRC };
enum : uint8_t { TR_OK = 0, TR_CRC_ERR = 1 };

struct Fsm {
  uint8_t estado;
  uint8_t registro;   // acumula bits (HUNT: ventana deslizante; resto: el byte en curso)
  uint8_t nbits;
  uint8_t len;
  uint8_t idx;
  uint8_t crc;
  uint8_t buf[MAX_LEN];
};

struct Trama {
  uint8_t estado;     // TR_OK | TR_CRC_ERR
  uint8_t len;
  uint8_t payload[MAX_LEN];
};

// CRC-8/SMBUS, un byte cada vez. Bit a bit (sin tabla): la tabla viviria en
// flash y esta funcion se llama desde la ISR.
static uint8_t IRAM_ATTR crc8Byte(uint8_t crc, uint8_t b) {
  crc ^= b;
  for (uint8_t i = 0; i < 8; i++) {
    crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
  }
  return crc;
}

static uint32_t xorshift32(uint32_t x) {
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return x;
}

// Payload de la trama numero `seq`; devuelve LEN. Byte 0 = seq & 0xFF para que
// el receptor pueda resincronizar el indice esperado tras tramas perdidas.
static uint8_t generarPayload(uint32_t seq, uint8_t *payload) {
  uint32_t st = SEED ^ (uint32_t)(seq * 2654435761u);
  if (st == 0) st = 1;
  st = xorshift32(st);
  uint8_t len = (uint8_t)(1 + (st % 32));
  payload[0] = (uint8_t)(seq & 0xFF);
  for (uint8_t i = 1; i < len; i++) {
    st = xorshift32(st);
    payload[i] = (uint8_t)((st >> 8) & 0xFF);
  }
  return len;
}

// Arma 0xAA|LEN|PAYLOAD|CRC8 en `out` (>= MAX_LEN + 3 bytes); devuelve el total.
static uint8_t armarTrama(const uint8_t *payload, uint8_t len, uint8_t *out) {
  out[0] = SYNC_BYTE;
  out[1] = len;
  uint8_t crc = crc8Byte(0, len);
  for (uint8_t i = 0; i < len; i++) {
    out[2 + i] = payload[i];
    crc = crc8Byte(crc, payload[i]);
  }
  out[2 + len] = crc;
  return (uint8_t)(len + 3);
}

// --------------------------------------------------------------------------
//  Velocidad (comun a ambos roles)
// --------------------------------------------------------------------------
static const uint32_t VELOCIDADES[] = {1000, 5000, 10000, 50000, 100000, 200000};
static const uint8_t N_VELOCIDADES = sizeof(VELOCIDADES) / sizeof(VELOCIDADES[0]);
static uint32_t g_rate_hz = BIT_RATE_HZ;
static uint32_t g_medio_ciclos = 0;   // medio periodo de reloj en ciclos de CPU (solo TX)

static void aplicarVelocidad(uint32_t hz) {
  if (hz < 100 || hz > 500000) {
    Serial.printf("# velocidad fuera de rango (100..500000): %u\n", (unsigned)hz);
    return;
  }
  g_rate_hz = hz;
  g_medio_ciclos = (getCpuFrequencyMhz() * 1000000UL) / (2UL * hz);
  Serial.printf("# velocidad = %u bit/s (medio periodo = %u ciclos de CPU)\n",
                (unsigned)hz, (unsigned)g_medio_ciclos);
}

static void cambiarVelocidad(int dir) {
  if (dir > 0) {
    for (uint8_t i = 0; i < N_VELOCIDADES; i++) {
      if (VELOCIDADES[i] > g_rate_hz) { aplicarVelocidad(VELOCIDADES[i]); return; }
    }
    aplicarVelocidad(VELOCIDADES[N_VELOCIDADES - 1]);
  } else {
    for (int i = N_VELOCIDADES - 1; i >= 0; i--) {
      if (VELOCIDADES[i] < g_rate_hz) { aplicarVelocidad(VELOCIDADES[i]); return; }
    }
    aplicarVelocidad(VELOCIDADES[0]);
  }
}

// ==========================================================================
//  TRANSMISOR
// ==========================================================================
static bool tx_activo = false;
static uint32_t tx_seq = 0;
static uint32_t tx_t_inicio_ms = 0;

// Espera activa con el contador de ciclos de CPU en lugar de micros(): a
// 100-200 kbit/s el medio periodo es de 5 a 2.5 us y micros() solo resuelve
// 1 us. El plazo se acumula (sin deriva) pero, si una interrupcion nos hizo
// llegar tarde, se reancla al presente: asi un retraso estira ESE medio
// periodo en vez de comprimir los siguientes hasta hacer pulsos ilegibles.
// Que el reloj se estire es inofensivo: el enlace es sincrono.
static inline void esperarCiclos(uint32_t &plazo) {
  plazo += g_medio_ciclos;
  uint32_t ahora = ESP.getCycleCount();
  if ((int32_t)(ahora - plazo) > (int32_t)g_medio_ciclos) plazo = ahora;
  while ((int32_t)(ESP.getCycleCount() - plazo) < 0) { }
}

static void enviarBit(uint8_t bit, uint32_t &plazo) {
  if (bit) GPIO.out_w1ts = MASK_TX_DATA; else GPIO.out_w1tc = MASK_TX_DATA;  // dato con CLK bajo
  esperarCiclos(plazo);
  GPIO.out_w1ts = MASK_TX_CLK;    // flanco de subida: aqui muestrea el receptor
  esperarCiclos(plazo);
  GPIO.out_w1tc = MASK_TX_CLK;
}

static void enviarTrama(const uint8_t *trama, uint8_t total) {
  uint32_t plazo = ESP.getCycleCount();
  for (uint8_t i = 0; i < total; i++) {
    for (int8_t b = 7; b >= 0; b--) enviarBit((trama[i] >> b) & 1, plazo);   // MSB primero
  }
  GPIO.out_w1tc = MASK_TX_DATA;   // tras el ultimo bit, DATA vuelve a 0
  for (uint8_t i = 0; i < 2 * HUECO_PERIODOS; i++) esperarCiclos(plazo);
}

static void txIniciar() {
  tx_seq = 0;
  tx_activo = true;
  Serial.printf("# TX inicio: %u tramas a %u bit/s\n", (unsigned)TOTAL_TRAMAS, (unsigned)g_rate_hz);
  delay(300);   // margen para que el receptor ya este a cero y escuchando
  tx_t_inicio_ms = millis();
}

static void txLoop() {
  if (!tx_activo) return;
  if (tx_seq >= TOTAL_TRAMAS) {
    tx_activo = false;
    Serial.printf("# TX fin: %u tramas enviadas en %u ms\n", (unsigned)tx_seq,
                  (unsigned)(millis() - tx_t_inicio_ms));
    return;
  }
  uint8_t payload[MAX_LEN];
  uint8_t trama[MAX_LEN + 3];
  uint8_t len = generarPayload(tx_seq, payload);
  uint8_t total = armarTrama(payload, len, trama);
  enviarTrama(trama, total);
  tx_seq++;
  if (tx_seq % 100 == 0) Serial.printf("# TX enviadas=%u\n", (unsigned)tx_seq);
}

// ==========================================================================
//  RECEPTOR: maquina de estados alimentada bit a bit desde la ISR
// ==========================================================================
// Cola ISR -> loop. Productor unico (ISR) y consumidor unico (loop), en el
// mismo nucleo: basta con indices volatile y una barrera de compilador. Una
// cola de varias tramas evita perder alguna mientras loop() imprime.
#define RING_N 8
static Fsm g_fsm;
static Trama g_ring[RING_N];
static volatile uint8_t g_head = 0;
static volatile uint8_t g_tail = 0;
static volatile uint32_t g_len_err = 0;
static volatile uint32_t g_overruns = 0;
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR rxEntregar(uint8_t estado) {
  uint8_t h = g_head;
  uint8_t n = (uint8_t)((h + 1) % RING_N);
  if (n == g_tail) {                 // cola llena: se pierde la trama (queda contado)
    g_overruns = g_overruns + 1;
    return;
  }
  g_ring[h].estado = estado;
  g_ring[h].len = g_fsm.len;
  for (uint8_t i = 0; i < g_fsm.len; i++) g_ring[h].payload[i] = g_fsm.buf[i];
  asm volatile("" ::: "memory");     // que los datos queden escritos ANTES de publicar el indice
  g_head = n;
}

static void IRAM_ATTR rxVolverAHunt() {
  g_fsm.estado = ST_HUNT;
  g_fsm.registro = 0;
  g_fsm.nbits = 0;
}

static void IRAM_ATTR rxAlimentarBit(uint8_t bit) {
  g_fsm.registro = (uint8_t)((g_fsm.registro << 1) | bit);

  if (g_fsm.estado == ST_HUNT) {
    if (g_fsm.registro == SYNC_BYTE) {
      g_fsm.estado = ST_LEN;
      g_fsm.registro = 0;
      g_fsm.nbits = 0;
    }
    return;
  }

  if (++g_fsm.nbits < 8) return;
  uint8_t byte = g_fsm.registro;
  g_fsm.registro = 0;
  g_fsm.nbits = 0;

  switch (g_fsm.estado) {
    case ST_LEN:
      if (byte == 0 || byte > MAX_LEN) {
        g_len_err = g_len_err + 1;
        rxVolverAHunt();
      } else {
        g_fsm.len = byte;
        g_fsm.idx = 0;
        g_fsm.crc = crc8Byte(0, byte);
        g_fsm.estado = ST_PAYLOAD;
      }
      break;
    case ST_PAYLOAD:
      g_fsm.buf[g_fsm.idx++] = byte;
      g_fsm.crc = crc8Byte(g_fsm.crc, byte);
      if (g_fsm.idx == g_fsm.len) g_fsm.estado = ST_CRC;
      break;
    case ST_CRC:
      rxEntregar(byte == g_fsm.crc ? TR_OK : TR_CRC_ERR);
      rxVolverAHunt();
      break;
  }
}

// El dato se lee en el instante del flanco de subida del reloj. Sin Serial ni
// malloc aqui dentro: solo acceso a registro y la maquina de estados.
static void IRAM_ATTR isrReloj() {
  rxAlimentarBit((GPIO.in & MASK_RX_DATA) ? 1 : 0);
}

// ---- contabilidad (solo la toca loop()) -----------------------------------
static uint32_t c_ok = 0, c_crc_err = 0, c_bits_rx = 0, c_bit_errors = 0;
static uint32_t rx_esperado = 0;        // indice de trama que se espera ahora
static uint32_t rx_t_ultima_ms = 0;
static bool rx_reportado = false;

static void rxReset() {
  portENTER_CRITICAL(&g_mux);
  rxVolverAHunt();
  g_head = 0;
  g_tail = 0;
  g_len_err = 0;
  g_overruns = 0;
  portEXIT_CRITICAL(&g_mux);
  c_ok = c_crc_err = c_bits_rx = c_bit_errors = 0;
  rx_esperado = 0;
  rx_reportado = false;
  Serial.println("# RX a cero, escuchando");
}

static uint32_t popcount8(uint8_t x) {
  uint32_t n = 0;
  while (x) { n += x & 1; x >>= 1; }
  return n;
}

// `final_de_prueba`: solo el reporte automatico de fin de prueba marca
// rx_reportado; un 'r' manual a mitad de prueba no debe impedir el final.
static void rxReporte(bool final_de_prueba) {
  uint32_t completas = c_ok + c_crc_err;
  uint32_t perdidas = (completas < TOTAL_TRAMAS) ? (TOTAL_TRAMAS - completas) : 0;
  double ber = (c_bits_rx > 0) ? (double)c_bit_errors / (double)c_bits_rx : 0.0;
  Serial.println("# ---- reporte RX ----");
  Serial.printf("# velocidad_declarada=%u bit/s, overruns_cola=%u\n", (unsigned)g_rate_hz,
                (unsigned)g_overruns);
  Serial.println("frames_ok,frames_crc_err,frames_len_err,bits_rx,bit_errors,BER,frames_perdidas");
  Serial.printf("%u,%u,%u,%u,%u,%.3e,%u\n", (unsigned)c_ok, (unsigned)c_crc_err,
                (unsigned)g_len_err, (unsigned)c_bits_rx, (unsigned)c_bit_errors, ber,
                (unsigned)perdidas);
  if (final_de_prueba) rx_reportado = true;
}

static void rxProcesar(const Trama &t) {
  uint32_t k = rx_esperado;
  uint32_t idx = k;
  if (t.estado == TR_OK) {
    // Resincronizar: el byte 0 lleva seq & 0xFF, asi que tras tramas perdidas
    // sabemos a cual nos hemos saltado.
    idx = k + ((t.payload[0] - k) & 0xFFu);
    c_ok++;
  } else {
    c_crc_err++;   // se compara contra el indice esperado actual
  }
  c_bits_rx += 8u * (t.len + 2u);   // LEN + PAYLOAD + CRC, despues del sincronismo

  // Regla de SPEC-LINK.md: se comparan exactamente los `len` bytes RECIBIDOS
  // contra el payload esperado rellenado con ceros hasta 32 B; los bytes que
  // faltan no se cuentan. Asi bit_errors esta incluido en bits_rx y BER <= 1.
  // Una trama con CRC valido pero payload distinto tambien cuenta aqui
  // (colision de CRC: hay que verla).
  uint8_t esperado[MAX_LEN] = {0};
  generarPayload(idx, esperado);
  for (uint8_t i = 0; i < t.len; i++) c_bit_errors += popcount8(t.payload[i] ^ esperado[i]);

  rx_esperado = idx + 1;
  rx_t_ultima_ms = millis();
  if (idx >= TOTAL_TRAMAS - 1 && !rx_reportado) rxReporte(true);
}

static void rxLoop() {
  while (g_tail != g_head) {
    Trama t = g_ring[g_tail];
    asm volatile("" ::: "memory");
    g_tail = (uint8_t)((g_tail + 1) % RING_N);
    rxProcesar(t);
  }
  // La prueba puede terminar sin que llegue la trama 999 (se perdio): 5 s de
  // silencio tras haber recibido algo = fin.
  if (!rx_reportado && (c_ok + c_crc_err) > 0 && millis() - rx_t_ultima_ms > 5000) rxReporte(true);
}

// ==========================================================================
//  Comandos por serie (ambas placas)
// ==========================================================================
static bool num_modo = false;
static uint32_t num_val = 0;
static uint32_t num_t_ms = 0;

static void cerrarNumero() {
  if (num_modo) aplicarVelocidad(num_val);
  num_modo = false;
}

static void comando(char c) {
  switch (c) {
    case 't':
      if (SOY_TX) txIniciar(); else rxReset();
      break;
    case 'r':
      if (SOY_TX) {
        Serial.printf("# TX activo=%d seq=%u velocidad=%u bit/s\n", (int)tx_activo,
                      (unsigned)tx_seq, (unsigned)g_rate_hz);
      } else {
        rxReporte(false);
      }
      break;
    case '+': cambiarVelocidad(+1); break;
    case '-': cambiarVelocidad(-1); break;
    case 'b': num_modo = true; num_val = 0; num_t_ms = millis(); break;
    default: break;   // '\n', '\r', espacios y lo desconocido se ignoran
  }
}

static void atenderSerie() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (num_modo) {
      if (c >= '0' && c <= '9') {
        num_val = num_val * 10 + (uint32_t)(c - '0');
        num_t_ms = millis();
        continue;
      }
      cerrarNumero();   // cualquier otro caracter (p. ej. fin de linea) cierra el numero
    }
    comando(c);
  }
  // Monitores serie sin fin de linea: si no llegan mas digitos, aplicar.
  if (num_modo && millis() - num_t_ms > 300) cerrarNumero();
}

// ==========================================================================
void setup() {
  Serial.begin(115200);
  if (SOY_TX) {
    // Primero el latch a 0 y DESPUES habilitar el driver: al reves, el pin
    // sale a lo que el latch tuviera (no esta definido tras un reset) y un 1
    // en CLK seria un flanco de subida espurio que el receptor tomaria por un
    // bit en cada arranque del transmisor.
    GPIO.out_w1tc = MASK_TX_DATA | MASK_TX_CLK;   // reposo: DATA=0, CLK=0
    pinMode(TX_DATA, OUTPUT);
    pinMode(TX_CLK, OUTPUT);
  } else {
    pinMode(RX_DATA, INPUT_PULLDOWN);
    pinMode(RX_CLK, INPUT_PULLDOWN);
    rxVolverAHunt();
    attachInterrupt(RX_CLK, isrReloj, RISING);
  }
  Serial.println(SOY_TX ? "# N3 ROL_A (transmisor). 't' inicia, +/-/b<N> velocidad"
                        : "# N3 ROL_B (receptor). 't' pone a cero, 'r' reporte");
  aplicarVelocidad(BIT_RATE_HZ);
}

void loop() {
  atenderSerie();
  if (SOY_TX) txLoop(); else rxLoop();
}

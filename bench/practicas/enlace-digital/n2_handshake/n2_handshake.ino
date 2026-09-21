/*
 * N2 - HANDSHAKE bidireccional y medicion de round-trip (ESP32 WROOM)
 *
 * UN solo sketch para las dos placas: elige el rol con UNA de las dos lineas
 * de mas abajo (#define ROL_A o #define ROL_B).
 *
 *   ROL_A = INICIADOR  : sube TX_DATA, mide cuanto tarda en volver la respuesta
 *                        (round-trip, RTT), baja TX_DATA, espera que la
 *                        respuesta tambien baje, y repite.
 *   ROL_B = RESPONDEDOR: copia en su salida, lo mas rapido que puede, el nivel
 *                        que ve en su entrada (dentro de una interrupcion).
 *
 * CABLEADO CRUZADO (cuatro cables contando GND, 330 ohm en serie en las senales):
 *
 *      Placa A                       Placa B
 *      GPIO26 (TX_DATA) --330R--> GPIO25 (RX_DATA)
 *      GPIO25 (RX_DATA) <--330R-- GPIO26 (TX_DATA)
 *      GND ---------------------------- GND            <-- OBLIGATORIO
 *
 * Reglas que NO se negocian:
 *  - GND comun entre placas: sin referencia comun un "1" no significa nada.
 *  - Nunca unir dos SALIDAS entre si (26 con 26): si una manda 1 y la otra 0
 *    es un cortocircuito. Aqui cada salida va a una ENTRADA (26 -> 25).
 *  - GPIO12 esta PROHIBIDO (strapping/MTDI, puede impedir el arranque).
 *  - RX_DATA usa INPUT_PULLDOWN: una entrada flotante NO es un 0.
 *
 * Como se mide (para que se entienda que significa cada numero):
 *  El RTT incluye: escribir el pin en A + latencia de la interrupcion en B +
 *  escribir el pin en B + latencia de la interrupcion en A + el filtro RC que
 *  forma la resistencia de 330 ohm con la capacidad del cable/pin. No es un
 *  retardo de "un cable": es un retardo de sistema. Las marcas de tiempo del
 *  flanco de vuelta se toman DENTRO de la ISR (esp_timer_get_time(), que es el
 *  mismo reloj de 1 us que usa micros()), no cuando loop() se entera: asi no
 *  se mide el tiempo que tarda el sketch en darse cuenta.
 *
 * Serie 115200. Salida CSV:  seq,rtt_us,timeouts
 *   seq = numero de intento (empieza en 0); rtt_us = -1 si el intento se
 *   perdio por timeout; timeouts = acumulado. Las lineas '#' son comentarios.
 */

// ---- ELIGE EL ROL (exactamente uno) ---------------------------------------
#define ROL_A
// #define ROL_B
// ---------------------------------------------------------------------------

#if defined(ROL_A) && defined(ROL_B)
#error "Define SOLO UNO: ROL_A o ROL_B, no los dos."
#endif
#if !defined(ROL_A) && !defined(ROL_B)
#error "Falta el rol: define ROL_A (iniciador) o ROL_B (respondedor)."
#endif

#include <Arduino.h>
#include "esp_timer.h"
#include "soc/gpio_struct.h"

#define TX_DATA 26
#define RX_DATA 25
#define TIMEOUT_US 50000        // si la respuesta no llega en este tiempo, se cuenta como perdida
#define N_INTERCAMBIOS 1000     // intercambios exitosos por tanda
#define N_CUBETAS 10

// Escritura directa a registro (GPIO.out_w1ts / out_w1tc): un solo acceso
// de bus, sin la logica de digitalWrite(). Solo vale para pines < 32.
static_assert(TX_DATA < 32 && RX_DATA < 32, "el acceso directo a registro exige pines < 32");
static const uint32_t MASK_TX = 1UL << TX_DATA;
static const uint32_t MASK_RX = 1UL << RX_DATA;

#ifdef ROL_A
// ===========================================================================
//  ROL A - INICIADOR
// ===========================================================================

// Compartidas con la ISR: volatile, y la ISR no imprime ni reserva memoria.
static volatile bool g_subida = false;
static volatile bool g_bajada = false;
static volatile uint32_t g_t_subida_us = 0;
static volatile uint32_t g_t_bajada_us = 0;

static uint32_t rtt_us[N_INTERCAMBIOS];    // 4 KB: cabe de sobra y permite histograma
static uint32_t n_ok = 0;
static uint32_t timeouts = 0;
static uint32_t seq = 0;
static uint32_t timeouts_seguidos = 0;
static bool terminado = false;

void IRAM_ATTR isrRespuesta() {
  uint32_t ahora = (uint32_t)esp_timer_get_time();
  if (GPIO.in & MASK_RX) {
    g_t_subida_us = ahora;
    g_subida = true;
  } else {
    g_t_bajada_us = ahora;
    g_bajada = true;
  }
}

static bool esperarLineaBaja() {
  uint32_t t = micros();
  while (GPIO.in & MASK_RX) {
    if (micros() - t > TIMEOUT_US) return false;
  }
  return true;
}

// La linea de vuelta debe estar en 0 antes de empezar un intercambio. Si se
// quedo en 1 (B perdio un flanco y, al copiar solo cambios, no se corrige
// solo), se le dan flancos nuevos: un pulso completo hace que B vuelva a
// copiar el nivel de su entrada.
static bool esperarReposo() {
  if (esperarLineaBaja()) return true;
  GPIO.out_w1ts = MASK_TX;
  delayMicroseconds(200);
  GPIO.out_w1tc = MASK_TX;
  return esperarLineaBaja();
}

// Un intercambio completo. Devuelve true y el RTT de subida si todo salio bien.
static bool intercambio(uint32_t &rtt) {
  if (!esperarReposo()) return false;

  g_subida = false;
  g_bajada = false;

  uint32_t t0 = micros();          // ANTES de escribir: incluye el coste de la propia escritura
  GPIO.out_w1ts = MASK_TX;         // TX = 1
  while (!g_subida) {
    if (micros() - t0 > TIMEOUT_US) {
      GPIO.out_w1tc = MASK_TX;     // abortar: devolver la linea a reposo
      return false;
    }
  }
  rtt = g_t_subida_us - t0;        // resta en uint32: correcta aunque el contador de 32 bits desborde

  uint32_t t1 = micros();
  GPIO.out_w1tc = MASK_TX;         // TX = 0: ahora el respondedor debe bajar tambien
  while (!g_bajada) {
    if (micros() - t1 > TIMEOUT_US) return false;
  }
  return true;
}

static void imprimirEstadisticas() {
  uint64_t suma = 0;
  uint32_t mn = rtt_us[0], mx = rtt_us[0];
  for (uint32_t i = 0; i < n_ok; i++) {
    suma += rtt_us[i];
    if (rtt_us[i] < mn) mn = rtt_us[i];
    if (rtt_us[i] > mx) mx = rtt_us[i];
  }
  double media = (double)suma / n_ok;
  // Dos pasadas (media y luego suma de cuadrados): es lo mas claro y con 1000
  // datos en un array no hace falta el algoritmo de Welford de una sola pasada.
  double acc = 0.0;
  for (uint32_t i = 0; i < n_ok; i++) {
    double d = (double)rtt_us[i] - media;
    acc += d * d;
  }
  double desv = (n_ok > 1) ? sqrt(acc / (n_ok - 1)) : 0.0;

  Serial.println("# ---- estadisticas RTT (us) ----");
  Serial.println("# n,min,max,media,desv_est,timeouts");
  Serial.printf("# %u,%u,%u,%.2f,%.2f,%u\n", (unsigned)n_ok, (unsigned)mn, (unsigned)mx,
                media, desv, (unsigned)timeouts);

  // Histograma: N_CUBETAS cubetas de igual ancho entre min y max. Un solo
  // valor atipico (p. ej. una interrupcion de la UART) estira el rango y
  // amontona el resto en la primera cubeta: eso tambien es informacion.
  uint32_t cuentas[N_CUBETAS] = {0};
  uint32_t rango = mx - mn + 1;
  uint32_t ancho = (rango + N_CUBETAS - 1) / N_CUBETAS;   // redondeo hacia arriba
  if (ancho == 0) ancho = 1;
  uint32_t mayor = 1;
  for (uint32_t i = 0; i < n_ok; i++) {
    uint32_t c = (rtt_us[i] - mn) / ancho;
    if (c >= N_CUBETAS) c = N_CUBETAS - 1;
    cuentas[c]++;
    if (cuentas[c] > mayor) mayor = cuentas[c];
  }
  Serial.println("# histograma: cubeta,desde_us,hasta_us,cuenta");
  for (uint32_t c = 0; c < N_CUBETAS; c++) {
    uint32_t desde = mn + c * ancho;
    uint32_t hasta = desde + ancho - 1;
    Serial.printf("# %u,%u,%u,%u  ", (unsigned)c, (unsigned)desde, (unsigned)hasta,
                  (unsigned)cuentas[c]);
    uint32_t barra = (cuentas[c] * 40 + mayor - 1) / mayor;   // barra ASCII de hasta 40 '*'
    for (uint32_t k = 0; k < barra; k++) Serial.print('*');
    Serial.println();
  }
  Serial.println("# Fin. Envia 'r' por serie para repetir la tanda.");
}

static void reiniciarTanda() {
  n_ok = 0;
  timeouts = 0;
  seq = 0;
  timeouts_seguidos = 0;
  terminado = false;
  Serial.println("seq,rtt_us,timeouts");
}

void setup() {
  Serial.begin(115200);
  pinMode(TX_DATA, OUTPUT);
  pinMode(RX_DATA, INPUT_PULLDOWN);
  GPIO.out_w1tc = MASK_TX;
  attachInterrupt(RX_DATA, isrRespuesta, CHANGE);
  Serial.println("# N2 ROL_A (iniciador)");
  reiniciarTanda();
}

void loop() {
  if (terminado) {
    if (Serial.available() && Serial.read() == 'r') reiniciarTanda();
    return;
  }

  uint32_t rtt;
  if (intercambio(rtt)) {
    rtt_us[n_ok++] = rtt;
    timeouts_seguidos = 0;
    Serial.printf("%u,%u,%u\n", (unsigned)seq, (unsigned)rtt, (unsigned)timeouts);
  } else {
    timeouts++;
    timeouts_seguidos++;
    Serial.printf("%u,-1,%u\n", (unsigned)seq, (unsigned)timeouts);
    if (timeouts_seguidos % 100 == 0) {
      Serial.println("# 100 intentos seguidos sin respuesta: revisa cableado cruzado, GND y que B tenga ROL_B");
    }
    // Tras un timeout, dejar pasar un rato: si la respuesta llegaba tarde, que
    // no contamine el siguiente intento.
    delay(20);
  }
  seq++;
  // Esperar a que la UART termine de sacar la linea: si no, sus interrupciones
  // caerian dentro del intercambio siguiente y ensancharian el RTT medido.
  Serial.flush();

  if (n_ok >= N_INTERCAMBIOS) {
    imprimirEstadisticas();
    terminado = true;
  }
}

#else
// ===========================================================================
//  ROL B - RESPONDEDOR
// ===========================================================================

static volatile uint32_t g_flancos = 0;
static uint32_t t_informe = 0;

// La ISR hace lo minimo: leer RX y copiarlo a TX con un acceso a registro.
// Sin Serial, sin malloc, sin digitalWrite (no son seguros / son lentos en ISR).
void IRAM_ATTR isrEco() {
  if (GPIO.in & MASK_RX) {
    GPIO.out_w1ts = MASK_TX;
  } else {
    GPIO.out_w1tc = MASK_TX;
  }
  g_flancos = g_flancos + 1;
}

void setup() {
  Serial.begin(115200);
  pinMode(TX_DATA, OUTPUT);
  pinMode(RX_DATA, INPUT_PULLDOWN);
  if (GPIO.in & MASK_RX) GPIO.out_w1ts = MASK_TX; else GPIO.out_w1tc = MASK_TX;
  attachInterrupt(RX_DATA, isrEco, CHANGE);
  Serial.println("# N2 ROL_B (respondedor)");
  t_informe = millis();
}

void loop() {
  if (millis() - t_informe >= 5000) {
    t_informe += 5000;
    Serial.printf("# B flancos_atendidos=%u\n", (unsigned)g_flancos);
  }
}
#endif

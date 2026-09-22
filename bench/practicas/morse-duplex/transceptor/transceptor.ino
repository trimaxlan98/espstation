/*
 * MORSE DUPLEX - TRANSCEPTOR (ESP32 WROOM, esp32:esp32:esp32)
 *
 * EL MISMO SKETCH EN LAS DOS PLACAS. No hay placa A ni placa B, no hay roles:
 * cada placa tiene su llave y decodifica lo que llega de la otra. Lo que cambia
 * respecto a ../clave-morse/ no es el codigo de cada mitad, es que ahora las dos
 * mitades conviven en la misma placa y el cable va CRUZADO en los dos sentidos.
 *
 * CABLEADO (cruzado, como un null-modem serie):
 *      P1 GPIO26 (TX_DATA) --330R--> P2 GPIO25 (RX_DATA)
 *      P2 GPIO26 (TX_DATA) --330R--> P1 GPIO25 (RX_DATA)
 *      P1 GND ---------------------- P2 GND            <-- OBLIGATORIO
 *      LLAVE (en cada placa): punta de jumper pelada en GPIO13 (KEY_PIN)
 *                             otra punta pelada a 3V3
 *      LED testigo TX: GPIO4  -> 330 ohm -> LED -> GND  (mi llave)
 *      LED testigo RX: GPIO16 -> 330 ohm -> LED -> GND  (la llave del otro)
 *
 * ===========================================================================
 *  PELIGRO SI LAS DOS PLACAS ESTAN EN ORDENADORES DISTINTOS
 * ===========================================================================
 *  El puente de GND une las masas de los DOS ordenadores. Si estan en enchufes
 *  distintos puede haber tension entre ellas (las fuentes conmutadas con toma de
 *  tierra flotante dejan tension de fuga a traves de sus condensadores Y: poca
 *  corriente, pero suficiente para matar un puerto USB o un GPIO). El jumper de
 *  GND es un cortocircuito para esa diferencia y toda la corriente pasa por ahi.
 *  ANTES de unir las masas hay que MEDIRLO: ver README.md, "Enlazar dos
 *  ordenadores". Con las dos placas en el MISMO ordenador el problema no existe.
 *
 * LA LLAVE ES METAL CONTRA METAL, NO EL DEDO: dos puntas peladas que se
 * presionan una contra otra (~0 ohm). Cerrando con la piel, el dedo es una
 * resistencia variable (1 kohm mojado a >500 kohm seco) en un divisor contra el
 * pull-down interno (~45 kohm) y el nivel cae en la zona indefinida.
 *
 * DOS DECODIFICADORES, UMBRALES INDEPENDIENTES. El mismo codigo corre dos veces:
 *   RX = lo que llega por el cable, o sea la mano del OTRO operador;
 *   TX = eco local de mi propia llave, o sea MI mano.
 * Los umbrales de RX hay que ajustarlos al pulso del OTRO. Es el punto raro de
 * esta practica: el umbral pertenece a la mano que teclea pero vive en la placa
 * que escucha. Por eso son dos juegos de umbrales y no uno.
 *
 * GPIO12 esta PROHIBIDO (pin de strapping). GPIO16/17 estan libres en WROOM; en
 * modulos WROVER los usa la PSRAM y habria que mover LED_RX.
 *
 * Serie: 115200 baudios. Contrato completo en SPEC-DUPLEX.md.
 * Salida: cada linea de simbolo/letra va prefijada por "RX " o "TX ".
 * Comandos: p/l/w/d umbrales de RX, tp/tl/tw/td los de TX (eco), k antirrebote
 * de la llave, e eco local on/off, v verbose, c contadores a cero, r estado.
 */

#include <Arduino.h>
#include "soc/gpio_struct.h"      // registro GPIO.in, como en clave-morse y N2/N3

#define KEY_PIN 13        // mi llave, INPUT_PULLDOWN; el otro extremo a 3V3
#define TX_DATA 26        // mi salida -> RX_DATA de la otra placa
#define RX_DATA 25        // mi entrada <- TX_DATA de la otra placa
#define LED_TX 4          // testigo: mi llave (ya filtrada)
#define LED_RX 16         // testigo: nivel crudo recibido del otro
#define MASK_RX (1UL << RX_DATA)   // GPIO25 < 32: se lee de GPIO.in

#define N_FLANCOS 64      // buffer circular ISR -> loop
#define MAX_SIMBOLO 8     // puntos/rayas maximos por letra
#define PERIODO_RESUMEN_MS 5000
#define DEBOUNCE_MAX_MS 200

// --- Tabla Morse: datos, no una cadena de if/else ----------------------------
struct Morse { const char *codigo; char c; };
static const Morse TABLA[] = {
  {".-", 'A'},   {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'},  {".", 'E'},
  {"..-.", 'F'}, {"--.", 'G'},  {"....", 'H'}, {"..", 'I'},   {".---", 'J'},
  {"-.-", 'K'},  {".-..", 'L'}, {"--", 'M'},   {"-.", 'N'},   {"---", 'O'},
  {".--.", 'P'}, {"--.-", 'Q'}, {".-.", 'R'},  {"...", 'S'},  {"-", 'T'},
  {"..-", 'U'},  {"...-", 'V'}, {".--", 'W'},  {"-..-", 'X'}, {"-.--", 'Y'},
  {"--..", 'Z'},
  {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'}, {"....-", '4'},
  {".....", '5'}, {"-....", '6'}, {"--...", '7'}, {"---..", '8'}, {"----.", '9'},
};
#define N_TABLA (sizeof(TABLA) / sizeof(TABLA[0]))

// --- Un decodificador. Hay dos instancias: rx (el otro) y tx (eco de mi llave).
// Mismo codigo las dos veces: si la decodificacion fuera distinta en cada sentido
// no se podria comparar lo que envie con lo que el otro recibio.
struct Dec {
  const char *et = "??";                 // etiqueta que prefija cada linea
  uint32_t punto_raya_ms = 300;          // < esto = punto, >= esto = raya
  uint32_t letra_ms = 700;               // silencio = fin de letra
  uint32_t palabra_ms = 1800;            // silencio = fin de palabra
  uint32_t debounce_ms = 15;             // pulsos mas cortos = ruido
  uint8_t nivel_ant = 0;                 // ultimo nivel ya procesado
  bool en_pulso = false;                 // hubo subida y aun no su bajada
  bool hay_bajada = false;               // t_bajada_us es valido
  bool midiendo_silencio = false;        // falta decidir letra o palabra
  bool desbordado = false;               // mas de MAX_SIMBOLO pulsos
  bool letra_desde_palabra = false;      // hubo letra desde el ultimo [palabra]
  uint32_t t_subida_us = 0, t_bajada_us = 0;
  char simbolo[MAX_SIMBOLO + 1] = {0};
  uint8_t n_simbolo = 0;
  uint32_t filtrados = 0, puntos = 0, rayas = 0, letras = 0;
  uint32_t desconocidas = 0, repetidos = 0;
};
static Dec rx, tx;

static bool eco_local = true;    // decodificar tambien mi propia llave
static bool verbose = false;

// --- Compartido con la ISR: volatile; la ISR no imprime ni reserva memoria ---
struct Flanco { uint32_t t_us; uint8_t nivel; };
static volatile Flanco g_buf[N_FLANCOS];
static volatile uint8_t g_head = 0;          // lo escribe SOLO la ISR
static volatile uint8_t g_tail = 0;          // lo escribe SOLO loop()
static volatile uint32_t g_crudos = 0;       // flancos que entraron en la ISR
static volatile uint32_t g_desbordes = 0;    // flancos perdidos por buffer lleno

void IRAM_ATTR isrFlanco() {
  uint32_t t = micros();                               // sellar el tiempo primero
  uint8_t nivel = (GPIO.in & MASK_RX) ? 1 : 0;
  g_crudos = g_crudos + 1;   // (++ sobre volatile esta obsoleto y avisa)
  uint8_t sig = (uint8_t)((g_head + 1) % N_FLANCOS);
  if (sig == g_tail) {                                 // lleno: contar y descartar
    g_desbordes = g_desbordes + 1;
    return;
  }
  g_buf[g_head].t_us = t;
  g_buf[g_head].nivel = nivel;
  g_head = sig;
}

// --- Llave propia (antirrebote) ----------------------------------------------
static uint32_t llave_debounce_ms = 15;
static uint8_t llave_estable = 0;      // nivel ya aceptado (el que sale por TX_DATA)
static uint8_t llave_candidato = 0;    // ultima lectura cruda del pin
static uint32_t t_candidato = 0;       // desde cuando la lectura vale 'candidato'
static uint32_t llave_crudos = 0;      // cambios de lectura ANTES del filtro
static uint32_t llave_aceptados = 0;   // flancos que pasaron el filtro
static uint32_t t_resumen = 0;
static uint32_t resumen_crudos = 0, resumen_aceptados = 0, resumen_rx = 0;

static char linea[16];
static uint8_t n_linea = 0;

static void imprimirBin8(uint8_t v) {
  // 8 bits SIEMPRE: Serial.print(c, BIN) no rellena ('A' saldria de 7 bits).
  for (int i = 7; i >= 0; i--) Serial.print((v >> i) & 1);
}

static void imprimirUmbrales(const Dec &d) {
  Serial.print("# umbrales ");
  Serial.print(d.et);
  Serial.print(" punto_raya_ms=");
  Serial.print(d.punto_raya_ms);
  Serial.print(" letra_ms=");
  Serial.print(d.letra_ms);
  Serial.print(" palabra_ms=");
  Serial.print(d.palabra_ms);
  Serial.print(" debounce_ms=");
  Serial.println(d.debounce_ms);
}

static void imprimirContadores(const Dec &d, bool con_isr) {
  Serial.print("# contadores ");
  Serial.print(d.et);
  Serial.print(" puntos=");
  Serial.print(d.puntos);
  Serial.print(" rayas=");
  Serial.print(d.rayas);
  Serial.print(" letras=");
  Serial.print(d.letras);
  Serial.print(" desconocidas=");
  Serial.print(d.desconocidas);
  Serial.print(" filtrados=");
  Serial.print(d.filtrados);
  Serial.print(" niveles_repetidos=");
  Serial.print(d.repetidos);
  if (con_isr) {                 // solo RX tiene una ISR detras
    Serial.print(" flancos_crudos=");
    Serial.print(g_crudos);
    Serial.print(" desbordes_buffer=");
    Serial.print(g_desbordes);
  }
  Serial.println();
}

static void imprimirEstado() {
  imprimirUmbrales(rx);
  imprimirUmbrales(tx);
  imprimirContadores(rx, true);
  imprimirContadores(tx, false);
  Serial.print("# llave debounce_ms=");
  Serial.print(llave_debounce_ms);
  Serial.print(" crudos=");
  Serial.print(llave_crudos);
  Serial.print(" aceptados=");
  Serial.println(llave_aceptados);
  Serial.print("# modo eco_local=");
  Serial.print(eco_local ? 1 : 0);
  Serial.print(" verbose=");
  Serial.println(verbose ? 1 : 0);
}

static void cerrarLetra(Dec &d) {
  d.simbolo[d.n_simbolo] = '\0';
  char c = '?';
  bool hallada = false;
  for (size_t i = 0; i < N_TABLA; i++) {
    if (strcmp(TABLA[i].codigo, d.simbolo) == 0) {
      c = TABLA[i].c;
      hallada = true;
      break;
    }
  }
  Serial.print(d.et);
  Serial.print(' ');
  if (hallada && !d.desbordado) {
    d.letras++;
    Serial.print("[letra: ");
    Serial.print(c);
    Serial.print("] [bin: ");
    imprimirBin8((uint8_t)c);
    Serial.println("]");
  } else {
    d.desconocidas++;
    Serial.print("[letra: ?] [morse: ");
    Serial.print(d.simbolo);
    Serial.println(d.desbordado ? "+]" : "]");
  }
  d.n_simbolo = 0;
  d.desbordado = false;
  d.letra_desde_palabra = true;
}

// Un flanco ya situado en el tiempo. La llaman el drenaje del buffer (RX) y el
// filtro de la llave (TX): los dos sentidos pasan por aqui, con el mismo codigo.
static void procesarFlanco(Dec &d, uint8_t nivel, uint32_t t_us) {
  if (nivel == d.nivel_ant) {   // la ISR lo leyo cuando la linea ya habia vuelto
    d.repetidos++;
    return;
  }
  d.nivel_ant = nivel;

  if (nivel) {                            // subida
    if (verbose && d.hay_bajada) {
      Serial.print("# ");
      Serial.print(d.et);
      Serial.print(" silencio_ms=");
      Serial.println((uint32_t)(t_us - d.t_bajada_us) / 1000UL);
    }
    d.t_subida_us = t_us;
    d.en_pulso = true;
    return;
  }

  // bajada
  if (!d.en_pulso) return;      // arranque con la llave ya cerrada: sin subida
  d.en_pulso = false;
  uint32_t dur_ms = (uint32_t)(t_us - d.t_subida_us) / 1000UL;

  if (dur_ms < d.debounce_ms) { // ruido: no es simbolo ni interrumpe el silencio
    d.filtrados++;
    if (verbose) {
      Serial.print("# ");
      Serial.print(d.et);
      Serial.print(" pulso_ms=");
      Serial.print(dur_ms);
      Serial.println(" filtrado");
    }
    return;
  }

  char s = (dur_ms < d.punto_raya_ms) ? '.' : '-';
  if (s == '.') d.puntos++; else d.rayas++;
  if (d.n_simbolo < MAX_SIMBOLO) d.simbolo[d.n_simbolo++] = s; else d.desbordado = true;
  Serial.print(d.et);           // feedback inmediato, al decidirlo
  Serial.print(' ');
  Serial.println(s);
  if (verbose) {
    Serial.print("# ");
    Serial.print(d.et);
    Serial.print(" pulso_ms=");
    Serial.println(dur_ms);
  }
  d.t_bajada_us = t_us;
  d.hay_bajada = true;
  d.midiendo_silencio = true;
}

static void revisarSilencio(Dec &d) {
  if (!d.midiendo_silencio || d.nivel_ant != 0) return;
  uint32_t sil_ms = (uint32_t)(micros() - d.t_bajada_us) / 1000UL;
  if (d.n_simbolo > 0 && sil_ms >= d.letra_ms) cerrarLetra(d);
  if (d.letra_desde_palabra && sil_ms >= d.palabra_ms) {
    Serial.print(d.et);
    Serial.println(" [palabra]");            // una sola vez por hueco
    d.letra_desde_palabra = false;
  }
  // Nada mas que esperar: dejar de medir (y no envolver micros() a los 71 min).
  if (d.n_simbolo == 0 && !d.letra_desde_palabra) d.midiendo_silencio = false;
}

static void ponerContadoresACero() {
  noInterrupts();
  g_crudos = 0;
  g_desbordes = 0;
  interrupts();
  rx.filtrados = rx.puntos = rx.rayas = rx.letras = rx.desconocidas = rx.repetidos = 0;
  tx.filtrados = tx.puntos = tx.rayas = tx.letras = tx.desconocidas = tx.repetidos = 0;
  llave_crudos = llave_aceptados = 0;
  resumen_crudos = resumen_aceptados = resumen_rx = 0;
}

// Con el eco apagado, atenderLlave() deja de alimentar al decodificador TX, que
// se queda congelado en el nivel que tuviera. Al reencenderlo puede estar DESFASADO
// respecto a la llave, y entonces la siguiente bajada mide contra un t_subida_us
// de hace minutos: sale una raya falsa de varios segundos y una letra inventada.
// Solo se resincroniza si de verdad se perdio algun flanco (los niveles no
// coinciden); si coinciden, no se toca nada y no se pierde ningun simbolo.
static void resincronizarEco() {
  if (tx.nivel_ant == llave_estable) return;   // no se perdio ningun flanco
  tx.nivel_ant = llave_estable;
  tx.en_pulso = false;                         // el pulso a medias ya no es medible
  tx.midiendo_silencio = false;
  tx.letra_desde_palabra = false;
  tx.n_simbolo = 0;                            // la letra a medias mezclaria dos manos
  tx.desbordado = false;
}

// <cmd><ms> necesita AL MENOS UNA CIFRA. Sin esto atol("") vale 0, que es un
// valor VALIDO para k y para d: un dedazo como "k" o "dx" apagaba el antirrebote
// en silencio, que es justo lo que el SPEC quiere que no pase inadvertido.
static bool argNumerico(const char *s) {
  if (*s == '-' || *s == '+') s++;
  if (!*s) return false;
  for (; *s; s++) if (*s < '0' || *s > '9') return false;
  return true;
}

static void ejecutarComando(const char *cmd) {
  // Primero los comandos globales: no llevan prefijo de sentido.
  switch (cmd[0]) {
    case 'r': imprimirEstado(); return;
    case 'c': ponerContadoresACero(); Serial.println("# contadores a cero"); return;
    case 'v': verbose = !verbose; imprimirEstado(); return;
    case 'e':
      eco_local = !eco_local;
      if (eco_local) resincronizarEco();       // al volver, ponerse al dia con la llave
      imprimirEstado();
      return;
    case 'k': {
      long vk = atol(cmd + 1);
      if (!argNumerico(cmd + 1) || vk < 0 || vk > DEBOUNCE_MAX_MS) {
        Serial.println("# rechazado: k<ms> admite 0..200");
      } else {
        llave_debounce_ms = (uint32_t)vk;
        imprimirEstado();
      }
      return;
    }
    default: break;
  }

  // Umbrales: sin prefijo van a RX (la mano del otro); con 't' delante, al eco.
  Dec *d = &rx;
  const char *p = cmd;
  if (p[0] == 't' && p[1] != '\0') { d = &tx; p++; }
  char k = p[0];
  long v = atol(p + 1);

  if (k == 'd') {
    if (!argNumerico(p + 1) || v < 0 || v > DEBOUNCE_MAX_MS) { Serial.println("# rechazado: d<ms> admite 0..200"); return; }
    d->debounce_ms = (uint32_t)v;
  } else if (k == 'p' || k == 'l' || k == 'w') {
    if (!argNumerico(p + 1) || v < 1 || v > 60000) { Serial.println("# rechazado: admite 1..60000 ms"); return; }
    if (k == 'l' && (uint32_t)v >= d->palabra_ms) {
      Serial.println("# rechazado: letra_ms debe ser menor que palabra_ms");
      return;
    }
    if (k == 'w' && (uint32_t)v <= d->letra_ms) {
      Serial.println("# rechazado: palabra_ms debe ser mayor que letra_ms");
      return;
    }
    if (k == 'p') d->punto_raya_ms = (uint32_t)v;
    if (k == 'l') d->letra_ms = (uint32_t)v;
    if (k == 'w') d->palabra_ms = (uint32_t)v;
  } else {
    Serial.print("# rechazado: comando desconocido ");
    Serial.println(cmd);
    return;
  }
  imprimirUmbrales(*d);
}

static void leerSerie() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (n_linea > 0) {
        linea[n_linea] = '\0';
        ejecutarComando(linea);
        n_linea = 0;
      }
    } else if (n_linea < sizeof(linea) - 1) {
      linea[n_linea++] = c;
    }
  }
}

// Lee la llave, la filtra y espeja el nivel al cable. El eco local recibe el
// MISMO flanco que sale por TX_DATA, no la lectura cruda: asi lo que decodifico
// de mi mano es exactamente lo que el otro puede decodificar.
static void atenderLlave(uint32_t ahora_ms) {
  uint8_t lectura = digitalRead(KEY_PIN);

  if (lectura != llave_candidato) {
    // Cambio de lectura (real o rebote): reinicia la cuenta de estabilidad.
    llave_candidato = lectura;
    t_candidato = ahora_ms;
    llave_crudos++;
  } else if (llave_candidato != llave_estable && ahora_ms - t_candidato >= llave_debounce_ms) {
    // Estable el tiempo suficiente: es un flanco de verdad. Primero al cable.
    llave_estable = llave_candidato;
    digitalWrite(TX_DATA, llave_estable);
    digitalWrite(LED_TX, llave_estable);
    llave_aceptados++;
    if (verbose) {
      Serial.print("# TX flanco t_ms=");
      Serial.print(ahora_ms);
      Serial.print(" nivel=");
      Serial.println(llave_estable);
    }
    // Los dos flancos salen retrasados el mismo debounce_ms, asi que la duracion
    // que ve el eco (y la que ve el otro) es la real.
    if (eco_local) procesarFlanco(tx, llave_estable, micros());
  }
}

void setup() {
  Serial.begin(115200);
  rx.et = "RX";
  tx.et = "TX";
  pinMode(KEY_PIN, INPUT_PULLDOWN);   // llave abierta = 0 definido, nunca flotante
  pinMode(RX_DATA, INPUT_PULLDOWN);   // sin cable: 0 estable, nunca flotante
  pinMode(TX_DATA, OUTPUT);
  pinMode(LED_TX, OUTPUT);
  pinMode(LED_RX, OUTPUT);
  digitalWrite(TX_DATA, LOW);
  digitalWrite(LED_TX, LOW);
  digitalWrite(LED_RX, LOW);

  llave_estable = llave_candidato = digitalRead(KEY_PIN);
  if (llave_estable) {                // arranco con la llave cerrada: reflejarlo
    digitalWrite(TX_DATA, HIGH);
    digitalWrite(LED_TX, HIGH);
  }
  tx.nivel_ant = llave_estable;
  rx.nivel_ant = (GPIO.in & MASK_RX) ? 1 : 0;
  digitalWrite(LED_RX, rx.nivel_ant);

  attachInterrupt(RX_DATA, isrFlanco, CHANGE);
  Serial.println("# transceptor morse listo (el mismo sketch en las dos placas)");
  imprimirEstado();
  t_candidato = millis();
  t_resumen = millis();
}

void loop() {
  // Drenar el buffer: la ISR solo escribe head, loop() solo escribe tail.
  while (g_tail != g_head) {
    uint8_t i = g_tail;
    uint32_t t_us = g_buf[i].t_us;
    uint8_t nivel = g_buf[i].nivel;
    g_tail = (uint8_t)((i + 1) % N_FLANCOS);
    digitalWrite(LED_RX, nivel);           // LED = nivel crudo recibido
    procesarFlanco(rx, nivel, t_us);
  }

  uint32_t ahora_ms = millis();
  atenderLlave(ahora_ms);
  revisarSilencio(rx);
  if (eco_local) revisarSilencio(tx);

  if (ahora_ms - t_resumen >= PERIODO_RESUMEN_MS) {
    t_resumen += PERIODO_RESUMEN_MS;
    if (llave_crudos != resumen_crudos || llave_aceptados != resumen_aceptados ||
        g_crudos != resumen_rx) {
      resumen_crudos = llave_crudos;
      resumen_aceptados = llave_aceptados;
      resumen_rx = g_crudos;
      Serial.print("# resumen t_ms=");
      Serial.print(ahora_ms);
      Serial.print(" llave_crudos=");
      Serial.print(llave_crudos);
      Serial.print(" llave_aceptados=");
      Serial.print(llave_aceptados);
      Serial.print(" rx_flancos_crudos=");
      Serial.print(g_crudos);
      Serial.print(" nivel_tx=");
      Serial.print(llave_estable);
      Serial.print(" nivel_rx=");
      Serial.println(rx.nivel_ant);
    }
  }

  leerSerie();
}
